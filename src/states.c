/*
 * Copyright (C) 2017-2019 CUJO LLC
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/idr.h>

#include <lualib.h>

#include "luautil.h"
#include "xt_lua.h"
#include "states.h"
#include "netlink.h"

#ifndef NFLUA_SETPAUSE
#define NFLUA_SETPAUSE	100
#endif /* NFLUA_SETPAUSE */

#define LUA_DATALIBNAME "data"
extern int luaopen_data(lua_State* L);

static inline int name_hash(void *salt, const char *name)
{
	return full_name_hash(salt, name, strnlen(name, XT_LUA_STATENAME_SIZE)) &
		(XT_LUA_HASH_BUCKETS - 1);
}

struct nflua_state *nflua_state_lookup(struct xt_lua_net *xt_lua,
		const char *name)
{
	struct hlist_head *head;
	struct nflua_state *state;

	if (xt_lua == NULL)
		return NULL;

	head = &xt_lua->state_table[name_hash(xt_lua, name)];
	hlist_for_each_entry_rcu(state, head, node) {
		if (!strncmp(state->name, name, XT_LUA_STATENAME_SIZE))
			return state;
	}
	return NULL;
}

static void state_destroy(struct xt_lua_net *xt_lua, struct nflua_state *s)
{
	hlist_del_rcu(&s->node);
	ida_simple_remove(&xt_lua->ida, s->id);

	spin_lock_bh(&s->lock);
	if (s->L != NULL) {
		lua_close(s->L);
		s->L = NULL;
	}
	spin_unlock_bh(&s->lock);

	/* FIXME: There's a possible race condition here if the nflua module is
	 * unloaded while a timer is being executed and waiting to acquire the lock.
	 */
	kfree(s);
}

static void *lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	struct nflua_state *s = ud;
	void *nptr = NULL;

	/* osize doesn't represent the object old size if ptr is NULL */
	osize = ptr != NULL ? osize : 0;

	if (nsize == 0) {
		s->curralloc -= osize;
		kfree(ptr);
	} else if (s->curralloc - osize + nsize > s->maxalloc) {
		pr_warn_ratelimited("maxalloc limit %zu reached on state %.*s\n",
		    s->maxalloc, NFLUA_NAME_MAXSIZE, s->name);
	} else if ((nptr = krealloc(ptr, nsize, GFP_ATOMIC)) != NULL) {
		s->curralloc += nsize - osize;
	}

	return nptr;
}

static int state_init(struct nflua_state *s)
{
	s->L = lua_newstate(lua_alloc, s);
	if (s->L == NULL)
		return -ENOMEM;

	luaU_setenv(s->L, s, struct nflua_state);
	luaL_openlibs(s->L);

	luaL_requiref(s->L, LUA_DATALIBNAME, luaopen_data, 1);
	lua_pop(s->L, 1);

	/* fixes an issue where the Lua's GC enters a vicious cycle.
	 * more info here: https://marc.info/?l=lua-l&m=155024035605499&w=2
	 */
	lua_gc(s->L, LUA_GCSETPAUSE, NFLUA_SETPAUSE);

	return 0;
}

