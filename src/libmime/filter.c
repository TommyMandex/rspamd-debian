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
#include "mem_pool.h"
#include "filter.h"
#include "rspamd.h"
#include "message.h"
#include "lua/lua_common.h"
#include <math.h>
#include "contrib/uthash/utlist.h"


#define COMMON_PART_FACTOR 95

struct rspamd_metric_result *
rspamd_create_metric_result (struct rspamd_task *task)
{
	struct rspamd_metric_result *metric_res;
	struct rspamd_metric *metric;
	guint i;

	metric_res = task->result;

	if (metric_res != NULL) {
		return metric_res;
	}

	metric = task->cfg->default_metric;

	if (metric == NULL) {
		return NULL;
	}

	metric_res =
			rspamd_mempool_alloc (task->task_pool,
					sizeof (struct rspamd_metric_result));
	metric_res->symbols = g_hash_table_new (rspamd_str_hash,
			rspamd_str_equal);
	rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t) g_hash_table_unref,
			metric_res->symbols);
	metric_res->sym_groups = g_hash_table_new (g_direct_hash, g_direct_equal);
	rspamd_mempool_add_destructor (task->task_pool,
			(rspamd_mempool_destruct_t) g_hash_table_unref,
			metric_res->sym_groups);
	metric_res->metric = metric;
	metric_res->grow_factor = 0;
	metric_res->score = 0;

	for (i = 0; i < METRIC_ACTION_MAX; i++) {
		metric_res->actions_limits[i] = metric->actions[i].score;
	}

	metric_res->action = METRIC_ACTION_MAX;

	return metric_res;
}

static inline gdouble
rspamd_check_group_score (struct rspamd_task *task,
		const gchar *symbol,
		struct rspamd_symbols_group *gr,
		gdouble *group_score,
		gdouble w)
{
	if (gr != NULL && group_score && gr->max_score > 0.0 && w > 0.0) {
		if (*group_score >= gr->max_score && w > 0) {
			msg_info_task ("maximum group score %.2f for group %s has been reached,"
					" ignoring symbol %s with weight %.2f", gr->max_score,
					gr->name, symbol, w);
			return NAN;
		}
		else if (*group_score + w > gr->max_score) {
			w = gr->max_score - *group_score;
		}
	}

	return w;
}

static struct rspamd_symbol_result *
insert_metric_result (struct rspamd_task *task,
	struct rspamd_metric *metric,
	const gchar *symbol,
	double flag,
	const gchar *opt,
	gboolean single)
{
	struct rspamd_metric_result *metric_res;
	struct rspamd_symbol_result *s = NULL;
	gdouble w, *gr_score = NULL, next_gf = 1.0, diff;
	struct rspamd_symbol *sdef;
	struct rspamd_symbols_group *gr = NULL;
	const ucl_object_t *mobj, *sobj;
	gint max_shots;

	metric_res = rspamd_create_metric_result (task);

	sdef = g_hash_table_lookup (metric->symbols, symbol);
	if (sdef == NULL) {
		w = 0.0;
	}
	else {
		w = (*sdef->weight_ptr) * flag;
		gr = sdef->gr;

		if (gr != NULL) {
			gr_score = g_hash_table_lookup (metric_res->sym_groups, gr);

			if (gr_score == NULL) {
				gr_score = rspamd_mempool_alloc (task->task_pool, sizeof (gdouble));
				*gr_score = 0;
				g_hash_table_insert (metric_res->sym_groups, gr, gr_score);
			}
		}
	}

	if (task->settings) {
		mobj = task->settings;
		gdouble corr;

		sobj = ucl_object_lookup (mobj, symbol);
		if (sobj != NULL && ucl_object_todouble_safe (sobj, &corr)) {
			msg_debug ("settings: changed weight of symbol %s from %.2f to %.2f",
					symbol, w, corr);
			w = corr * flag;
		}
	}

	/* Add metric score */
	if ((s = g_hash_table_lookup (metric_res->symbols, symbol)) != NULL) {
		if (single) {
			max_shots = 1;
		}
		else {
			if (sdef) {
				max_shots = sdef->nshots;
			}
			else {
				max_shots = task->cfg->default_max_shots;
			}
		}

		if (!single && (max_shots > 0 && (s->nshots >= max_shots))) {
			single = TRUE;
		}

		/* Now check for the duplicate options */
		if (opt && s->options && g_hash_table_lookup (s->options, opt)) {
			single = TRUE;
		}
		else {
			s->nshots ++;
			rspamd_task_add_result_option (task, s, opt);
		}

		/* Adjust diff */
		if (!single) {
			diff = w;
		}
		else {
			if (fabs (s->score) < fabs (w) && signbit (s->score) == signbit (w)) {
				/* Replace less significant weight with a more significant one */
				diff = w - s->score;
			}
			else {
				diff = 0;
			}
		}

		if (diff) {
			/* Handle grow factor */
			if (metric_res->grow_factor && diff > 0) {
				diff *= metric_res->grow_factor;
				next_gf *= metric->grow_factor;
			}
			else if (diff > 0) {
				next_gf = metric->grow_factor;
			}

			diff = rspamd_check_group_score (task, symbol, gr, gr_score, diff);

			if (!isnan (diff)) {
				metric_res->score += diff;
				metric_res->grow_factor = next_gf;

				if (gr_score) {
					*gr_score += diff;
				}

				if (single) {
					s->score = w;
				}
				else {
					s->score += diff;
				}
			}
		}
	}
	else {
		s = rspamd_mempool_alloc0 (task->task_pool, sizeof (struct rspamd_symbol_result));

		/* Handle grow factor */
		if (metric_res->grow_factor && w > 0) {
			w *= metric_res->grow_factor;
			next_gf *= metric->grow_factor;
		}
		else if (w > 0) {
			next_gf = metric->grow_factor;
		}

		s->name = symbol;
		s->sym = sdef;
		s->nshots = 1;

		w = rspamd_check_group_score (task, symbol, gr, gr_score, w);

		if (!isnan (w)) {
			metric_res->score += w;
			metric_res->grow_factor = next_gf;
			s->score = w;

			if (gr_score) {
				*gr_score += w;
			}

		}
		else {
			s->score = 0;
		}

		rspamd_task_add_result_option (task, s, opt);
		g_hash_table_insert (metric_res->symbols, (gpointer) symbol, s);
	}

	msg_debug_task ("symbol %s, score %.2f, metric %s, factor: %f",
		symbol,
		s->score,
		metric->name,
		w);

	return s;
}

