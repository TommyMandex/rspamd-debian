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

#include "config.h"
#include "stat_api.h"
#include "rspamd.h"
#include "cfg_rcl.h"
#include "stat_internal.h"
#include "lua/lua_common.h"

static struct rspamd_stat_ctx *stat_ctx = NULL;

static struct rspamd_stat_classifier lua_classifier = {
	.name = "lua",
	.init_func = lua_classifier_init,
	.classify_func = lua_classifier_classify,
	.learn_spam_func = lua_classifier_learn_spam,
};

static struct rspamd_stat_classifier stat_classifiers[] = {
	{
		.name = "bayes",
		.init_func = bayes_init,
		.classify_func = bayes_classify,
		.learn_spam_func = bayes_learn_spam,
	}
};

static struct rspamd_stat_tokenizer stat_tokenizers[] = {
	{
		.name = "osb-text",
		.get_config = rspamd_tokenizer_osb_get_config,
		.tokenize_func = rspamd_tokenizer_osb,
	},
	{
		.name = "osb",
		.get_config = rspamd_tokenizer_osb_get_config,
		.tokenize_func = rspamd_tokenizer_osb,
	},
};

#define RSPAMD_STAT_BACKEND_ELT(nam, eltn) { \
		.name = #nam, \
		.init = rspamd_##eltn##_init, \
		.runtime = rspamd_##eltn##_runtime, \
		.process_tokens = rspamd_##eltn##_process_tokens, \
		.finalize_process = rspamd_##eltn##_finalize_process, \
		.learn_tokens = rspamd_##eltn##_learn_tokens, \
		.finalize_learn = rspamd_##eltn##_finalize_learn, \
		.total_learns = rspamd_##eltn##_total_learns, \
		.inc_learns = rspamd_##eltn##_inc_learns, \
		.dec_learns = rspamd_##eltn##_dec_learns, \
		.get_stat = rspamd_##eltn##_get_stat, \
		.load_tokenizer_config = rspamd_##eltn##_load_tokenizer_config, \
		.close = rspamd_##eltn##_close \
	}

static struct rspamd_stat_backend stat_backends[] = {
		RSPAMD_STAT_BACKEND_ELT(mmap, mmaped_file),
		RSPAMD_STAT_BACKEND_ELT(sqlite3, sqlite3),
#ifdef WITH_HIREDIS
		RSPAMD_STAT_BACKEND_ELT(redis, redis)
#endif
};

#define RSPAMD_STAT_CACHE_ELT(nam, eltn) { \
		.name = #nam, \
		.init = rspamd_stat_cache_##eltn##_init, \
		.runtime = rspamd_stat_cache_##eltn##_runtime, \
		.check = rspamd_stat_cache_##eltn##_check, \
		.learn = rspamd_stat_cache_##eltn##_learn, \
		.close = rspamd_stat_cache_##eltn##_close \
	}

static struct rspamd_stat_cache stat_caches[] = {
		RSPAMD_STAT_CACHE_ELT(sqlite3, sqlite3),
#ifdef WITH_HIREDIS
		RSPAMD_STAT_CACHE_ELT(redis, redis),
#endif
};

