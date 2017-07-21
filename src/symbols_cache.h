#ifndef RSPAMD_SYMBOLS_CACHE_H
#define RSPAMD_SYMBOLS_CACHE_H

#include "config.h"
#include "radix.h"

#define MAX_SYMBOL 128

struct worker_task;
struct config_file;

typedef void (*symbol_func_t)(struct worker_task *task, gpointer user_data);

struct saved_cache_item {
	gchar symbol[MAX_SYMBOL];
	double weight;
	guint32 frequency;
	double avg_time;
};

struct dynamic_map_item {
	struct in_addr addr;
	guint32 mask;
	gboolean negative;
};

struct cache_item {
	/* Static item's data */
	struct saved_cache_item *s;

	/* For dynamic rules */
	struct dynamic_map_item *networks;
	guint32 networks_number;
	gboolean is_dynamic;
	
	/* Callback data */
	symbol_func_t func;
	gpointer user_data;

	/* Flags of virtual symbols */
	gboolean is_virtual;
	gboolean is_callback;

	/* Priority */
	gint priority;
	gdouble metric_weight;
};


struct symbols_cache {
	/* Normal cache items */
	GList *static_items;

	/* Items that have negative weights */
	GList *negative_items;
	
	/* Radix map of dynamic rules with ip mappings */
	radix_tree_t *dynamic_map;
	radix_tree_t *negative_dynamic_map;

	/* Common dynamic rules */
	GList *dynamic_items;

	/* Hash table for fast access */
	GHashTable *items_by_symbol;

	memory_pool_t *static_pool;

	guint cur_items;
	guint used_items;
	guint uses;
	gpointer map;
	memory_pool_rwlock_t *lock;
	struct config_file *cfg;
};

/**
 * Load symbols cache from file, must be called _after_ init_symbols_cache
 */
gboolean init_symbols_cache (memory_pool_t *pool, struct symbols_cache *cache, struct config_file *cfg,
		const gchar *filename, gboolean ignore_checksum);

/**
 * Register function for symbols parsing
 * @param name name of symbol
 * @param func pointer to handler
 * @param user_data pointer to user_data
 */
void register_symbol (struct symbols_cache **cache, const gchar *name, double weight,
		symbol_func_t func, gpointer user_data);


/**
 * Register virtual symbol
 * @param name name of symbol
 */
void register_virtual_symbol (struct symbols_cache **cache, const gchar *name, double weight);

/**
 * Register callback function for symbols parsing
 * @param name name of symbol
 * @param func pointer to handler
 * @param user_data pointer to user_data
 */
void register_callback_symbol (struct symbols_cache **cache, const gchar *name, double weight,
		symbol_func_t func, gpointer user_data);

/**
 * Register function for symbols parsing with strict priority
 * @param name name of symbol
 * @param func pointer to handler
 * @param user_data pointer to user_data
 */
void register_callback_symbol_priority (struct symbols_cache **cache, const gchar *name, double weight,
		gint priority, symbol_func_t func, gpointer user_data);

/**
 * Register function for dynamic symbols parsing
 * @param name name of symbol
 * @param func pointer to handler
 * @param user_data pointer to user_data
 */
void register_dynamic_symbol (memory_pool_t *pool, struct symbols_cache **cache, const gchar *name, 
						double weight, symbol_func_t func, 
						gpointer user_data, GList *networks);

/**
 * Call function for cached symbol using saved callback
 * @param task task object
 * @param cache symbols cache
 * @param saved_item pointer to currently saved item
 */
gboolean call_symbol_callback (struct worker_task *task, struct symbols_cache *cache, gpointer *save);

/**
 * Remove all dynamic rules from cache
 * @param cache symbols cache
 */
void remove_dynamic_rules (struct symbols_cache *cache);

/**
 * Validate cache items agains theirs weights defined in metrics
 * @param cache symbols cache
 * @param cfg configuration
 * @param strict do strict checks - symbols MUST be described in metrics
 */
gboolean validate_cache (struct symbols_cache *cache, struct config_file *cfg, gboolean strict);


#endif
