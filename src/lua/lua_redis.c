/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "lua_common.h"
#include "utlist.h"

#include "contrib/hiredis/hiredis.h"
#include "contrib/hiredis/async.h"

#define REDIS_DEFAULT_TIMEOUT 1.0

/***
 * @module rspamd_redis
 * This module implements redis asynchronous client for rspamd LUA API.
 * Here is an example of using of this module:
 * @example
local rspamd_redis = require "rspamd_redis"
local rspamd_logger = require "rspamd_logger"

local function symbol_callback(task)
	local redis_key = 'some_key'
	local function redis_cb(err, data)
		if not err then
			rspamd_logger.infox('redis returned %1=%2', redis_key, data)
		end
	end

	rspamd_redis.make_request(task, "127.0.0.1:6379", redis_cb,
		'GET', {redis_key})
	-- or in table form:
	-- rspamd_redis.make_request({task=task, host="127.0.0.1:6379,
	--	callback=redis_cb, timeout=2.0, cmd='GET', args={redis_key}})
end
 */

LUA_FUNCTION_DEF (redis, make_request);
LUA_FUNCTION_DEF (redis, make_request_sync);
LUA_FUNCTION_DEF (redis, connect);
LUA_FUNCTION_DEF (redis, connect_sync);
LUA_FUNCTION_DEF (redis, add_cmd);
LUA_FUNCTION_DEF (redis, exec);
LUA_FUNCTION_DEF (redis, gc);

static const struct luaL_reg redislib_f[] = {
	LUA_INTERFACE_DEF (redis, make_request),
	LUA_INTERFACE_DEF (redis, make_request_sync),
	LUA_INTERFACE_DEF (redis, connect),
	LUA_INTERFACE_DEF (redis, connect_sync),
	{NULL, NULL}
};

static const struct luaL_reg redislib_m[] = {
	LUA_INTERFACE_DEF (redis, add_cmd),
	LUA_INTERFACE_DEF (redis, exec),
	{"__gc", lua_redis_gc},
	{"__tostring", rspamd_lua_class_tostring},
	{NULL, NULL}
};

#undef REDIS_DEBUG_REFS
#ifdef REDIS_DEBUG_REFS
#define REDIS_RETAIN(x) do { \
	msg_err ("retain ref %p, refcount: %d", (x), (x)->ref.refcount); \
	REF_RETAIN(x);	\
} while (0)

#define REDIS_RELEASE(x) do { \
	msg_err ("release ref %p, refcount: %d", (x), (x)->ref.refcount); \
	REF_RELEASE(x);	\
} while (0)
#else
#define REDIS_RETAIN REF_RETAIN
#define REDIS_RELEASE REF_RELEASE
#endif

#ifdef WITH_HIREDIS
struct lua_redis_specific_userdata;
/**
 * Struct for userdata representation
 */
struct lua_redis_userdata {
	redisAsyncContext *ctx;
	lua_State *L;
	struct rspamd_async_session *s;
	struct event_base *ev_base;
	struct rspamd_config *cfg;
	struct rspamd_redis_pool *pool;
	gchar *server;
	gchar *reqline;
	struct lua_redis_specific_userdata *specific;
	gdouble timeout;
	guint16 port;
	guint16 terminated;
};

#define LUA_REDIS_SPECIFIC_REPLIED (1 << 0)
#define LUA_REDIS_SPECIFIC_FINISHED (1 << 1)
#define LUA_REDIS_ASYNC (1 << 0)
#define LUA_REDIS_TEXTDATA (1 << 1)
#define IS_ASYNC(ctx) ((ctx)->flags & LUA_REDIS_ASYNC)

struct lua_redis_specific_userdata {
	gint cbref;
	guint nargs;
	gchar **args;
	gsize *arglens;
	struct rspamd_async_watcher *w;
	struct lua_redis_userdata *c;
	struct lua_redis_ctx *ctx;
	struct lua_redis_specific_userdata *next;
	struct event timeout;
	guint flags;
};

struct lua_redis_ctx {
	guint flags;
	union {
		struct lua_redis_userdata async;
		redisContext *sync;
	} d;
	guint cmds_pending;
	ref_entry_t ref;
};

static struct lua_redis_ctx *
lua_check_redis (lua_State * L, gint pos)
{
	void *ud = rspamd_lua_check_udata (L, pos, "rspamd{redis}");
	luaL_argcheck (L, ud != NULL, pos, "'redis' expected");
	return ud ? *((struct lua_redis_ctx **)ud) : NULL;
}

static void
lua_redis_free_args (char **args, gsize *arglens, guint nargs)
{
	guint i;

	if (args) {
		for (i = 0; i < nargs; i ++) {
			g_slice_free1 (arglens[i], args[i]);
		}

		g_slice_free1 (sizeof (gchar *) * nargs, args);
		g_slice_free1 (sizeof (gsize) * nargs, arglens);
	}
}

