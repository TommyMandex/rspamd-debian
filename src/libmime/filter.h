/**
 * @file filter.h
 * Filters logic implementation
 */

#ifndef RSPAMD_FILTER_H
#define RSPAMD_FILTER_H

#include "config.h"
#include "symbols_cache.h"
#include "task.h"

struct rspamd_task;
struct rspamd_settings;
struct rspamd_classifier_config;

struct rspamd_symbol_option {
	gchar *option;
	struct rspamd_symbol_option *prev, *next;
};

/**
 * Rspamd symbol
 */
struct rspamd_symbol_result {
	double score;                                   /**< symbol's score							*/
	GHashTable *options;                            /**< list of symbol's options				*/
	struct rspamd_symbol_option *opts_head;        /**< head of linked list of options			*/
	const gchar *name;
	struct rspamd_symbol *sym;						/**< symbol configuration					*/
	guint nshots;
};

/**
 * Result of metric processing
 */
struct rspamd_metric_result {
	struct rspamd_metric *metric;                   /**< pointer to metric structure			*/
	double score;                                   /**< total score							*/
	double grow_factor;								/**< current grow factor					*/
	GHashTable *symbols;                            /**< symbols of metric						*/
	GHashTable *sym_groups;							/**< groups of symbols						*/
	gdouble actions_limits[METRIC_ACTION_MAX];		/**< set of actions for this metric			*/
	enum rspamd_metric_action action;               /**< the current action						*/
};

/**
 * Create or return existing result for the specified metric name
 * @param task task object
 * @return metric result or NULL if metric `name` has not been found
 */
struct rspamd_metric_result * rspamd_create_metric_result (struct rspamd_task *task);

/**
 * Insert a result to task
 * @param task worker's task that present message from user
 * @param metric_name metric's name to which we need to insert result
 * @param symbol symbol to insert
 * @param flag numeric weight for symbol
 * @param opts list of symbol's options
 */
struct rspamd_symbol_result* rspamd_task_insert_result (struct rspamd_task *task,
	const gchar *symbol,
	double flag,
	const gchar *opts);

/**
 * Insert a single result to task
 * @param task worker's task that present message from user
 * @param metric_name metric's name to which we need to insert result
 * @param symbol symbol to insert
 * @param flag numeric weight for symbol
 * @param opts list of symbol's options
 */
struct rspamd_symbol_result* rspamd_task_insert_result_single (struct rspamd_task *task,
	const gchar *symbol,
	double flag,
	const gchar *opts);


/**
 * Adds new option to symbol
 * @param task
 * @param s
 * @param opt
 */
gboolean rspamd_task_add_result_option (struct rspamd_task *task,
		struct rspamd_symbol_result *s, const gchar *opt);

/**
 * Default consolidation function for metric, it get all symbols and multiply symbol
 * weight by some factor that is specified in config. Default factor is 1.
 * @param task worker's task that present message from user
 * @param metric_name name of metric
 * @return result metric weight
 */
double rspamd_factor_consolidation_func (struct rspamd_task *task,
	const gchar *metric_name,
	const gchar *unused);


/*
 * Get action for specific metric
 */
enum rspamd_metric_action rspamd_check_action_metric (struct rspamd_task *task,
	struct rspamd_metric_result *mres);

#endif