static struct rspamd_symbol_result *
insert_result_common (struct rspamd_task *task,
	const gchar *symbol,
	double flag,
	const gchar *opt,
	gboolean single)
{
	struct rspamd_metric *metric;
	GList *cur, *metric_list;
	struct rspamd_symbol_result *s = NULL;

	metric_list = g_hash_table_lookup (task->cfg->metrics_symbols, symbol);
	if (metric_list) {
		cur = metric_list;

		while (cur) {
			metric = cur->data;
			s = insert_metric_result (task, metric, symbol, flag, opt, single);
			cur = g_list_next (cur);
		}
	}
	else {
		/* Insert symbol to default metric */
		s = insert_metric_result (task,
			task->cfg->default_metric,
			symbol,
			flag,
			opt,
			single);
	}

	/* Process cache item */
	if (task->cfg->cache) {
		rspamd_symbols_cache_inc_frequency (task->cfg->cache, symbol);
	}

	return s;
}

/* Insert result that may be increased on next insertions */
struct rspamd_symbol_result *
rspamd_task_insert_result (struct rspamd_task *task,
	const gchar *symbol,
	double flag,
	const gchar *opt)
{
	return insert_result_common (task, symbol, flag, opt,
			FALSE);
}

/* Insert result as a single option */
struct rspamd_symbol_result *
rspamd_task_insert_result_single (struct rspamd_task *task,
	const gchar *symbol,
	double flag,
	const gchar *opt)
{
	return insert_result_common (task, symbol, flag, opt, TRUE);
}

gboolean
rspamd_task_add_result_option (struct rspamd_task *task,
		struct rspamd_symbol_result *s, const gchar *val)
{
	struct rspamd_symbol_option *opt;
	gboolean ret = FALSE;

	if (s && val) {
		if (s->options && !(s->sym &&
				(s->sym->flags & RSPAMD_SYMBOL_FLAG_ONEPARAM)) &&
				g_hash_table_size (s->options) < task->cfg->default_max_shots) {
			/* Append new options */
			if (!g_hash_table_lookup (s->options, val)) {
				opt = rspamd_mempool_alloc (task->task_pool, sizeof (*opt));
				opt->option = rspamd_mempool_strdup (task->task_pool, val);
				DL_APPEND (s->opts_head, opt);

				g_hash_table_insert (s->options, opt->option, opt);
				ret = TRUE;
			}
		}
		else {
			s->options = g_hash_table_new (rspamd_strcase_hash,
					rspamd_strcase_equal);
			rspamd_mempool_add_destructor (task->task_pool,
					(rspamd_mempool_destruct_t)g_hash_table_unref,
					s->options);
			opt = rspamd_mempool_alloc (task->task_pool, sizeof (*opt));
			opt->option = rspamd_mempool_strdup (task->task_pool, val);
			s->opts_head = NULL;
			DL_APPEND (s->opts_head, opt);

			g_hash_table_insert (s->options, opt->option, opt);
			ret = TRUE;
		}
	}
	else if (!val) {
		ret = TRUE;
	}

	return ret;
}

enum rspamd_metric_action
rspamd_check_action_metric (struct rspamd_task *task, struct rspamd_metric_result *mres)
{
	struct metric_action *action, *selected_action = NULL;
	double max_score = 0, sc;
	int i;

	if (task->pre_result.action == METRIC_ACTION_MAX) {
		for (i = METRIC_ACTION_REJECT; i < METRIC_ACTION_MAX; i++) {
			action = &mres->metric->actions[i];
			sc = mres->actions_limits[i];

			if (isnan (sc)) {
				continue;
			}

			if (mres->score >= sc && sc > max_score) {
				selected_action = action;
				max_score = sc;
			}
		}
	}
	else {
		sc = NAN;

		for (i = task->pre_result.action; i < METRIC_ACTION_MAX; i ++) {
			selected_action = &mres->metric->actions[i];
			sc = mres->actions_limits[i];

			if (isnan (sc)) {
				if (i == task->pre_result.action) {
					/* No scores defined, just avoid NaN */
					sc = 0;
					break;
				}
			}
			else {
				break;
			}
		}

		if (!isnan (sc)) {
			mres->score = sc;
		}
		else {
			mres->score = 0;
		}
	}

	if (selected_action) {
		return selected_action->action;
	}

	return METRIC_ACTION_NOACTION;
}