void
rspamd_stat_init (struct rspamd_config *cfg, struct event_base *ev_base)
{
	GList *cur, *curst;
	struct rspamd_classifier_config *clf;
	struct rspamd_statfile_config *stf;
	struct rspamd_stat_backend *bk;
	struct rspamd_statfile *st;
	struct rspamd_classifier *cl;
	const ucl_object_t *cache_obj = NULL, *cache_name_obj;
	const gchar *cache_name = NULL;
	lua_State *L = cfg->lua_state;
	guint lua_classifiers_cnt = 0, i;

	if (stat_ctx == NULL) {
		stat_ctx = g_slice_alloc0 (sizeof (*stat_ctx));
	}

	lua_getglobal (L, "rspamd_classifiers");

	if (lua_type (L, -1) == LUA_TTABLE) {
		lua_pushnil (L);

		while (lua_next (L, -2) != 0) {
			lua_classifiers_cnt ++;
			lua_pop (L, 1);
		}
	}

	lua_pop (L, 1);

	stat_ctx->classifiers_count = G_N_ELEMENTS (stat_classifiers) +
				lua_classifiers_cnt;
	stat_ctx->classifiers_subrs = g_new0 (struct rspamd_stat_classifier,
			stat_ctx->classifiers_count);

	for (i = 0; i < G_N_ELEMENTS (stat_classifiers); i ++) {
		memcpy (&stat_ctx->classifiers_subrs[i], &stat_classifiers[i],
				sizeof (struct rspamd_stat_classifier));
	}

	lua_getglobal (L, "rspamd_classifiers");

	if (lua_type (L, -1) == LUA_TTABLE) {
		lua_pushnil (L);

		while (lua_next (L, -2) != 0) {
			lua_pushvalue (L, -2);
			memcpy (&stat_ctx->classifiers_subrs[i], &lua_classifier,
							sizeof (struct rspamd_stat_classifier));
			stat_ctx->classifiers_subrs[i].name = g_strdup (lua_tostring (L, -1));
			i ++;
			lua_pop (L, 2);
		}
	}

	lua_pop (L, 1);
	stat_ctx->backends_subrs = stat_backends;
	stat_ctx->backends_count = G_N_ELEMENTS (stat_backends);

	stat_ctx->tokenizers_subrs = stat_tokenizers;
	stat_ctx->tokenizers_count = G_N_ELEMENTS (stat_tokenizers);
	stat_ctx->caches_subrs = stat_caches;
	stat_ctx->caches_count = G_N_ELEMENTS (stat_caches);
	stat_ctx->cfg = cfg;
	stat_ctx->statfiles = g_ptr_array_new ();
	stat_ctx->classifiers = g_ptr_array_new ();
	stat_ctx->async_elts = g_queue_new ();
	stat_ctx->ev_base = ev_base;

	/* Create statfiles from the classifiers */
	cur = cfg->classifiers;

	while (cur) {
		bk = NULL;
		clf = cur->data;
		cl = g_slice_alloc0 (sizeof (*cl));
		cl->cfg = clf;
		cl->ctx = stat_ctx;
		cl->statfiles_ids = g_array_new (FALSE, FALSE, sizeof (gint));
		cl->subrs = rspamd_stat_get_classifier (clf->classifier);

		if (cl->subrs == NULL) {
			g_slice_free1 (sizeof (*cl), cl);
			msg_err_config ("cannot init classifier type %s", clf->name);
			cur = g_list_next (cur);
			continue;
		}

		if (!cl->subrs->init_func (cfg->cfg_pool, cl)) {
			g_slice_free1 (sizeof (*cl), cl);
			msg_err_config ("cannot init classifier type %s", clf->name);
			cur = g_list_next (cur);
			continue;
		}

		if (!(clf->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND)) {
			bk = rspamd_stat_get_backend (clf->backend);

			if (bk == NULL) {
				msg_err_config ("cannot get backend of type %s, so disable classifier"
						" %s completely", clf->backend, clf->name);
				cur = g_list_next (cur);
				continue;
			}
		}

		/* XXX:
		 * Here we get the first classifier tokenizer config as the only one
		 * We NO LONGER support multiple tokenizers per rspamd instance
		 */
		if (stat_ctx->tkcf == NULL) {
			stat_ctx->tokenizer = rspamd_stat_get_tokenizer (clf->tokenizer->name);
			g_assert (stat_ctx->tokenizer != NULL);
			stat_ctx->tkcf = stat_ctx->tokenizer->get_config (cfg->cfg_pool,
					clf->tokenizer, NULL);
		}

		/* Init classifier cache */
		cache_name = NULL;

		if (clf->opts) {
			cache_obj = ucl_object_lookup (clf->opts, "cache");
			cache_name_obj = NULL;

			if (cache_obj) {
				cache_name_obj = ucl_object_lookup_any (cache_obj,
						"name", "type", NULL);
			}

			if (cache_name_obj) {
				cache_name = ucl_object_tostring (cache_name_obj);
			}
		}

		if (cache_name == NULL) {
			/* We assume that learn cache is the same as backend */
			cache_name = clf->backend;
		}

		curst = clf->statfiles;

		while (curst) {
			stf = curst->data;
			st = g_slice_alloc0 (sizeof (*st));
			st->classifier = cl;
			st->stcf = stf;

			if (!(cl->cfg->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND) && bk) {
				st->backend = bk;
				st->bkcf = bk->init (stat_ctx, cfg, st);
				msg_debug_config ("added backend %s for symbol %s",
						bk->name, stf->symbol);
			}
			else {
				msg_debug_config ("added backend-less statfile for symbol %s",
						stf->symbol);
			}

			/* XXX: bad hack to pass statfiles configuration to cache */
			if (cl->cache == NULL) {
				cl->cache = rspamd_stat_get_cache (cache_name);
				g_assert (cl->cache != NULL);
				cl->cachecf = cl->cache->init (stat_ctx, cfg, st, cache_obj);

				if (cl->cachecf == NULL) {
					msg_err_config ("error adding cache %s for symbol %s",
							cl->cache->name, stf->symbol);
					cl->cache = NULL;
				}
				else {
					msg_debug_config ("added cache %s for symbol %s",
							cl->cache->name, stf->symbol);
				}
			}

			if (st->bkcf == NULL &&
					!(cl->cfg->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND)) {
				msg_err_config ("cannot init backend %s for statfile %s",
						clf->backend, stf->symbol);

				g_slice_free1 (sizeof (*st), st);
			}
			else {
				st->id = stat_ctx->statfiles->len;
				g_ptr_array_add (stat_ctx->statfiles, st);
				g_array_append_val (cl->statfiles_ids, st->id);
			}

			curst = curst->next;
		}

		g_ptr_array_add (stat_ctx->classifiers, cl);

		cur = cur->next;
	}
}