struct nflua_state *nflua_state_create(struct xt_lua_net *xt_lua,
	size_t maxalloc, const char *name)
{
	struct hlist_head *head;
	struct nflua_state *s = nflua_state_lookup(xt_lua, name);
	int namelen = strnlen(name, NFLUA_NAME_MAXSIZE);
	int id;

	pr_debug("creating state: %.*s maxalloc: %zd\n", namelen, name,
		maxalloc);

	if (s != NULL) {
		pr_err("state already exists: %.*s\n", namelen, name);
		return NULL;
	}

	id = ida_simple_get(&xt_lua->ida, 1, NFLUA_MAX_STATES + 1, GFP_KERNEL);
	if (id < 0) {
		pr_err("could not allocate id for state %.*s\n", namelen, name);
		pr_err("max states limit reached or out of memory\n");
		return NULL;
	}

	if (maxalloc < NFLUA_MIN_ALLOC_BYTES) {
		pr_err("maxalloc %zu should be greater then MIN_ALLOC %zu\n",
		    maxalloc, NFLUA_MIN_ALLOC_BYTES);
		return NULL;
	}

	if ((s = kzalloc(sizeof(struct nflua_state), GFP_ATOMIC)) == NULL) {
		pr_err("could not allocate nflua state\n");
		goto err;
	}

	INIT_HLIST_NODE(&s->node);
	spin_lock_init(&s->lock);
	s->id        = id;
	s->dseqnum   = 0;
	s->maxalloc  = maxalloc;
	s->curralloc = 0;
	s->sock      = xt_lua->sock;
	memcpy(&(s->name), name, namelen);

	if (state_init(s)) {
		pr_err("could not allocate a new lua state\n");
		kfree(s);
		goto err;
	}

	spin_lock_bh(&xt_lua->state_lock);
	head = &xt_lua->state_table[name_hash(xt_lua, name)];
	hlist_add_head_rcu(&s->node, head);
	nflua_state_get(s);
	spin_unlock_bh(&xt_lua->state_lock);

	pr_debug("new state created with id: %d\n", id);
	return s;

err:
	ida_simple_remove(&xt_lua->ida, id);
	return NULL;
}

int nflua_state_destroy(struct xt_lua_net *xt_lua, const char *name)
{
	struct nflua_state *s = nflua_state_lookup(xt_lua, name);

	if (s == NULL || refcount_read(&s->users) > 1)
		return -1;

	spin_lock_bh(&xt_lua->state_lock);
	state_destroy(xt_lua, s);
	spin_unlock_bh(&xt_lua->state_lock);

	return 0;
}

static int count_states(struct xt_lua_net *xt_lua)
{
	struct hlist_head *head;
	struct nflua_state *s;
	int i, count = 0;
	for (i = 0; i < XT_LUA_HASH_BUCKETS; i++) {
		head = &xt_lua->state_table[i];
		hlist_for_each_entry_rcu(s, head, node) {
			count++;
		}
	}
	return count;
}

int nflua_state_list(struct xt_lua_net *xt_lua, nflua_state_cb cb, void *data)
{
	struct hlist_head *head;
	struct nflua_state *s;
	int i, total, ret = 0;

	spin_lock_bh(&xt_lua->state_lock);
	total = count_states(xt_lua);
	for (i = 0; i < XT_LUA_HASH_BUCKETS; i++) {
		head = &xt_lua->state_table[i];
		hlist_for_each_entry_rcu(s, head, node) {
			if ((ret = cb(s, total, data)) != 0)
				goto out;
		}
	}

out:
	spin_unlock_bh(&xt_lua->state_lock);
	return ret;
}

void nflua_state_destroy_all(struct xt_lua_net *xt_lua)
{
	struct hlist_head *head;
	struct hlist_node *tmp;
	struct nflua_state *s;
	int i;

	spin_lock_bh(&xt_lua->state_lock);
	for (i = 0; i < XT_LUA_HASH_BUCKETS; i++) {
		head = &xt_lua->state_table[i];
		hlist_for_each_entry_safe(s, tmp, head, node) {
			state_destroy(xt_lua, s);
		}
	}
	spin_unlock_bh(&xt_lua->state_lock);
}

void nflua_states_init(struct xt_lua_net *xt_lua)
{
	int i;
	ida_init(&xt_lua->ida);
	spin_lock_init(&xt_lua->state_lock);
	for (i = 0; i < XT_LUA_HASH_BUCKETS; i++)
		INIT_HLIST_HEAD(&xt_lua->state_table[i]);
}

void nflua_states_exit(struct xt_lua_net *xt_lua)
{
	nflua_state_destroy_all(xt_lua);
}