static void
lua_redis_dtor (struct lua_redis_ctx *ctx)
{
	struct lua_redis_userdata *ud;
	struct lua_redis_specific_userdata *cur, *tmp;
	gboolean is_successful = TRUE;
	struct redisAsyncContext *ac;

	if (IS_ASYNC (ctx)) {
		msg_debug ("desctructing %p", ctx);
		ud = &ctx->d.async;

		if (ud->ctx) {

			LL_FOREACH_SAFE (ud->specific, cur, tmp) {
				event_del (&cur->timeout);

				if (!(cur->flags & LUA_REDIS_SPECIFIC_REPLIED)) {
					is_successful = FALSE;
				}

				cur->flags |= LUA_REDIS_SPECIFIC_FINISHED;
			}

			ud->terminated = 1;
			ac = ud->ctx;
			ud->ctx = NULL;
			rspamd_redis_pool_release_connection (ud->pool, ac, is_successful);
		}

		LL_FOREACH_SAFE (ud->specific, cur, tmp) {
			lua_redis_free_args (cur->args, cur->arglens, cur->nargs);

			if (cur->cbref != -1) {
				luaL_unref (ud->L, LUA_REGISTRYINDEX, cur->cbref);
			}

			g_slice_free1 (sizeof (*cur), cur);
		}
	}
	else {
		if (ctx->d.sync) {
			redisFree (ctx->d.sync);
		}
	}

	g_slice_free1 (sizeof (*ctx), ctx);
}

static gint
lua_redis_gc (lua_State *L)
{
	struct lua_redis_ctx *ctx = lua_check_redis (L, 1);

	if (ctx) {
		REDIS_RELEASE (ctx);
	}

	return 0;
}

static void
lua_redis_fin (void *arg)
{
	struct lua_redis_specific_userdata *sp_ud = arg;
	struct lua_redis_ctx *ctx;

	ctx = sp_ud->ctx;
	event_del (&sp_ud->timeout);
	msg_debug ("finished redis query %p from session %p", sp_ud, ctx);
	sp_ud->flags |= LUA_REDIS_SPECIFIC_FINISHED;

	REDIS_RELEASE (ctx);
}

/**
 * Push error of redis request to lua callback
 * @param code
 * @param ud
 */
static void
lua_redis_push_error (const gchar *err,
	struct lua_redis_ctx *ctx,
	struct lua_redis_specific_userdata *sp_ud,
	gboolean connected)
{
	struct lua_redis_userdata *ud = sp_ud->c;

	if (!(sp_ud->flags & (LUA_REDIS_SPECIFIC_REPLIED|LUA_REDIS_SPECIFIC_FINISHED))) {
		if (sp_ud->cbref != -1) {
			/* Push error */
			lua_rawgeti (ud->L, LUA_REGISTRYINDEX, sp_ud->cbref);

			/* String of error */
			lua_pushstring (ud->L, err);
			/* Data is nil */
			lua_pushnil (ud->L);

			if (lua_pcall (ud->L, 2, 0, 0) != 0) {
				msg_info ("call to callback failed: %s", lua_tostring (ud->L, -1));
				lua_pop (ud->L, 1);
			}
		}

		sp_ud->flags |= LUA_REDIS_SPECIFIC_REPLIED;

		if (connected && ud->s) {
			rspamd_session_watcher_pop (ud->s, sp_ud->w);
			rspamd_session_remove_event (ud->s, lua_redis_fin, sp_ud);
		}
		else {
			lua_redis_fin (sp_ud);
		}
	}
}

static void
lua_redis_push_reply (lua_State *L, const redisReply *r, gboolean text_data)
{
	guint i;
	struct rspamd_lua_text *t;

	switch (r->type) {
	case REDIS_REPLY_INTEGER:
		lua_pushnumber (L, r->integer);
		break;
	case REDIS_REPLY_NIL:
		/* XXX: not the best approach */
		lua_newuserdata (L, sizeof (gpointer));
		break;
	case REDIS_REPLY_STRING:
	case REDIS_REPLY_STATUS:
		if (text_data) {
			t = lua_newuserdata (L, sizeof (*t));
			rspamd_lua_setclass (L, "rspamd{text}", -1);
			t->flags = 0;
			t->start = r->str;
			t->len = r->len;
		}
		else {
			lua_pushlstring (L, r->str, r->len);
		}
		break;
	case REDIS_REPLY_ARRAY:
		lua_createtable (L, r->elements, 0);
		for (i = 0; i < r->elements; ++i) {
			lua_redis_push_reply (L, r->element[i], text_data);
			lua_rawseti (L, -2, i + 1); /* Store sub-reply */
		}
		break;
	default: /* should not happen */
		msg_info ("unknown reply type: %d", r->type);
		break;
	}
}

/**
 * Push data of redis request to lua callback
 * @param r redis reply data
 * @param ud
 */
static void
lua_redis_push_data (const redisReply *r, struct lua_redis_ctx *ctx,
		struct lua_redis_specific_userdata *sp_ud)
{
	struct lua_redis_userdata *ud = sp_ud->c;