void
rspamd_stat_close (void)
{
	struct rspamd_classifier *cl;
	struct rspamd_statfile *st;
	struct rspamd_stat_ctx *st_ctx;
	struct rspamd_stat_async_elt *aelt;
	GList *cur;
	guint i, j;
	gint id;

	st_ctx = rspamd_stat_get_ctx ();
	g_assert (st_ctx != NULL);

	for (i = 0; i < st_ctx->classifiers->len; i ++) {
		cl = g_ptr_array_index (st_ctx->classifiers, i);

		for (j = 0; j < cl->statfiles_ids->len; j ++) {
			id = g_array_index (cl->statfiles_ids, gint, j);
			st = g_ptr_array_index (st_ctx->statfiles, id);
			if (!(st->classifier->cfg->flags & RSPAMD_FLAG_CLASSIFIER_NO_BACKEND)) {
				st->backend->close (st->bkcf);
			}

			g_slice_free1 (sizeof (*st), st);
		}

		if (cl->cache && cl->cachecf) {
			cl->cache->close (cl->cachecf);
		}

		g_array_free (cl->statfiles_ids, TRUE);
		g_slice_free1 (sizeof (*cl), cl);
	}

	cur = st_ctx->async_elts->head;

	while (cur) {
		aelt = cur->data;
		REF_RELEASE (aelt);
		cur = g_list_next (cur);
	}

	g_queue_free (stat_ctx->async_elts);
	g_ptr_array_free (st_ctx->statfiles, TRUE);
	g_ptr_array_free (st_ctx->classifiers, TRUE);
	g_slice_free1 (sizeof (*st_ctx), st_ctx);

	/* Set global var to NULL */
	stat_ctx = NULL;
}

struct rspamd_stat_ctx *
rspamd_stat_get_ctx (void)
{
	return stat_ctx;
}

