/*
 * Copyright (c) 2009-2012, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "lua_common.h"
#include "map.h"
#include "message.h"
#include "radix.h"
#include "expression.h"
#include "utlist.h"

/***
 * This module is used to configure rspamd and is normally available as global
 * variable named `rspamd_config`. Unlike other modules, it is not necessary to
 * require it before usage.
 * @module rspamd_config
 * @example
-- Register some callback symbol
local function foo(task)
    -- do something
end
rspamd_config:register_symbol('SYMBOL', 1.0, foo)

-- Get configuration
local tab = rspamd_config:get_all_opt('module') -- get table for module's options
local opts = rspamd_config:get_key('options') -- get content of the specified key in rspamd configuration
 */

/* Config file methods */
/***
 * @method rspamd_config:get_module_opt(mname, optname)
 * Returns value of specified option `optname` for a module `mname`,
 * @param {string} mname name of module
 * @param {string} optname option to get
 * @return {string or table} value of the option or `nil` if option is not found
 */
LUA_FUNCTION_DEF (config, get_module_opt);
/***
 * @method rspamd_config:get_all_opt(mname)
 * Returns value of all options for a module `mname`, flattening values into a single table consisting
 * of all sections with such a name.
 * @param {string} mname name of module
 * @return {table} table of all options for `mname` or `nil` if a module's configuration is not found
 */
LUA_FUNCTION_DEF (config, get_all_opt);
/***
 * @method rspamd_config:get_mempool()
 * Returns static configuration memory pool.
 * @return {mempool} [memory pool](mempool.md) object
 */
LUA_FUNCTION_DEF (config, get_mempool);
/***
 * @method rspamd_config:add_radix_map(mapline[, description])
 * Creates new dynamic map of IP/mask addresses.
 * @param {string} mapline URL for a map
 * @param {string} description optional map description
 * @return {radix} radix tree object
 * @example
local ip_map = rspamd_config:add_radix_map ('file:///path/to/file', 'my radix map')
...
local function foo(task)
	local ip = task:get_from_ip()
	if ip_map:get_key(ip) then
		return true
	end
	return false
end
 */
LUA_FUNCTION_DEF (config, add_radix_map);
/***
 * @method rspamd_config:radix_from_config(mname, optname)
 * Creates new static map of IP/mask addresses from config.
 * @param {string} mname name of module
 * @param {string} optname option to get
 * @return {radix} radix tree object
 * @example
local ip_map = rspamd_config:radix_from_config ('mymodule', 'ips')
...
local function foo(task)
	local ip = task:get_from_ip()
	if ip_map:get_key(ip) then
		return true
	end
	return false
end
 */
LUA_FUNCTION_DEF (config, radix_from_config);
/***
 * @method rspamd_config:add_hash_map(mapline[, description])
 * Creates new dynamic map string objects.
 * @param {string} mapline URL for a map
 * @param {string} description optional map description
 * @return {hash} hash set object
 * @example
local hash_map = rspamd_config:add_hash_map ('file:///path/to/file', 'my hash map')
...
local function foo(task)
	local from = task:get_from()
	if hash_map:get_key(from['user']) then
		return true
	end
	return false
end
 */
LUA_FUNCTION_DEF (config, add_hash_map);
/***
 * @method rspamd_config:add_kv_map(mapline[, description])
 * Creates new dynamic map of key/values associations.
 * @param {string} mapline URL for a map
 * @param {string} description optional map description
 * @return {hash} hash table object
 * @example
local kv_map = rspamd_config:add_kv_map ('file:///path/to/file', 'my kv map')
...
local function foo(task)
	local from = task:get_from()
	if from then
		local value = kv_map:get_key(from['user'])
		if value then
			return true,value
		end
	end
	return false
end
 */
LUA_FUNCTION_DEF (config, add_kv_map);
/***
 * @method rspamd_config:add_map(mapline[, description], callback)
 * Creates new dynamic map with free-form callback
 * @param {string} mapline URL for a map
 * @param {string} description optional map description
 * @param {function} callback function to be called on map load and/or update
 * @return {bool} `true` if map has been added
 * @example

local str = ''
local function process_map(in)
	str = in
end

rspamd_config:add_map('http://example.com/map', "settings map", process_map)
 */
LUA_FUNCTION_DEF (config, add_map);
/***
 * @method rspamd_config:get_classifier(name)
 * Returns classifier config.
 * @param {string} name name of classifier (e.g. `bayes`)
 * @return {classifier} classifier object or `nil`
 */
LUA_FUNCTION_DEF (config, get_classifier);
/***
 * @method rspamd_config:register_symbol(name, weight, callback)
 * Register callback function to be called for a specified symbol with initial weight.
 * @param {string} name symbol's name
 * @param {number} weight initial weight of symbol (can be less than zero to specify non-spam symbols)
 * @param {function} callback callback function to be called for a specified symbol
 */
LUA_FUNCTION_DEF (config, register_symbol);
/***
 * @method rspamd_config:register_symbols(callback, [weight], callback_name, [, symbol, ...])
 * Register callback function to be called for a set of symbols with initial weight.
 * @param {function} callback callback function to be called for a specified symbol
 * @param {number} weight initial weight of symbol (can be less than zero to specify non-spam symbols)
 * @param {string} callback_name symbolic name of callback
 * @param {list of strings} symbol list of symbols registered by this function
 */
LUA_FUNCTION_DEF (config, register_symbols);
/***
 * @method rspamd_config:register_virtual_symbol(name, weight,)
 * Register virtual symbol that is not associated with any callback.
 * @param {string} virtual name symbol's name
 * @param {number} weight initial weight of symbol (can be less than zero to specify non-spam symbols)
 */