	if (!(sp_ud->flags & (LUA_REDIS_SPECIFIC_REPLIED|LUA_REDIS_SPECIFIC_FINISHED))) {
		if (sp_ud->cbref != -1) {
			/* Push error */
			lua_rawgeti (ud->L, LUA_REGISTRYINDEX, sp_ud->cbref);
			/* Error is nil */
			lua_pushnil (ud->L);
			/* Data */
			lua_redis_push_reply (ud->L, r, ctx->flags & LUA_REDIS_TEXTDATA);

			if (lua_pcall (ud->L, 2, 0, 0) != 0) {
				msg_info ("call to callback failed: %s", lua_tostring (ud->L, -1));
				lua_pop (ud->L, 1);
			}

		}

		sp_ud->flags |= LUA_REDIS_SPECIFIC_REPLIED;

		if (ud->s) {
			rspamd_session_watcher_pop (ud->s, sp_ud->w);
			rspamd_session_remove_event (ud->s, lua_redis_fin, sp_ud);
		}
		else {
			lua_redis_fin (sp_ud);
		}
	}
}

/**
 * Callback for redis replies
 * @param c context of redis connection
 * @param r redis reply
 * @param priv userdata
 */
static void
lua_redis_callback (redisAsyncContext *c, gpointer r, gpointer priv)
{
	redisReply *reply = r;
	struct lua_redis_specific_userdata *sp_ud = priv;
	struct lua_redis_ctx *ctx;
	struct lua_redis_userdata *ud;
	redisAsyncContext *ac;

	ctx = sp_ud->ctx;
	ud = sp_ud->c;

	if (ud->terminated) {
		/* We are already at the termination stage, just go out */
		return;
	}

	msg_debug ("got reply from redis %p for query %p", ctx, sp_ud);

	REDIS_RETAIN (ctx);

	/* If session is finished, we cannot call lua callbacks */
	if (!(sp_ud->flags & LUA_REDIS_SPECIFIC_FINISHED)) {
		if (c->err == 0) {
			if (r != NULL) {
				if (reply->type != REDIS_REPLY_ERROR) {
					lua_redis_push_data (reply, ctx, sp_ud);
				}
				else {
					lua_redis_push_error (reply->str, ctx, sp_ud, TRUE);
				}
			}
			else {
				lua_redis_push_error ("received no data from server", ctx, sp_ud, TRUE);
			}
		}
		else {
			if (c->err == REDIS_ERR_IO) {
				lua_redis_push_error (strerror (errno), ctx, sp_ud, TRUE);
			}
			else {
				lua_redis_push_error (c->errstr, ctx, sp_ud, TRUE);
			}
		}
	}

	ctx->cmds_pending --;

	if (ctx->cmds_pending == 0 && !ud->terminated) {
		/* Disconnect redis early as we don't need it anymore */
		ud->terminated = 1;
		ac = ud->ctx;
		ud->ctx = NULL;

		if (ac) {
			rspamd_redis_pool_release_connection (ud->pool, ac, FALSE);
		}
	}

	REDIS_RELEASE (ctx);
}

static void
lua_redis_timeout (int fd, short what, gpointer u)
{
	struct lua_redis_specific_userdata *sp_ud = u;
	struct lua_redis_ctx *ctx;
	redisAsyncContext *ac;

	if (sp_ud->flags & LUA_REDIS_SPECIFIC_FINISHED) {
		return;
	}

	ctx = sp_ud->ctx;

	REDIS_RETAIN (ctx);
	msg_debug ("timeout while querying redis server");
	lua_redis_push_error ("timeout while connecting the server", ctx, sp_ud, TRUE);

	if (sp_ud->c->ctx) {
		ac = sp_ud->c->ctx;
		/* Set to NULL to avoid double free in dtor */
		sp_ud->c->ctx = NULL;
		ac->err = REDIS_ERR_IO;
		errno = ETIMEDOUT;
		/*
		 * This will call all callbacks pending so the entire context
		 * will be destructed
		 */
		rspamd_redis_pool_release_connection (sp_ud->c->pool, ac, TRUE);
	}

	REDIS_RELEASE (ctx);
}