struct rspamd_stat_classifier *
rspamd_stat_get_classifier (const gchar *name)
{
	guint i;

	if (name == NULL || name[0] == '\0') {
		name = RSPAMD_DEFAULT_CLASSIFIER;
	}

	for (i = 0; i < stat_ctx->classifiers_count; i ++) {
		if (strcmp (name, stat_ctx->classifiers_subrs[i].name) == 0) {
			return &stat_ctx->classifiers_subrs[i];
		}
	}

	msg_err ("cannot find classifier named %s", name);

	return NULL;
}

struct rspamd_stat_backend *
rspamd_stat_get_backend (const gchar *name)
{
	guint i;

	if (name == NULL || name[0] == '\0') {
		name = RSPAMD_DEFAULT_BACKEND;
	}

	for (i = 0; i < stat_ctx->backends_count; i ++) {
		if (strcmp (name, stat_ctx->backends_subrs[i].name) == 0) {
			return &stat_ctx->backends_subrs[i];
		}
	}

	msg_err ("cannot find backend named %s", name);

	return NULL;
}

struct rspamd_stat_tokenizer *
rspamd_stat_get_tokenizer (const gchar *name)
{
	guint i;

	if (name == NULL || name[0] == '\0') {
		name = RSPAMD_DEFAULT_TOKENIZER;
	}

	for (i = 0; i < stat_ctx->tokenizers_count; i ++) {
		if (strcmp (name, stat_ctx->tokenizers_subrs[i].name) == 0) {
			return &stat_ctx->tokenizers_subrs[i];
		}
	}

	msg_err ("cannot find tokenizer named %s", name);

	return NULL;
}

struct rspamd_stat_cache *
rspamd_stat_get_cache (const gchar *name)
{
	guint i;

	if (name == NULL || name[0] == '\0') {
		name = RSPAMD_DEFAULT_CACHE;
	}

	for (i = 0; i < stat_ctx->caches_count; i++) {
		if (strcmp (name, stat_ctx->caches_subrs[i].name) == 0) {
			return &stat_ctx->caches_subrs[i];
		}
	}

	msg_err ("cannot find cache named %s", name);

	return NULL;
}

static void
rspamd_async_elt_dtor (struct rspamd_stat_async_elt *elt)
{
	if (elt->cleanup) {
		elt->cleanup (elt, elt->ud);
	}

	event_del (&elt->timer_ev);
	g_slice_free1 (sizeof (*elt), elt);
}

static void
rspamd_async_elt_on_timer (gint fd, short what, gpointer d)
{
	struct rspamd_stat_async_elt *elt = d;
	gdouble jittered_time;

	event_del (&elt->timer_ev);

	if (elt->enabled) {
		elt->handler (elt, elt->ud);
	}

	jittered_time = rspamd_time_jitter (elt->timeout, 0);
	double_to_tv (jittered_time, &elt->tv);
	event_add (&elt->timer_ev, &elt->tv);
}

struct rspamd_stat_async_elt*
rspamd_stat_ctx_register_async (rspamd_stat_async_handler handler,
		rspamd_stat_async_cleanup cleanup,
		gpointer d,
		gdouble timeout)
{
	struct rspamd_stat_async_elt *elt;
	struct rspamd_stat_ctx *st_ctx;

	st_ctx = rspamd_stat_get_ctx ();
	g_assert (st_ctx != NULL);

	elt = g_slice_alloc (sizeof (*elt));
	REF_INIT_RETAIN (elt, rspamd_async_elt_dtor);
	elt->handler = handler;
	elt->cleanup = cleanup;
	elt->ud = d;
	elt->timeout = timeout;
	/* Enabled by default */
	elt->enabled = TRUE;

	event_set (&elt->timer_ev, -1, EV_TIMEOUT, rspamd_async_elt_on_timer, elt);
	event_base_set (st_ctx->ev_base, &elt->timer_ev);
	/*
	 * First we set timeval to zero as we want cb to be executed as
	 * fast as possible
	 */
	elt->tv.tv_sec = 0;
	elt->tv.tv_usec = 0;
	event_add (&elt->timer_ev, &elt->tv);

	g_queue_push_tail (st_ctx->async_elts, elt);

	return elt;
}
