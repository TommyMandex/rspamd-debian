#ifndef RSPAMD_LOGGER_H
#define RSPAMD_LOGGER_H

#include "config.h"
#include "cfg_file.h"
#include "radix.h"
#include "util.h"

#ifndef G_LOG_LEVEL_USER_SHIFT
#define G_LOG_LEVEL_USER_SHIFT 8
#endif

enum rspamd_log_flags {
	RSPAMD_LOG_FORCED = (1 << G_LOG_LEVEL_USER_SHIFT),
	RSPAMD_LOG_ENCRYPTED = (1 << (G_LOG_LEVEL_USER_SHIFT + 1)),
	RSPAMD_LOG_LEVEL_MASK = ~(RSPAMD_LOG_FORCED|RSPAMD_LOG_ENCRYPTED)
};

typedef void (*rspamd_log_func_t) (const gchar *module, const gchar *id,
		const gchar *function,
		gint level_flags, const gchar *message, gpointer arg);

typedef struct rspamd_logger_s rspamd_logger_t;

#define RSPAMD_LOGBUF_SIZE 8192

/**
 * Init logger
 */
void rspamd_set_logger (struct rspamd_config *cfg,
		GQuark ptype,
		rspamd_logger_t **plogger,
		rspamd_mempool_t *pool);

/**
 * Open log file or initialize other structures
 */
gint rspamd_log_open (rspamd_logger_t *logger);

/**
 * Close log file or destroy other structures
 */
void rspamd_log_close (rspamd_logger_t *logger);

/**
 * Close and open log again
 */
gint rspamd_log_reopen (rspamd_logger_t *logger);

/**
 * Open log file or initialize other structures for privileged processes
 */
gint rspamd_log_open_priv (rspamd_logger_t *logger, uid_t uid, gid_t gid);

/**
 * Close log file or destroy other structures for privileged processes
 */
void rspamd_log_close_priv (rspamd_logger_t *logger, uid_t uid, gid_t gid);

/**
 * Close and open log again for privileged processes
 */
gint rspamd_log_reopen_priv (rspamd_logger_t *logger, uid_t uid, gid_t gid);

/**
 * Set log pid
 */
void rspamd_log_update_pid (GQuark ptype, rspamd_logger_t *logger);

/**
 * Flush log buffer for some types of logging
 */
void rspamd_log_flush (rspamd_logger_t *logger);

/**
 * Log function that is compatible for glib messages
 */
void rspamd_glib_log_function (const gchar *log_domain,
		GLogLevelFlags log_level, const gchar *message, gpointer arg);

/**
 * Log function for printing glib assertions
 */
void rspamd_glib_printerr_function (const gchar *message);

/**
 * Function with variable number of arguments support
 */
void rspamd_common_log_function (rspamd_logger_t *logger,
		gint level_flags,
		const gchar *module, const gchar *id,
		const gchar *function, const gchar *fmt, ...);

void rspamd_common_logv (rspamd_logger_t *logger, gint level_flags,
		const gchar *module, const gchar *id, const gchar *function,
		const gchar *fmt, va_list args);

/**
 * Add new logging module, returns module ID
 * @param mod
 * @return
 */
guint rspamd_logger_add_debug_module (const gchar *mod);

/*
 * Macro to use for faster debug modules
 */
#define INIT_LOG_MODULE(mname) \
	static guint rspamd_##mname##_log_id = (guint)-1; \
	RSPAMD_CONSTRUCTOR(rspamd_##mname##_log_init) { \
		rspamd_##mname##_log_id = rspamd_logger_add_debug_module(#mname); \
}

void rspamd_logger_configure_modules (GHashTable *mods_enabled);

/**
 * Conditional debug function
 */
void rspamd_conditional_debug (rspamd_logger_t *logger,
		rspamd_inet_addr_t *addr, const gchar *module, const gchar *id,
		const gchar *function, const gchar *fmt, ...);

void rspamd_conditional_debug_fast (rspamd_logger_t *logger,
		rspamd_inet_addr_t *addr,
		guint mod_id, const gchar *module, const gchar *id,
		const gchar *function, const gchar *fmt, ...);

/**
 * Function with variable number of arguments support that uses static default logger
 */
void rspamd_default_log_function (gint level_flags,
		const gchar *module, const gchar *id,
		const gchar *function,
		const gchar *fmt,
		...);

/**
 * Varargs version of default log function
 * @param log_level
 * @param function
 * @param fmt
 * @param args
 */
void rspamd_default_logv (gint level_flags,
		const gchar *module, const gchar *id,
		const gchar *function,
		const gchar *fmt,
		va_list args);

/**
 * Temporary turn on debug
 */
void rspamd_log_debug (rspamd_logger_t *logger);

/**
 * Turn off debug
 */
void rspamd_log_nodebug (rspamd_logger_t *logger);

/**
 * Turn off locking on logger (useful to avoid races)
 */
void rspamd_log_nolock (rspamd_logger_t *logger);

/**
 * Turn on locking to avoid log output mix
 */
void rspamd_log_lock (rspamd_logger_t *logger);

/**
 * Return array of counters (4 numbers):
 * 0 - errors
 * 1 - warnings
 * 2 - info messages
 * 3 - debug messages
 */
const guint64* rspamd_log_counters (rspamd_logger_t *logger);

/**
 * Returns errors ring buffer as ucl array
 * @param logger
 * @return
 */