static void
lua_redis_parse_args (lua_State *L, gint idx, const gchar *cmd,
		gchar ***pargs, gsize **parglens, guint *nargs)
{
	gchar **args = NULL;
	gsize *arglens;
	gint top;

	if (idx != 0 && lua_type (L, idx) == LUA_TTABLE) {
		/* Get all arguments */
		lua_pushvalue (L, idx);
		lua_pushnil (L);
		top = 0;

		while (lua_next (L, -2) != 0) {
			gint type = lua_type (L, -1);

			if (type == LUA_TNUMBER || type == LUA_TSTRING ||
					type == LUA_TUSERDATA) {
				top ++;
			}
			lua_pop (L, 1);
		}

		args = g_slice_alloc ((top + 1) * sizeof (gchar *));
		arglens = g_slice_alloc ((top + 1) * sizeof (gsize));
		arglens[0] = strlen (cmd);
		args[0] = g_slice_alloc (arglens[0]);
		memcpy (args[0], cmd, arglens[0]);
		top = 1;
		lua_pushnil (L);

		while (lua_next (L, -2) != 0) {
			gint type = lua_type (L, -1);

			if (type == LUA_TSTRING) {
				const gchar *s;

				s = lua_tolstring (L, -1, &arglens[top]);
				args[top] = g_slice_alloc (arglens[top]);
				memcpy (args[top], s, arglens[top]);
				top ++;
			}
			else if (type == LUA_TUSERDATA) {
				struct rspamd_lua_text *t;

				t = lua_check_text (L, -1);

				if (t && t->start) {
					arglens[top] = t->len;
					args[top] = g_slice_alloc (arglens[top]);
					memcpy (args[top], t->start, arglens[top]);
					top ++;
				}
			}
			else if (type == LUA_TNUMBER) {
				gdouble val = lua_tonumber (L, -1);
				gint r;
				gchar numbuf[64];

				if (val == (gdouble)((gint64)val)) {
					r = rspamd_snprintf (numbuf, sizeof (numbuf), "%L",
							(gint64)val);
				}
				else {
					r = rspamd_snprintf (numbuf, sizeof (numbuf), "%f",
							val);
				}

				arglens[top] = r;
				args[top] = g_slice_alloc (arglens[top]);
				memcpy (args[top], numbuf, arglens[top]);
				top ++;
			}

			lua_pop (L, 1);
		}

		lua_pop (L, 1);
	}
	else {
		/* Use merely cmd */

		args = g_slice_alloc (sizeof (gchar *));
		arglens = g_slice_alloc (sizeof (gsize));
		arglens[0] = strlen (cmd);
		args[0] = g_slice_alloc (arglens[0]);
		memcpy (args[0], cmd, arglens[0]);
		top = 1;
	}

	*pargs = args;
	*parglens = arglens;
	*nargs = top;
}

static struct lua_redis_ctx *
rspamd_lua_redis_prepare_connection (lua_State *L, gint *pcbref)
{
	struct lua_redis_ctx *ctx;
	rspamd_inet_addr_t *ip = NULL;
	struct lua_redis_userdata *ud;
	struct rspamd_lua_ip *addr = NULL;
	struct rspamd_task *task = NULL;
	const gchar *host;
	const gchar *password = NULL, *dbname = NULL;
	gint cbref = -1;
	struct rspamd_config *cfg = NULL;
	struct rspamd_async_session *session = NULL;
	struct event_base *ev_base = NULL;
	gboolean ret = FALSE;
	guint flags = 0;

	if (lua_istable (L, 1)) {
		/* Table version */
		lua_pushvalue (L, 1);
		lua_pushstring (L, "task");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TUSERDATA) {
			task = lua_check_task_maybe (L, -1);
		}
		lua_pop (L, 1);

		if (!task) {
			/* We need to get ev_base, config and session separately */
			lua_pushstring (L, "config");
			lua_gettable (L, -2);
			if (lua_type (L, -1) == LUA_TUSERDATA) {
				cfg = lua_check_config (L, -1);
			}
			lua_pop (L, 1);

			lua_pushstring (L, "session");
			lua_gettable (L, -2);
			if (lua_type (L, -1) == LUA_TUSERDATA) {
				session = lua_check_session (L, -1);
			}
			lua_pop (L, 1);

			lua_pushstring (L, "ev_base");
			lua_gettable (L, -2);
			if (lua_type (L, -1) == LUA_TUSERDATA) {
				ev_base = lua_check_ev_base (L, -1);
			}
			lua_pop (L, 1);

			lua_pushstring (L, "ev_base");
			lua_gettable (L, -2);
			if (lua_type (L, -1) == LUA_TUSERDATA) {
				ev_base = lua_check_ev_base (L, -1);
			}
			lua_pop (L, 1);

			if (cfg && ev_base) {
				ret = TRUE;
			}
		}
		else {
			cfg = task->cfg;
			session = task->s;
			ev_base = task->ev_base;
			ret = TRUE;
		}

		if (pcbref) {
			lua_pushstring (L, "callback");
			lua_gettable (L, -2);
			if (lua_type (L, -1) == LUA_TFUNCTION) {
				/* This also pops function from the stack */
				cbref = luaL_ref (L, LUA_REGISTRYINDEX);
				*pcbref = cbref;
			}
			else {
				*pcbref = -1;
				lua_pop (L, 1);
			}
		}

		lua_pushstring (L, "host");
		lua_gettable (L, -2);

		if (lua_type (L, -1) == LUA_TUSERDATA) {
			addr = lua_check_ip (L, -1);
		}
		else if (lua_type (L, -1) == LUA_TSTRING) {
			host = lua_tostring (L, -1);

			if (rspamd_parse_inet_address (&ip, host, strlen (host))) {
				addr = g_alloca (sizeof (*addr));
				addr->addr = ip;

				if (rspamd_inet_address_get_port (ip) == 0) {
					rspamd_inet_address_set_port (ip, 6379);
				}
			}
		}
		lua_pop (L, 1);

		lua_pushstring (L, "password");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TSTRING) {
			password = lua_tostring (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "dbname");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TSTRING) {
			dbname = lua_tostring (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "opaque_data");
		lua_gettable (L, -2);
		if (!!lua_toboolean (L, -1)) {
			flags |= LUA_REDIS_TEXTDATA;
		}
		lua_pop (L, 1);

		lua_pop (L, 1); /* table */


		if (ret && addr != NULL) {
			ctx = g_slice_alloc0 (sizeof (struct lua_redis_ctx));
			REF_INIT_RETAIN (ctx, lua_redis_dtor);
			ctx->flags |= flags | LUA_REDIS_ASYNC;
			ud = &ctx->d.async;
			ud->s = session;
			ud->cfg = cfg;
			ud->pool = cfg->redis_pool;
			ud->ev_base = ev_base;
			ud->L = L;

			ret = TRUE;
		}
		else {
			if (cbref != -1) {
				luaL_unref (L, LUA_REGISTRYINDEX, cbref);
			}

			msg_err_task_check ("incorrect function invocation");
			ret = FALSE;
		}
	}

	if (ret) {
		ud->terminated = 0;
		ud->ctx = rspamd_redis_pool_connect (ud->pool,
				dbname, password,
				rspamd_inet_address_to_string (addr->addr),
				rspamd_inet_address_get_port (addr->addr));

		if (ip) {
			rspamd_inet_address_free (ip);
		}

		if (ud->ctx == NULL || ud->ctx->err) {
			if (ud->ctx) {
				msg_err_task_check ("cannot connect to redis: %s",
						ud->ctx->errstr);
				rspamd_redis_pool_release_connection (ud->pool, ud->ctx, TRUE);
				ud->ctx = NULL;
			}
			else {
				msg_err_task_check ("cannot connect to redis: unknown error");
			}

			REDIS_RELEASE (ctx);

			return NULL;
		}


		return ctx;
	}

	if (ip) {
		rspamd_inet_address_free (ip);
	}

	return NULL;
}