LUA_FUNCTION_DEF (config, register_virtual_symbol);
/***
 * @method rspamd_config:register_callback_symbol(name, weight, callback)
 * Register callback function to be called for a specified symbol with initial weight. Symbol itself is
 * not registered in the metric and is not intended to be visible by a user.
 * @param {string} name symbol's name (just for unique id purposes)
 * @param {number} weight initial weight of symbol (can be less than zero to specify non-spam symbols)
 * @param {function} callback callback function to be called for a specified symbol
 */
LUA_FUNCTION_DEF (config, register_callback_symbol);
LUA_FUNCTION_DEF (config, register_callback_symbol_priority);

/**
 * @method rspamd_config:set_metric_symbol(name, weight, [description], [metric])
 * Set the value of a specified symbol in a metric
 * @param {string} name name of symbol
 * @param {number} weight the weight multiplier
 * @param {string} description symbolic description
 * @param {string} metric metric name (default metric is used if this value is absent)
 */
LUA_FUNCTION_DEF (config, set_metric_symbol);

/**
 * @method rspamd_config:add_composite(name, expression)
 * @param {string} name name of composite symbol
 * @param {string} expression symbolic expression of the composite rule
 * @return {bool} true if a composite has been added sucessfully
 */
LUA_FUNCTION_DEF (config, add_composite);
/***
 * @method rspamd_config:register_pre_filter(callback)
 * Register function to be called prior to symbols processing.
 * @param {function} callback callback function
 * @example
local function check_function(task)
	-- It is possible to manipulate the task object here: set settings, set pre-action and so on
	...
end

rspamd_config:register_pre_filter(check_function)
 */
LUA_FUNCTION_DEF (config, register_pre_filter);
/***
 * @method rspamd_config:register_pre_filter(callback)
 * Register function to be called after symbols are processed.
 * @param {function} callback callback function
 */
LUA_FUNCTION_DEF (config, register_post_filter);
/* XXX: obsoleted */
LUA_FUNCTION_DEF (config, register_module_option);
/* XXX: not needed now */
LUA_FUNCTION_DEF (config, get_api_version);
/***
 * @method rspamd_config:get_key(name)
 * Returns configuration section with the specified `name`.
 * @param {string} name name of config section
 * @return {variant} specific value of section
 * @example

local set_section = rspamd_config:get_key("settings")
if type(set_section) == "string" then
  -- Just a map of ucl
  if rspamd_config:add_map(set_section, "settings map", process_settings_map) then
    rspamd_config:register_pre_filter(check_settings)
  end
elseif type(set_section) == "table" then
  if process_settings_table(set_section) then
    rspamd_config:register_pre_filter(check_settings)
  end
end
 */
LUA_FUNCTION_DEF (config, get_key);
/***
 * @method rspamd_config:__newindex(name, callback)
 * This metamethod is called if new indicies are added to the `rspamd_config` object.
 * Technically, it is the equialent of @see rspamd_config:register_symbol where `weight` is 1.0.
 * @param {string} name index name
 * @param {function} callback callback to be called
 * @example
rspamd_config.R_EMPTY_IMAGE = function (task)
	parts = task:get_text_parts()
	if parts then
		for _,part in ipairs(parts) do
			if part:is_empty() then
				images = task:get_images()
				if images then
					-- Symbol `R_EMPTY_IMAGE` is inserted
					return true
				end
				return false
			end
		end
	end
	return false
end
 */
LUA_FUNCTION_DEF (config, newindex);

static const struct luaL_reg configlib_m[] = {
	LUA_INTERFACE_DEF (config, get_module_opt),
	LUA_INTERFACE_DEF (config, get_mempool),
	LUA_INTERFACE_DEF (config, get_all_opt),
	LUA_INTERFACE_DEF (config, add_radix_map),
	LUA_INTERFACE_DEF (config, radix_from_config),
	LUA_INTERFACE_DEF (config, add_hash_map),
	LUA_INTERFACE_DEF (config, add_kv_map),
	LUA_INTERFACE_DEF (config, add_map),
	LUA_INTERFACE_DEF (config, get_classifier),
	LUA_INTERFACE_DEF (config, register_symbol),
	LUA_INTERFACE_DEF (config, register_symbols),
	LUA_INTERFACE_DEF (config, register_virtual_symbol),
	LUA_INTERFACE_DEF (config, register_callback_symbol),
	LUA_INTERFACE_DEF (config, register_callback_symbol_priority),
	LUA_INTERFACE_DEF (config, set_metric_symbol),
	LUA_INTERFACE_DEF (config, add_composite),
	LUA_INTERFACE_DEF (config, register_module_option),
	LUA_INTERFACE_DEF (config, register_pre_filter),
	LUA_INTERFACE_DEF (config, register_post_filter),
	LUA_INTERFACE_DEF (config, get_api_version),
	LUA_INTERFACE_DEF (config, get_key),
	{"__tostring", rspamd_lua_class_tostring},
	{"__newindex", lua_config_newindex},
	{NULL, NULL}
};


/* Radix tree */
LUA_FUNCTION_DEF (radix, get_key);