ucl_object_t * rspamd_log_errorbuf_export (const rspamd_logger_t *logger);

/* Typical functions */

extern guint rspamd_task_log_id;

/* Logging in postfix style */
#define msg_err(...)    rspamd_default_log_function (G_LOG_LEVEL_CRITICAL, \
        NULL, NULL, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_warn(...)   rspamd_default_log_function (G_LOG_LEVEL_WARNING, \
        NULL, NULL, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_info(...)   rspamd_default_log_function (G_LOG_LEVEL_INFO, \
        NULL, NULL, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_notice(...)   rspamd_default_log_function (G_LOG_LEVEL_MESSAGE, \
        NULL, NULL, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_debug(...)  rspamd_default_log_function (G_LOG_LEVEL_DEBUG, \
        NULL, NULL, \
        G_STRFUNC, \
        __VA_ARGS__)

#define debug_task(...) rspamd_conditional_debug_fast (NULL, \
        task->from_addr, \
        rspamd_task_log_id, "task", task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)

/* Use the following macros if you have `task` in the function */
#define msg_err_task(...) rspamd_default_log_function (G_LOG_LEVEL_CRITICAL, \
        task->task_pool->tag.tagname, task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_warn_task(...)   rspamd_default_log_function (G_LOG_LEVEL_WARNING, \
        task->task_pool->tag.tagname, task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_notice_task(...)   rspamd_default_log_function (G_LOG_LEVEL_MESSAGE, \
        task->task_pool->tag.tagname, task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_info_task(...)   rspamd_default_log_function (G_LOG_LEVEL_INFO, \
        task->task_pool->tag.tagname, task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_debug_task(...)  rspamd_conditional_debug_fast (NULL,  task->from_addr, \
        rspamd_task_log_id, "task", task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_err_task_encrypted(...) rspamd_default_log_function (G_LOG_LEVEL_CRITICAL|RSPAMD_LOG_ENCRYPTED, \
        task->task_pool->tag.tagname, task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_warn_task_encrypted(...)   rspamd_default_log_function (G_LOG_LEVEL_WARNING|RSPAMD_LOG_ENCRYPTED, \
        task->task_pool->tag.tagname, task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_notice_task_encrypted(...) rspamd_default_log_function (G_LOG_LEVEL_MESSAGE|RSPAMD_LOG_ENCRYPTED, \
        task->task_pool->tag.tagname, task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_info_task_encrypted(...)   rspamd_default_log_function (G_LOG_LEVEL_INFO|RSPAMD_LOG_ENCRYPTED, \
        task->task_pool->tag.tagname, task->task_pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
/* Check for NULL pointer first */
#define msg_err_task_check(...) rspamd_default_log_function (G_LOG_LEVEL_CRITICAL, \
        task ? task->task_pool->tag.tagname : NULL, task ? task->task_pool->tag.uid : NULL, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_warn_task_check(...)   rspamd_default_log_function (G_LOG_LEVEL_WARNING, \
        task ? task->task_pool->tag.tagname : NULL, task ? task->task_pool->tag.uid : NULL, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_info_task_check(...)   rspamd_default_log_function (G_LOG_LEVEL_MESSAGE, \
        task ? task->task_pool->tag.tagname : NULL, task ? task->task_pool->tag.uid : NULL, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_notice_task_check(...)   rspamd_default_log_function (G_LOG_LEVEL_INFO, \
        task ? task->task_pool->tag.tagname : NULL, task ? task->task_pool->tag.uid : NULL, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_debug_task_check(...)  rspamd_conditional_debug_fast (NULL, \
		task ? task->from_addr : NULL, \
        rspamd_task_log_id, "task", task ? task->task_pool->tag.uid : NULL, \
        G_STRFUNC, \
        __VA_ARGS__)

/* Use the following macros if you have `pool` in the function */
#define msg_err_pool(...) rspamd_default_log_function (G_LOG_LEVEL_CRITICAL, \
        pool->tag.tagname, pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_warn_pool(...)   rspamd_default_log_function (G_LOG_LEVEL_WARNING, \
        pool->tag.tagname, pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_info_pool(...)   rspamd_default_log_function (G_LOG_LEVEL_INFO, \
        pool->tag.tagname, pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_debug_pool(...)  rspamd_conditional_debug (NULL, NULL, \
        pool->tag.tagname, pool->tag.uid, \
        G_STRFUNC, \
        __VA_ARGS__)
/* Check for NULL pointer first */
#define msg_err_pool_check(...) rspamd_default_log_function (G_LOG_LEVEL_CRITICAL, \
        pool ? pool->tag.tagname : NULL, pool ? pool->tag.uid : NULL, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_warn_pool_check(...)   rspamd_default_log_function (G_LOG_LEVEL_WARNING, \
		pool ? pool->tag.tagname : NULL, pool ? pool->tag.uid : NULL, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_info_pool_check(...)   rspamd_conditional_debug (NULL, NULL, \
		G_LOG_LEVEL_INFO, \
		pool ? pool->tag.tagname : NULL, pool ? pool->tag.uid : NULL, \
        G_STRFUNC, \
        __VA_ARGS__)
#define msg_debug_pool_check(...)  rspamd_conditional_debug (NULL, NULL, \
		pool ? pool->tag.tagname : NULL, pool ? pool->tag.uid : NULL, \
		G_STRFUNC, \
		__VA_ARGS__)
#endif