/***
 * @function rspamd_redis.make_request({params})
 * Make request to redis server, params is a table of key=value arguments in any order
 * @param {task} task worker task object
 * @param {ip|string} host server address
 * @param {function} callback callback to be called in form `function (task, err, data)`
 * @param {string} cmd command to be sent to redis
 * @param {table} args numeric array of strings used as redis arguments
 * @param {number} timeout timeout in seconds for request (1.0 by default)
 * @return {boolean} `true` if a request has been scheduled
 */
static int
lua_redis_make_request (lua_State *L)
{
	struct lua_redis_specific_userdata *sp_ud;
	struct lua_redis_userdata *ud;
	struct lua_redis_ctx *ctx, **pctx;
	const gchar *cmd = NULL;
	struct timeval tv;
	gdouble timeout = REDIS_DEFAULT_TIMEOUT;
	gint cbref = -1;
	gboolean ret = FALSE;

	ctx = rspamd_lua_redis_prepare_connection (L, &cbref);

	if (ctx) {
		ud = &ctx->d.async;
		sp_ud = g_slice_alloc0 (sizeof (*sp_ud));
		sp_ud->cbref = cbref;
		sp_ud->c = ud;
		sp_ud->ctx = ctx;

		lua_pushstring (L, "cmd");
		lua_gettable (L, -2);
		cmd = lua_tostring (L, -1);
		lua_pop (L, 1);

		lua_pushstring (L, "timeout");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TNUMBER) {
			timeout = lua_tonumber (L, -1);
		}
		lua_pop (L, 1);
		ud->timeout = timeout;


		lua_pushstring (L, "args");
		lua_gettable (L, -2);
		lua_redis_parse_args (L, -1, cmd, &sp_ud->args, &sp_ud->arglens,
				&sp_ud->nargs);
		lua_pop (L, 1);
		LL_PREPEND (ud->specific, sp_ud);
		ret = redisAsyncCommandArgv (ud->ctx,
				lua_redis_callback,
				sp_ud,
				sp_ud->nargs,
				(const gchar **)sp_ud->args,
				sp_ud->arglens);

		if (ret == REDIS_OK) {
			if (ud->s) {
				rspamd_session_add_event (ud->s,
						lua_redis_fin,
						sp_ud,
						g_quark_from_static_string ("lua redis"));
				sp_ud->w = rspamd_session_get_watcher (ud->s);
				rspamd_session_watcher_push (ud->s);
			}
			else {
				sp_ud->w = NULL;
			}

			REDIS_RETAIN (ctx); /* Cleared by fin event */
			ctx->cmds_pending ++;
			double_to_tv (timeout, &tv);
			event_set (&sp_ud->timeout, -1, EV_TIMEOUT, lua_redis_timeout, sp_ud);
			event_base_set (ud->ev_base, &sp_ud->timeout);
			event_add (&sp_ud->timeout, &tv);
			ret = TRUE;
		}
		else {
			msg_info ("call to redis failed: %s", ud->ctx->errstr);
			rspamd_redis_pool_release_connection (ud->pool, ud->ctx, TRUE);
			ud->ctx = NULL;
			REDIS_RELEASE (ctx);
			ret = FALSE;
		}
	}
	else {
		lua_pushboolean (L, FALSE);
		lua_pushnil (L);

		return 2;
	}

	lua_pushboolean (L, ret);

	if (ret) {
		pctx = lua_newuserdata (L, sizeof (ctx));
		*pctx = ctx;
		rspamd_lua_setclass (L, "rspamd{redis}", -1);
	}
	else {
		lua_pushnil (L);
	}

	return 2;
}