static const struct luaL_reg radixlib_m[] = {
	LUA_INTERFACE_DEF (radix, get_key),
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

/* Hash table */
LUA_FUNCTION_DEF (hash_table, get_key);

static const struct luaL_reg hashlib_m[] = {
	LUA_INTERFACE_DEF (hash_table, get_key),
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

struct rspamd_config *
lua_check_config (lua_State * L, gint pos)
{
	void *ud = luaL_checkudata (L, pos, "rspamd{config}");
	luaL_argcheck (L, ud != NULL, pos, "'config' expected");
	return ud ? *((struct rspamd_config **)ud) : NULL;
}

static radix_compressed_t *
lua_check_radix (lua_State * L)
{
	void *ud = luaL_checkudata (L, 1, "rspamd{radix}");
	luaL_argcheck (L, ud != NULL, 1, "'radix' expected");
	return ud ? **((radix_compressed_t ***)ud) : NULL;
}

static GHashTable *
lua_check_hash_table (lua_State * L)
{
	void *ud = luaL_checkudata (L, 1, "rspamd{hash_table}");
	luaL_argcheck (L, ud != NULL, 1, "'hash_table' expected");
	return ud ? **((GHashTable ***)ud) : NULL;
}

/*** Config functions ***/
static gint
lua_config_get_api_version (lua_State *L)
{
	lua_pushinteger (L, RSPAMD_LUA_API_VERSION);
	return 1;
}

static gint
lua_config_get_module_opt (lua_State * L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	const gchar *mname, *optname;
	const ucl_object_t *obj;

	if (cfg) {
		mname = luaL_checkstring (L, 2);
		optname = luaL_checkstring (L, 3);

		if (mname && optname) {
			obj = rspamd_config_get_module_opt (cfg, mname, optname);
			if (obj) {
				return ucl_object_push_lua (L, obj, TRUE);
			}
		}
	}
	lua_pushnil (L);
	return 1;
}

static int
lua_config_get_mempool (lua_State * L)
{
	rspamd_mempool_t **ppool;
	struct rspamd_config *cfg = lua_check_config (L, 1);

	if (cfg != NULL) {
		ppool = lua_newuserdata (L, sizeof (rspamd_mempool_t *));
		rspamd_lua_setclass (L, "rspamd{mempool}", -1);
		*ppool = cfg->cfg_pool;
	}
	return 1;
}

static gint
lua_config_get_all_opt (lua_State * L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	const gchar *mname;
	const ucl_object_t *obj, *cur, *cur_elt;
	ucl_object_iter_t it = NULL;
	gint i;

	if (cfg) {
		mname = luaL_checkstring (L, 2);

		if (mname) {
			obj = ucl_obj_get_key (cfg->rcl_obj, mname);
			/* Flatten object */
			if (obj != NULL && (ucl_object_type (obj) == UCL_OBJECT ||
					ucl_object_type (obj) == UCL_ARRAY)) {

				lua_newtable (L);
				it = ucl_object_iterate_new (obj);

				LL_FOREACH (obj, cur) {
					it = ucl_object_iterate_reset (it, cur);

					while ((cur_elt = ucl_object_iterate_safe (it, true))) {
						lua_pushstring (L, ucl_object_key (cur_elt));
						ucl_object_push_lua (L, cur_elt, true);
						lua_settable (L, -3);
					}
				}

				ucl_object_iterate_free (it);

				return 1;
			}
			else if (obj != NULL) {
				lua_newtable (L);
				i = 1;

				LL_FOREACH (obj, cur) {
					lua_pushnumber (L, i++);
					ucl_object_push_lua (L, cur, true);
					lua_settable (L, -3);
				}

				return 1;
			}
		}
	}
	lua_pushnil (L);

	return 1;
}


static gint
lua_config_get_classifier (lua_State * L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	struct rspamd_classifier_config *clc = NULL, **pclc = NULL;
	const gchar *name;
	GList *cur;

	if (cfg) {
		name = luaL_checkstring (L, 2);

		cur = g_list_first (cfg->classifiers);
		while (cur) {
			clc = cur->data;
			if (g_ascii_strcasecmp (clc->name, name) == 0) {
				pclc = &clc;
				break;
			}
			cur = g_list_next (cur);
		}
		if (pclc) {
			pclc = lua_newuserdata (L,
					sizeof (struct rspamd_classifier_config *));
			rspamd_lua_setclass (L, "rspamd{classifier}", -1);
			*pclc = clc;
			return 1;
		}
	}

	lua_pushnil (L);
	return 1;

}

struct lua_callback_data {
	union {
		gchar *name;
		gint ref;
	} callback;
	gboolean cb_is_ref;
	lua_State *L;
	gchar *symbol;
};

/*
 * Unref symbol if it is local reference
 */
static void
lua_destroy_cfg_symbol (gpointer ud)
{
	struct lua_callback_data *cd = ud;

	/* Unref callback */
	if (cd->cb_is_ref) {
		luaL_unref (cd->L, LUA_REGISTRYINDEX, cd->callback.ref);
	}
}

static gint
lua_config_register_module_option (lua_State *L)
{
	return 0;
}

void
rspamd_lua_call_post_filters (struct rspamd_task *task)
{
	struct lua_callback_data *cd;
	struct rspamd_task **ptask;
	GList *cur;

	cur = task->cfg->post_filters;
	while (cur) {
		cd = cur->data;
		if (cd->cb_is_ref) {
			lua_rawgeti (cd->L, LUA_REGISTRYINDEX, cd->callback.ref);
		}
		else {
			lua_getglobal (cd->L, cd->callback.name);
		}
		ptask = lua_newuserdata (cd->L, sizeof (struct rspamd_task *));
		rspamd_lua_setclass (cd->L, "rspamd{task}", -1);
		*ptask = task;

		if (lua_pcall (cd->L, 1, 0, 0) != 0) {
			msg_info ("call to %s failed: %s",
				cd->cb_is_ref ? "local function" :
				cd->callback.name,
				lua_tostring (cd->L, -1));
		}
		cur = g_list_next (cur);
	}
}

static gint
lua_config_register_post_filter (lua_State *L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	struct lua_callback_data *cd;

	if (cfg) {
		cd =
			rspamd_mempool_alloc (cfg->cfg_pool,
				sizeof (struct lua_callback_data));
		if (lua_type (L, 2) == LUA_TSTRING) {
			cd->callback.name = rspamd_mempool_strdup (cfg->cfg_pool,
					luaL_checkstring (L, 2));
			cd->cb_is_ref = FALSE;
		}
		else {
			lua_pushvalue (L, 2);
			/* Get a reference */
			cd->callback.ref = luaL_ref (L, LUA_REGISTRYINDEX);
			cd->cb_is_ref = TRUE;
		}
		cd->L = L;
		cfg->post_filters = g_list_prepend (cfg->post_filters, cd);
		rspamd_mempool_add_destructor (cfg->cfg_pool,
			(rspamd_mempool_destruct_t)lua_destroy_cfg_symbol,
			cd);
	}
	return 1;
}

void
rspamd_lua_call_pre_filters (struct rspamd_task *task)
{
	struct lua_callback_data *cd;
	struct rspamd_task **ptask;
	GList *cur;

	cur = task->cfg->pre_filters;
	while (cur) {
		cd = cur->data;
		if (cd->cb_is_ref) {
			lua_rawgeti (cd->L, LUA_REGISTRYINDEX, cd->callback.ref);
		}
		else {
			lua_getglobal (cd->L, cd->callback.name);
		}
		ptask = lua_newuserdata (cd->L, sizeof (struct rspamd_task *));
		rspamd_lua_setclass (cd->L, "rspamd{task}", -1);
		*ptask = task;

		if (lua_pcall (cd->L, 1, 0, 0) != 0) {
			msg_info ("call to %s failed: %s",
				cd->cb_is_ref ? "local function" :
				cd->callback.name,
				lua_tostring (cd->L, -1));
		}
		cur = g_list_next (cur);
	}
}

static gint
lua_config_register_pre_filter (lua_State *L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	struct lua_callback_data *cd;

	if (cfg) {
		cd =
			rspamd_mempool_alloc (cfg->cfg_pool,
				sizeof (struct lua_callback_data));
		if (lua_type (L, 2) == LUA_TSTRING) {
			cd->callback.name = rspamd_mempool_strdup (cfg->cfg_pool,
					luaL_checkstring (L, 2));
			cd->cb_is_ref = FALSE;
		}
		else {
			lua_pushvalue (L, 2);
			/* Get a reference */
			cd->callback.ref = luaL_ref (L, LUA_REGISTRYINDEX);
			cd->cb_is_ref = TRUE;
		}
		cd->L = L;
		cfg->pre_filters = g_list_prepend (cfg->pre_filters, cd);
		rspamd_mempool_add_destructor (cfg->cfg_pool,
			(rspamd_mempool_destruct_t)lua_destroy_cfg_symbol,
			cd);
	}
	return 1;
}

static gint
lua_config_add_radix_map (lua_State *L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	const gchar *map_line, *description;
	radix_compressed_t **r, ***ud;

	if (cfg) {
		map_line = luaL_checkstring (L, 2);
		description = lua_tostring (L, 3);
		r = rspamd_mempool_alloc (cfg->cfg_pool, sizeof (radix_compressed_t *));
		*r = radix_create_compressed ();
		if (!rspamd_map_add (cfg, map_line, description, rspamd_radix_read,
			rspamd_radix_fin, (void **)r)) {
			msg_warn ("invalid radix map %s", map_line);
			radix_destroy_compressed (*r);
			lua_pushnil (L);
			return 1;
		}
		ud = lua_newuserdata (L, sizeof (radix_compressed_t *));
		*ud = r;
		rspamd_lua_setclass (L, "rspamd{radix}", -1);

		return 1;
	}

	lua_pushnil (L);
	return 1;

}

static gint
lua_config_radix_from_config (lua_State *L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	const gchar *mname, *optname;
	const ucl_object_t *obj;
	radix_compressed_t **r, ***ud;

	if (!cfg) {
		lua_pushnil (L);
		return 1;
	}

	mname = luaL_checkstring (L, 2);
	optname = luaL_checkstring (L, 3);

	if (mname && optname) {
		obj = rspamd_config_get_module_opt (cfg, mname, optname);
		if (obj) {
			r = rspamd_mempool_alloc (cfg->cfg_pool, sizeof (radix_compressed_t *));
			*r = radix_create_compressed ();
			radix_add_generic_iplist (ucl_obj_tostring (obj), r);
			ud = lua_newuserdata (L, sizeof (radix_compressed_t *));
			*ud = r;
			rspamd_lua_setclass (L, "rspamd{radix}", -1);
			return 1;
		} else {
			msg_warn ("Couldnt find config option [%s][%s]", mname, optname);
			lua_pushnil (L);
			return 1;
		}
	} else {
		msg_warn ("Couldnt find config option");
		lua_pushnil (L);
		return 1;
	}
}

static gint
lua_config_add_hash_map (lua_State *L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	const gchar *map_line, *description;
	GHashTable **r, ***ud;

	if (cfg) {
		map_line = luaL_checkstring (L, 2);
		description = lua_tostring (L, 3);
		r = rspamd_mempool_alloc (cfg->cfg_pool, sizeof (GHashTable *));
		*r = g_hash_table_new (rspamd_strcase_hash, rspamd_strcase_equal);
		if (!rspamd_map_add (cfg, map_line, description, rspamd_hosts_read, rspamd_hosts_fin,
			(void **)r)) {
			msg_warn ("invalid hash map %s", map_line);
			g_hash_table_destroy (*r);
			lua_pushnil (L);
			return 1;
		}
		rspamd_mempool_add_destructor (cfg->cfg_pool,
			(rspamd_mempool_destruct_t)g_hash_table_destroy,
			*r);
		ud = lua_newuserdata (L, sizeof (GHashTable *));
		*ud = r;
		rspamd_lua_setclass (L, "rspamd{hash_table}", -1);

		return 1;
	}

	lua_pushnil (L);
	return 1;

}

static gint
lua_config_add_kv_map (lua_State *L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	const gchar *map_line, *description;
	GHashTable **r, ***ud;

	if (cfg) {
		map_line = luaL_checkstring (L, 2);
		description = lua_tostring (L, 3);
		r = rspamd_mempool_alloc (cfg->cfg_pool, sizeof (GHashTable *));
		*r = g_hash_table_new (rspamd_strcase_hash, rspamd_strcase_equal);
		if (!rspamd_map_add (cfg, map_line, description, rspamd_kv_list_read, rspamd_kv_list_fin,
			(void **)r)) {
			msg_warn ("invalid hash map %s", map_line);
			g_hash_table_destroy (*r);
			lua_pushnil (L);
			return 1;
		}
		rspamd_mempool_add_destructor (cfg->cfg_pool,
			(rspamd_mempool_destruct_t)g_hash_table_destroy,
			*r);
		ud = lua_newuserdata (L, sizeof (GHashTable *));
		*ud = r;
		rspamd_lua_setclass (L, "rspamd{hash_table}", -1);

		return 1;
	}

	lua_pushnil (L);
	return 1;

}

static gint
lua_config_get_key (lua_State *L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	const gchar *name;
	size_t namelen;
	const ucl_object_t *val;

	name = luaL_checklstring(L, 2, &namelen);
	if (name && cfg) {
		val = ucl_object_find_keyl(cfg->rcl_obj, name, namelen);
		if (val != NULL) {
			ucl_object_push_lua (L, val, val->type != UCL_ARRAY);
		}
		else {
			lua_pushnil (L);
		}
	}
	else {
		lua_pushnil (L);
	}

	return 1;
}

static void
lua_metric_symbol_callback (struct rspamd_task *task, gpointer ud)
{
	struct lua_callback_data *cd = ud;
	struct rspamd_task **ptask;
	gint level = lua_gettop (cd->L), nresults;

	if (cd->cb_is_ref) {
		lua_rawgeti (cd->L, LUA_REGISTRYINDEX, cd->callback.ref);
	}
	else {
		lua_getglobal (cd->L, cd->callback.name);
	}
	ptask = lua_newuserdata (cd->L, sizeof (struct rspamd_task *));
	rspamd_lua_setclass (cd->L, "rspamd{task}", -1);
	*ptask = task;

	if (lua_pcall (cd->L, 1, LUA_MULTRET, 0) != 0) {
		msg_info ("call to (%s)%s failed: %s", cd->symbol,
			cd->cb_is_ref ? "local function" : cd->callback.name,
			lua_tostring (cd->L, -1));
	}

	nresults = lua_gettop (cd->L) - level;
	if (nresults >= 1) {
		/* Function returned boolean, so maybe we need to insert result? */
		gboolean res;
		GList *opts = NULL;
		gint i;
		gdouble flag = 1.0;

		if (lua_type (cd->L, level + 1) == LUA_TBOOLEAN) {
			res = lua_toboolean (cd->L, level + 1);
			if (res) {
				gint first_opt = 2;

				if (lua_type (cd->L, level + 2) == LUA_TNUMBER) {
					flag = lua_tonumber (cd->L, level + 2);
					/* Shift opt index */
					first_opt = 3;
				}

				for (i = lua_gettop (cd->L); i >= level + first_opt; i --) {
					if (lua_type (cd->L, i) == LUA_TSTRING) {
						const char *opt = lua_tostring (cd->L, i);

						opts = g_list_prepend (opts,
							rspamd_mempool_strdup (task->task_pool, opt));
					}
				}
				rspamd_task_insert_result (task, cd->symbol, flag, opts);
			}
		}
		lua_pop (cd->L, nresults);
	}
}

static void
rspamd_register_symbol_fromlua (lua_State *L,
		struct rspamd_config *cfg,
		const gchar *name,
		gint ref,
		gdouble weight,
		gint priority,
		enum rspamd_symbol_type type)
{
	struct lua_callback_data *cd;

	if (name) {
		cd = rspamd_mempool_alloc0 (cfg->cfg_pool,
				sizeof (struct lua_callback_data));
		cd->cb_is_ref = TRUE;
		cd->callback.ref = ref;
		cd->L = L;
		cd->symbol = rspamd_mempool_strdup (cfg->cfg_pool, name);

		register_symbol_common (&cfg->cache,
				name,
				weight,
				priority,
				lua_metric_symbol_callback,
				cd,
				type);
		rspamd_mempool_add_destructor (cfg->cfg_pool,
				(rspamd_mempool_destruct_t)lua_destroy_cfg_symbol,
				cd);
	}


}

static gint
lua_config_register_symbol (lua_State * L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	gchar *name;
	double weight;

	if (cfg) {
		name = rspamd_mempool_strdup (cfg->cfg_pool, luaL_checkstring (L, 2));
		weight = luaL_checknumber (L, 3);

		if (lua_type (L, 4) == LUA_TSTRING) {
			lua_getglobal (L, luaL_checkstring (L, 4));
		}
		else {
			lua_pushvalue (L, 4);
		}
		if (name) {
			rspamd_register_symbol_fromlua (L,
					cfg,
					name,
					luaL_ref (L, LUA_REGISTRYINDEX),
					weight,
					0,
					SYMBOL_TYPE_NORMAL);
		}
	}

	return 0;
}

static gint
lua_config_register_symbols (lua_State *L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	gint i, top, idx;
	gchar *sym;
	gdouble weight = 1.0;

	if (lua_gettop (L) < 3) {
		msg_err ("not enough arguments to register a function");
		return 0;
	}
	if (cfg) {
		if (lua_type (L, 2) == LUA_TSTRING) {
			lua_getglobal (L, luaL_checkstring (L, 2));
		}
		else {
			lua_pushvalue (L, 2);
		}
		idx = luaL_ref (L, LUA_REGISTRYINDEX);

		if (lua_type (L, 3) == LUA_TNUMBER) {
			weight = lua_tonumber (L, 3);
			top = 4;
		}
		else {
			top = 3;
		}
		sym = rspamd_mempool_strdup (cfg->cfg_pool, luaL_checkstring (L, top ++));
		rspamd_register_symbol_fromlua (L,
				cfg,
				sym,
				idx,
				weight,
				0,
				SYMBOL_TYPE_CALLBACK);
		for (i = top; i <= lua_gettop (L); i++) {
			if (lua_type (L, i) == LUA_TTABLE) {
				lua_pushvalue (L, i);
				lua_pushnil (L);
				while (lua_next (L, -2)) {
					lua_pushvalue (L, -2);
					sym = rspamd_mempool_strdup (cfg->cfg_pool,
							luaL_checkstring (L, -2));
					register_virtual_symbol (&cfg->cache, sym, weight);
					lua_pop (L, 2);
				}
				lua_pop (L, 1);
			}
			else if (lua_type (L, i) == LUA_TSTRING) {
				sym = rspamd_mempool_strdup (cfg->cfg_pool,
						luaL_checkstring (L, i));
				register_virtual_symbol (&cfg->cache, sym, weight);
			}
		}
	}

	return 0;
}

static gint
lua_config_register_virtual_symbol (lua_State * L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	gchar *name;
	double weight;

	if (cfg) {
		name = rspamd_mempool_strdup (cfg->cfg_pool, luaL_checkstring (L, 2));
		weight = luaL_checknumber (L, 3);
		if (name) {
			register_virtual_symbol (&cfg->cache, name, weight);
		}
	}
	return 0;
}

static gint
lua_config_register_callback_symbol (lua_State * L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	gchar *name;
	double weight;

	if (cfg) {
		name = rspamd_mempool_strdup (cfg->cfg_pool, luaL_checkstring (L, 2));
		weight = luaL_checknumber (L, 3);

		if (lua_type (L, 4) == LUA_TSTRING) {
			lua_getglobal (L, luaL_checkstring (L, 4));
		}
		else {
			lua_pushvalue (L, 4);
		}
		if (name) {
			rspamd_register_symbol_fromlua (L,
					cfg,
					name,
					luaL_ref (L, LUA_REGISTRYINDEX),
					weight,
					0,
					SYMBOL_TYPE_CALLBACK);
		}
	}

	return 0;
}

static gint
lua_config_register_callback_symbol_priority (lua_State * L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	gchar *name;
	double weight;
	gint priority;

	if (cfg) {
		name = rspamd_mempool_strdup (cfg->cfg_pool, luaL_checkstring (L, 2));
		weight = luaL_checknumber (L, 3);
		priority = luaL_checknumber (L, 4);

		if (lua_type (L, 5) == LUA_TSTRING) {
			lua_getglobal (L, luaL_checkstring (L, 5));
		}
		else {
			lua_pushvalue (L, 5);
		}
		if (name) {
			rspamd_register_symbol_fromlua (L,
					cfg,
					name,
					luaL_ref (L, LUA_REGISTRYINDEX),
					weight,
					priority,
					SYMBOL_TYPE_CALLBACK);
		}
	}

	return 0;
}

static gint
lua_config_set_metric_symbol (lua_State * L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	GList *metric_list;
	gchar *name;
	const gchar *metric_name = DEFAULT_METRIC, *description = NULL;
	double weight;
	struct rspamd_symbol_def *s;
	struct metric *metric;

	if (cfg) {
		name = rspamd_mempool_strdup (cfg->cfg_pool, luaL_checkstring (L, 2));
		weight = luaL_checknumber (L, 3);

		if (lua_gettop (L) > 3 && lua_type (L, 4) == LUA_TSTRING) {
			description = luaL_checkstring (L, 4);
		}
		if (lua_gettop (L) > 4 && lua_type (L, 5) == LUA_TSTRING) {
			metric_name = luaL_checkstring (L, 5);
		}

		metric = g_hash_table_lookup (cfg->metrics, metric_name);

		if (metric == NULL) {
			msg_err ("metric named %s is not defined", metric_name);
		}
		else if (name != NULL) {
			s = g_hash_table_lookup (metric->symbols, name);

			if (s == NULL) {
				msg_debug ("set new symbol %s in metric %s with weight %.2f",
						name, metric_name, weight);
				s = rspamd_mempool_alloc0 (cfg->cfg_pool, sizeof (*s));
				s->name = rspamd_mempool_strdup (cfg->cfg_pool, name);
				s->weight_ptr = rspamd_mempool_alloc (cfg->cfg_pool,
										sizeof (gdouble));

				if (description != NULL) {
					s->description =  rspamd_mempool_strdup (cfg->cfg_pool,
							description);
				}

				g_hash_table_insert (metric->symbols, s->name, s);
				if ((metric_list =
						g_hash_table_lookup (cfg->metrics_symbols, s->name)) == NULL) {
					metric_list = g_list_prepend (NULL, metric);
					rspamd_mempool_add_destructor (cfg->cfg_pool,
							(rspamd_mempool_destruct_t)g_list_free,
							metric_list);
					g_hash_table_insert (cfg->metrics_symbols, s->name, metric_list);
				}
				else {
					/* Slow but keep start element of list in safe */
					if (!g_list_find (metric_list, metric)) {
						metric_list = g_list_append (metric_list, metric);
					}
				}
			}

			*s->weight_ptr = weight;
		}
	}

	return 0;
}

static gint
lua_config_add_composite (lua_State * L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	struct rspamd_expression *expr;
	gchar *name;
	const gchar *expr_str;
	struct rspamd_composite *composite;
	gboolean ret = FALSE, new = TRUE;
	GError *err = NULL;

	if (cfg) {
		name = rspamd_mempool_strdup (cfg->cfg_pool, luaL_checkstring (L, 2));
		expr_str = luaL_checkstring (L, 3);

		if (name && expr_str) {
			if (!rspamd_parse_expression (expr_str, 0, &composite_expr_subr,
					NULL, cfg->cfg_pool, &err, &expr)) {
				msg_err ("cannot parse composite expression %s: %e", expr_str,
						err);
				g_error_free (err);
			}
			else {
				if (g_hash_table_lookup (cfg->composite_symbols, name) != NULL) {
					msg_warn ("composite %s is redefined", name);
					new = FALSE;
				}
				composite = rspamd_mempool_alloc (cfg->cfg_pool,
						sizeof (struct rspamd_composite));
				composite->expr = expr;
				composite->id = g_hash_table_size (cfg->composite_symbols);
				g_hash_table_insert (cfg->composite_symbols,
						(gpointer)name,
						composite);

				if (new) {
					register_virtual_symbol (&cfg->cache, name, 1);
				}

				ret = TRUE;
			}
		}
	}

	lua_pushboolean (L, ret);

	return 1;
}

static gint
lua_config_newindex (lua_State *L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	const gchar *name;

	name = luaL_checkstring (L, 2);

	if (cfg != NULL && name != NULL && lua_gettop (L) > 2) {
		if (lua_type (L, 3) == LUA_TFUNCTION) {
			/* Normal symbol from just a function */
			lua_pushvalue (L, 3);
			rspamd_register_symbol_fromlua (L,
					cfg,
					name,
					luaL_ref (L, LUA_REGISTRYINDEX),
					1.0,
					0,
					SYMBOL_TYPE_NORMAL);
		}
		else if (lua_type (L, 3) == LUA_TTABLE) {
			gint type = SYMBOL_TYPE_NORMAL, priority = 0, idx;
			gdouble weight = 1.0;
			const char *type_str;

			/*
			 * Table can have the following attributes:
			 * "callback" - should be a callback function
			 * "weight" - optional weight
			 * "priority" - optional priority
			 * "type" - optional type (normal, virtual, callback)
			 */
			lua_pushstring (L, "callback");
			lua_gettable (L, -2);

			if (lua_type (L, -1) != LUA_TFUNCTION) {
				lua_pop (L, 1);
				msg_info ("cannot find callback definition for %s", name);
				return 0;
			}
			idx = luaL_ref (L, LUA_REGISTRYINDEX);

			/* Optional fields */
			lua_pushstring (L, "weight");
			lua_gettable (L, -2);

			if (lua_type (L, -1) == LUA_TNUMBER) {
				weight = lua_tonumber (L, -1);
			}
			lua_pop (L, 1);

			lua_pushstring (L, "priority");
			lua_gettable (L, -2);

			if (lua_type (L, -1) == LUA_TNUMBER) {
				priority = lua_tonumber (L, -1);
			}
			lua_pop (L, 1);

			lua_pushstring (L, "type");
			lua_gettable (L, -2);

			if (lua_type (L, -1) == LUA_TSTRING) {
				type_str = lua_tostring (L, -1);
				if (strcmp (type_str, "normal") == 0) {
					type = SYMBOL_TYPE_NORMAL;
				}
				else if (strcmp (type_str, "virtual") == 0) {
					type = SYMBOL_TYPE_VIRTUAL;
				}
				else if (strcmp (type_str, "callback") == 0) {
					type = SYMBOL_TYPE_CALLBACK;
				}
				else {
					msg_info ("unknown type: %s", type_str);
				}

			}
			lua_pop (L, 1);

			rspamd_register_symbol_fromlua (L,
					cfg,
					name,
					idx,
					weight,
					priority,
					type);
		}
	}

	return 0;
}

struct lua_map_callback_data {
	lua_State *L;
	gint ref;
	GString *data;
};

static gchar *
lua_map_read (rspamd_mempool_t *pool, gchar *chunk, gint len,
	struct map_cb_data *data)
{
	struct lua_map_callback_data *cbdata, *old;

	if (data->cur_data == NULL) {
		cbdata = g_slice_alloc0 (sizeof (*cbdata));
		old = (struct lua_map_callback_data *)data->prev_data;
		cbdata->L = old->L;
		cbdata->ref = old->ref;
		data->cur_data = cbdata;
	}
	else {
		cbdata = (struct lua_map_callback_data *)data->cur_data;
	}

	if (cbdata->data == NULL) {
		cbdata->data = g_string_new_len (chunk, len);
	}
	else {
		g_string_append_len (cbdata->data, chunk, len);
	}

	return NULL;
}

void
lua_map_fin (rspamd_mempool_t * pool, struct map_cb_data *data)
{
	struct lua_map_callback_data *cbdata, *old;

	if (data->prev_data) {
		/* Cleanup old data */
		old = (struct lua_map_callback_data *)data->prev_data;
		if (old->data) {
			g_string_free (old->data, TRUE);
		}
		g_slice_free1 (sizeof (*old), old);
		data->prev_data = NULL;
	}

	if (data->cur_data) {
		cbdata = (struct lua_map_callback_data *)data->cur_data;
	}
	else {
		msg_err ("no data read for map");
		return;
	}

	if (cbdata->data != NULL && cbdata->data->len != 0) {
		lua_rawgeti (cbdata->L, LUA_REGISTRYINDEX, cbdata->ref);
		lua_pushlstring (cbdata->L, cbdata->data->str, cbdata->data->len);

		if (lua_pcall (cbdata->L, 1, 0, 0) != 0) {
			msg_info ("call to %s failed: %s", "local function",
				lua_tostring (cbdata->L, -1));
		}
	}
}

static gint
lua_config_add_map (lua_State *L)
{
	struct rspamd_config *cfg = lua_check_config (L, 1);
	const gchar *map_line, *description;
	struct lua_map_callback_data *cbdata, **pcbdata;
	int cbidx;

	if (cfg) {
		map_line = luaL_checkstring (L, 2);

		if (lua_gettop (L) == 4) {
			description = lua_tostring (L, 3);
			cbidx = 4;
		}
		else {
			description = NULL;
			cbidx = 3;
		}

		if (lua_type (L, cbidx) == LUA_TFUNCTION) {
			cbdata = g_slice_alloc (sizeof (*cbdata));
			cbdata->L = L;
			cbdata->data = NULL;
			lua_pushvalue (L, cbidx);
			/* Get a reference */
			cbdata->ref = luaL_ref (L, LUA_REGISTRYINDEX);
			pcbdata = rspamd_mempool_alloc (cfg->cfg_pool, sizeof (cbdata));
			*pcbdata = cbdata;
			if (!rspamd_map_add (cfg, map_line, description, lua_map_read, lua_map_fin,
				(void **)pcbdata)) {
				msg_warn ("invalid hash map %s", map_line);
				lua_pushboolean (L, false);
			}
			else {
				lua_pushboolean (L, true);
			}
		}
		else {
			msg_warn ("invalid callback argument for map %s", map_line);
			lua_pushboolean (L, false);
		}
	}
	else {
		lua_pushboolean (L, false);
	}

	return 1;
}

/* Radix and hash table functions */
static gint
lua_radix_get_key (lua_State * L)
{
	radix_compressed_t *radix = lua_check_radix (L);
	struct rspamd_lua_ip *addr = NULL;
	gpointer ud;
	guint32 key_num = 0;
	gboolean ret = FALSE;

	if (radix) {
		if (lua_type (L, 2) == LUA_TNUMBER) {
			key_num = luaL_checknumber (L, 2);
			key_num = htonl (key_num);
		}
		else if (lua_type (L, 2) == LUA_TUSERDATA) {
			ud = luaL_checkudata (L, 2, "rspamd{ip}");
			if (ud != NULL) {
				addr = *((struct rspamd_lua_ip **)ud);
				if (addr->addr == NULL) {
					msg_err ("rspamd{ip} is not valid");
					addr = NULL;
				}
			}
			else {
				msg_err ("invalid userdata type provided, rspamd{ip} expected");
			}
		}

		if (addr != NULL) {
			if (radix_find_compressed_addr (radix, addr->addr)
					!=  RADIX_NO_VALUE) {
				ret = TRUE;
			}
		}
		else if (key_num != 0) {
			if (radix_find_compressed (radix, (guint8 *)&key_num, sizeof (key_num))
				!= RADIX_NO_VALUE) {
				ret = TRUE;
			}
		}
	}

	lua_pushboolean (L, ret);
	return 1;
}

static gint
lua_hash_table_get_key (lua_State * L)
{
	GHashTable *tbl = lua_check_hash_table (L);
	const gchar *key, *value;

	if (tbl) {
		key = luaL_checkstring (L, 2);

		if ((value = g_hash_table_lookup (tbl, key)) != NULL) {
			lua_pushstring (L, value);
			return 1;
		}
	}

	lua_pushnil (L);
	return 1;
}

/* Trie functions */

/* Init functions */

void
luaopen_config (lua_State * L)
{
	rspamd_lua_new_class (L, "rspamd{config}", configlib_m);

	lua_pop (L, 1);                      /* remove metatable from stack */
}

void
luaopen_radix (lua_State * L)
{
	rspamd_lua_new_class (L, "rspamd{radix}", radixlib_m);

	lua_pop (L, 1);                      /* remove metatable from stack */
}

void
luaopen_hash_table (lua_State * L)
{
	rspamd_lua_new_class (L, "rspamd{hash_table}", hashlib_m);
	luaL_register (L, "rspamd_hash_table", null_reg);

	lua_pop (L, 1);                      /* remove metatable from stack */
}