/***
 * @function rspamd_redis.make_request_sync({params})
 * Make blocking request to redis server, params is a table of key=value arguments in any order
 * @param {ip|string} host server address
 * @param {string} cmd command to be sent to redis
 * @param {table} args numeric array of strings used as redis arguments
 * @param {number} timeout timeout in seconds for request (1.0 by default)
 * @return {boolean + result} `true` and a result if a request has been successful
 */
static int
lua_redis_make_request_sync (lua_State *L)
{
	struct rspamd_lua_ip *addr = NULL;
	rspamd_inet_addr_t *ip = NULL;
	const gchar *cmd = NULL, *host;
	struct timeval tv;
	gboolean ret = FALSE;
	gdouble timeout = REDIS_DEFAULT_TIMEOUT;
	gchar **args = NULL;
	gsize *arglens = NULL;
	guint nargs = 0, flags = 0;
	redisContext *ctx;
	redisReply *r;

	if (lua_istable (L, 1)) {
		lua_pushvalue (L, 1);

		lua_pushstring (L, "cmd");
		lua_gettable (L, -2);
		cmd = lua_tostring (L, -1);
		lua_pop (L, 1);

		lua_pushstring (L, "host");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TUSERDATA) {
			addr = lua_check_ip (L, -1);
		}
		else if (lua_type (L, -1) == LUA_TSTRING) {
			host = lua_tostring (L, -1);
			if (rspamd_parse_inet_address (&ip, host, strlen (host))) {
				addr = g_alloca (sizeof (*addr));
				addr->addr = ip;

				if (rspamd_inet_address_get_port (ip) == 0) {
					rspamd_inet_address_set_port (ip, 6379);
				}
			}
		}
		lua_pop (L, 1);

		lua_pushstring (L, "timeout");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TNUMBER) {
			timeout = lua_tonumber (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "opaque_data");
		lua_gettable (L, -2);
		if (!!lua_toboolean (L, -1)) {
			flags |= LUA_REDIS_TEXTDATA;
		}
		lua_pop (L, 1);


		if (cmd) {
			lua_pushstring (L, "args");
			lua_gettable (L, -2);
			lua_redis_parse_args (L, -1, cmd, &args, &arglens, &nargs);
			lua_pop (L, 1);
		}

		lua_pop (L, 1);

		if (addr && cmd) {
			ret = TRUE;
		}
	}

	if (ret) {
		double_to_tv (timeout, &tv);
		ctx = redisConnectWithTimeout (rspamd_inet_address_to_string (addr->addr),
				rspamd_inet_address_get_port (addr->addr), tv);

		if (ip) {
			rspamd_inet_address_free (ip);
		}

		if (ctx == NULL || ctx->err) {
			redisFree (ctx);
			lua_redis_free_args (args, arglens, nargs);
			lua_pushboolean (L, FALSE);

			return 1;
		}

		r = redisCommandArgv (ctx,
					nargs,
					(const gchar **)args,
					arglens);

		if (r != NULL) {
			if (r->type != REDIS_REPLY_ERROR) {
				lua_pushboolean (L, TRUE);
				lua_redis_push_reply (L, r, flags & LUA_REDIS_TEXTDATA);
			}
			else {
				lua_pushboolean (L, FALSE);
				lua_pushstring (L, r->str);
			}

			freeReplyObject (r);
			redisFree (ctx);
			lua_redis_free_args (args, arglens, nargs);

			return 2;
		}
		else {
			msg_info ("call to redis failed: %s", ctx->errstr);
			redisFree (ctx);
			lua_redis_free_args (args, arglens, nargs);
			lua_pushboolean (L, FALSE);
		}
	}
	else {
		if (ip) {
			rspamd_inet_address_free (ip);
		}
		msg_err ("bad arguments for redis request");
		lua_pushboolean (L, FALSE);
	}

	return 1;
}

/***
 * @function rspamd_redis.connect({params})
 * Make request to redis server, params is a table of key=value arguments in any order
 * @param {task} task worker task object
 * @param {ip|string} host server address
 * @param {number} timeout timeout in seconds for request (1.0 by default)
 * @return {redis} new connection object or nil if connection failed
 */
static int
lua_redis_connect (lua_State *L)
{
	struct lua_redis_userdata *ud;
	struct lua_redis_ctx *ctx, **pctx;
	gdouble timeout = REDIS_DEFAULT_TIMEOUT;

	ctx = rspamd_lua_redis_prepare_connection (L, NULL);

	if (ctx) {
		ud = &ctx->d.async;

		lua_pushstring (L, "timeout");
		lua_gettable (L, 1);
		if (lua_type (L, -1) == LUA_TNUMBER) {
			timeout = lua_tonumber (L, -1);
		}

		lua_pop (L, 1);
		ud->timeout = timeout;
	}
	else {
		lua_pushboolean (L, FALSE);
		lua_pushnil (L);

		return 2;
	}

	pctx = lua_newuserdata (L, sizeof (ctx));
	*pctx = ctx;
	rspamd_lua_setclass (L, "rspamd{redis}", -1);

	return 1;
}

/***
 * @function rspamd_redis.connect_sync({params})
 * Make blocking request to redis server, params is a table of key=value arguments in any order
 * @param {ip|string} host server address
 * @param {number} timeout timeout in seconds for request (1.0 by default)
 * @return {redis} redis object if a request has been successful
 */
static int
lua_redis_connect_sync (lua_State *L)
{
	struct rspamd_lua_ip *addr = NULL;
	rspamd_inet_addr_t *ip = NULL;
	const gchar *host;
	struct timeval tv;
	gboolean ret = FALSE;
	guint flags = 0;
	gdouble timeout = REDIS_DEFAULT_TIMEOUT;
	struct lua_redis_ctx *ctx, **pctx;

	if (lua_istable (L, 1)) {
		lua_pushstring (L, "host");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TUSERDATA) {
			addr = lua_check_ip (L, -1);
		}
		else if (lua_type (L, -1) == LUA_TSTRING) {
			host = lua_tostring (L, -1);
			if (rspamd_parse_inet_address (&ip, host, strlen (host))) {
				addr = g_alloca (sizeof (*addr));
				addr->addr = ip;

				if (rspamd_inet_address_get_port (ip) == 0) {
					rspamd_inet_address_set_port (ip, 6379);
				}
			}
		}
		lua_pop (L, 1);

		lua_pushstring (L, "timeout");
		lua_gettable (L, -2);
		if (lua_type (L, -1) == LUA_TNUMBER) {
			timeout = lua_tonumber (L, -1);
		}
		lua_pop (L, 1);

		lua_pushstring (L, "opaque_data");
		lua_gettable (L, -2);
		if (!!lua_toboolean (L, -1)) {
			flags |= LUA_REDIS_TEXTDATA;
		}
		lua_pop (L, 1);

		if (addr) {
			ret = TRUE;
		}
	}

	if (ret) {
		double_to_tv (timeout, &tv);
		ctx = g_slice_alloc0 (sizeof (struct lua_redis_ctx));
		REF_INIT_RETAIN (ctx, lua_redis_dtor);
		ctx->flags = flags;
		ctx->d.sync = redisConnectWithTimeout (
				rspamd_inet_address_to_string (addr->addr),
				rspamd_inet_address_get_port (addr->addr), tv);

		if (ip) {
			rspamd_inet_address_free (ip);
		}

		if (ctx->d.sync == NULL || ctx->d.sync->err) {
			lua_pushboolean (L, FALSE);

			if (ctx->d.sync) {
				lua_pushstring (L, ctx->d.sync->errstr);
			}
			else {
				lua_pushstring (L, "unknown error");
			}

			REDIS_RELEASE (ctx);

			return 2;
		}

		pctx = lua_newuserdata (L, sizeof (ctx));
		*pctx = ctx;
		rspamd_lua_setclass (L, "rspamd{redis}", -1);

	}
	else {
		if (ip) {
			rspamd_inet_address_free (ip);
		}

		lua_pushboolean (L, FALSE);
		lua_pushstring (L, "bad arguments for redis request");
		return 2;
	}

	return 1;
}

/***
 * @method rspamd_redis:add_cmd(cmd, {args})
 * Append new cmd to redis pipeline
 * @param {string} cmd command to be sent to redis
 * @param {table} args array of strings used as redis arguments
 * @return {boolean} `true` if a request has been successful
 */
static int
lua_redis_add_cmd (lua_State *L)
{
	struct lua_redis_ctx *ctx = lua_check_redis (L, 1);
	struct lua_redis_specific_userdata *sp_ud;
	struct lua_redis_userdata *ud;
	const gchar *cmd = NULL;
	gint args_pos = 2;
	gchar **args = NULL;
	gsize *arglens = NULL;
	guint nargs = 0;
	gint cbref = -1, ret;
	struct timeval tv;

	if (ctx) {

		if (IS_ASYNC (ctx)) {
			ud = &ctx->d.async;

			/* Async version */
			if (lua_type (L, 2) == LUA_TSTRING) {
				/* No callback version */
				cmd = lua_tostring (L, 2);
				args_pos = 3;
			}
			else if (lua_type (L, 2) == LUA_TFUNCTION) {
				lua_pushvalue (L, 2);
				cbref = luaL_ref (L, LUA_REGISTRYINDEX);
				cmd = lua_tostring (L, 3);
				args_pos = 4;
			}
			else {
				return luaL_error (L, "invalid arguments");
			}

			sp_ud = g_slice_alloc0 (sizeof (*sp_ud));
			sp_ud->cbref = cbref;
			sp_ud->c = &ctx->d.async;
			sp_ud->ctx = ctx;

			lua_redis_parse_args (L, args_pos, cmd, &sp_ud->args,
						&sp_ud->arglens, &sp_ud->nargs);

			LL_PREPEND (sp_ud->c->specific, sp_ud);

			ret = redisAsyncCommandArgv (sp_ud->c->ctx,
					lua_redis_callback,
					sp_ud,
					sp_ud->nargs,
					(const gchar **)sp_ud->args,
					sp_ud->arglens);

			if (ret == REDIS_OK) {
				if (ud->s) {
					rspamd_session_add_event (ud->s,
							lua_redis_fin,
							sp_ud,
							g_quark_from_static_string ("lua redis"));
					sp_ud->w = rspamd_session_get_watcher (ud->s);
					rspamd_session_watcher_push (ud->s);
				}

				double_to_tv (sp_ud->c->timeout, &tv);
				event_set (&sp_ud->timeout, -1, EV_TIMEOUT, lua_redis_timeout, sp_ud);
				event_base_set (ud->ev_base, &sp_ud->timeout);
				event_add (&sp_ud->timeout, &tv);
				REDIS_RETAIN (ctx);
				ctx->cmds_pending ++;
			}
			else {
				msg_info ("call to redis failed: %s",
						sp_ud->c->ctx->errstr);
				lua_pushboolean (L, 0);
				lua_pushstring (L, sp_ud->c->ctx->errstr);

				return 2;
			}
		}
		else {
			/* Synchronous version */
			if (lua_type (L, 2) == LUA_TSTRING) {
				cmd = lua_tostring (L, 2);
				args_pos = 3;
			}
			else {
				return luaL_error (L, "invalid arguments");
			}

			if (ctx->d.sync) {
				lua_redis_parse_args (L, args_pos, cmd, &args, &arglens, &nargs);

				if (nargs > 0) {
					if (redisAppendCommandArgv (ctx->d.sync, nargs,
							(const char **)args, arglens) == REDIS_OK) {
						ctx->cmds_pending ++;
					}

					lua_redis_free_args (args, arglens, nargs);
				}
				else {
					lua_pushstring (L, "cannot append commands when not connected");
					return lua_error (L);
				}

			}
			else {
				lua_pushstring (L, "cannot append commands when not connected");
				return lua_error (L);
			}
		}
	}

	lua_pushboolean (L, true);

	return 1;
}

/***
 * @method rspamd_redis:exec()
 * Executes pending commands (suitable for blocking IO only for now)
 * @return {boolean}, {table}, ...: pairs in format [bool, result] for each request pending
 */
static int
lua_redis_exec (lua_State *L)
{
	struct lua_redis_ctx *ctx = lua_check_redis (L, 1);
	redisReply *r;
	gint ret;
	guint i, nret = 0;

	if (ctx == NULL) {
		lua_error (L);

		return 1;
	}

	if (IS_ASYNC (ctx)) {
		lua_pushstring (L, "Async redis pipelining is not implemented");
		lua_error (L);
		return 0;
	}
	else {
		if (!ctx->d.sync) {
			lua_pushstring (L, "cannot exec commands when not connected");
			lua_error (L);
			return 0;
		}
		else {
			if (!lua_checkstack (L, (ctx->cmds_pending * 2) + 1)) {
				return luaL_error (L, "cannot resiz stack to fit %d commands",
					ctx->cmds_pending);
			}
			for (i = 0; i < ctx->cmds_pending; i ++) {
				ret = redisGetReply (ctx->d.sync, (void **)&r);

				if (ret == REDIS_OK) {
					if (r->type != REDIS_REPLY_ERROR) {
						lua_pushboolean (L, TRUE);
						lua_redis_push_reply (L, r,
								ctx->flags & LUA_REDIS_TEXTDATA);
					}
					else {
						lua_pushboolean (L, FALSE);
						lua_pushlstring (L, r->str, r->len);
					}
					freeReplyObject (r);
				}
				else {
					msg_info ("call to redis failed: %s", ctx->d.sync->errstr);
					lua_pushboolean (L, FALSE);
					lua_pushstring (L, ctx->d.sync->errstr);
				}

				nret += 2;
			}
		}
	}

	return nret;
}
#else
static int
lua_redis_make_request (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_make_request_sync (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_connect (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_connect_sync (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_add_cmd (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_exec (lua_State *L)
{
	msg_warn ("rspamd is compiled with no redis support");

	lua_pushboolean (L, FALSE);

	return 1;
}
static int
lua_redis_gc (lua_State *L)
{
	return 0;
}
#endif

static gint
lua_load_redis (lua_State * L)
{
	lua_newtable (L);
	luaL_register (L, NULL, redislib_f);

	return 1;
}
/**
 * Open redis library
 * @param L lua stack
 * @return
 */
void
luaopen_redis (lua_State * L)
{
	luaL_newmetatable (L, "rspamd{redis}");
	lua_pushstring (L, "__index");
	lua_pushvalue (L, -2);
	lua_settable (L, -3);

	lua_pushstring (L, "class");
	lua_pushstring (L, "rspamd{redis}");
	lua_rawset (L, -3);

	luaL_register (L, NULL, redislib_m);
	lua_pop (L, 1);

	rspamd_lua_add_preload (L, "rspamd_redis", lua_load_redis);
}
