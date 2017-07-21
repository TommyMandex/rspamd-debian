/**
 * @file main.h
 * Definitions for main rspamd structures
 */

#ifndef RSPAMD_MAIN_H
#define RSPAMD_MAIN_H

#include "config.h"
#include "fstring.h"
#include "mem_pool.h"
#include "statfile.h"
#include "url.h"
#include "memcached.h"
#include "protocol.h"
#include "filter.h"
#include "buffer.h"
#include "hash.h"
#include "events.h"
#include "util.h"
#include "logger.h"
#include "roll_history.h"

/* Default values */
#define FIXED_CONFIG_FILE RSPAMD_CONFDIR "/rspamd.conf"
/* Time in seconds to exit for old worker */
#define SOFT_SHUTDOWN_TIME 10
/* Default metric name */
#define DEFAULT_METRIC "default"

/* Spam subject */
#define SPAM_SUBJECT "*** SPAM *** "

#ifdef CRLF
#undef CRLF
#undef CR
#undef LF
#endif

#define CRLF "\r\n"
#define CR '\r'
#define LF '\n'

/** 
 * Worker process structure 
 */
struct rspamd_worker {
	pid_t pid;													/**< pid of worker									*/
	gboolean is_initialized;									/**< is initialized									*/
	gboolean is_dying;											/**< if worker is going to shutdown					*/
	gboolean pending;											/**< if worker is pending to run					*/
	struct rspamd_main *srv;									/**< pointer to server structure					*/
	GQuark type;												/**< process type									*/
	struct event sig_ev_usr1;									/**< signals event									*/
	struct event sig_ev_usr2;									/**< signals event									*/
	GList *accept_events;										/**< socket events									*/
	struct worker_conf *cf;										/**< worker config data								*/
	gpointer ctx;												/**< worker's specific data							*/
};

/**
 * Module
 */

struct pidfh;
struct config_file;
struct tokenizer;
struct classifier;
struct classifier_config;
struct mime_part;
struct rspamd_view;
struct rspamd_dns_resolver;
struct worker_task;

/** 
 * Server statistics
 */
struct rspamd_stat {
	guint messages_scanned;								/**< total number of messages scanned				*/
	guint actions_stat[METRIC_ACTION_NOACTION + 1];		/**< statistic for each action						*/
	guint connections_count;							/**< total connections count						*/
	guint control_connections_count;					/**< connections count to control interface			*/
	guint messages_learned;								/**< messages learned								*/
	guint fuzzy_hashes;									/**< number of fuzzy hashes stored					*/
	guint fuzzy_hashes_expired;							/**< number of fuzzy hashes expired					*/
};

/**
 * Struct that determine main server object (for logging purposes)
 */
struct rspamd_main {
	struct config_file *cfg;									/**< pointer to config structure					*/
	pid_t pid;													/**< main pid										*/
	/* Pid file structure */
	rspamd_pidfh_t *pfh;										/**< struct pidfh for pidfile						*/
	GQuark type;												/**< process type									*/
	guint ev_initialized;										/**< is event system is initialized					*/
	struct rspamd_stat *stat;									/**< pointer to statistics							*/

	memory_pool_t *server_pool;									/**< server's memory pool							*/
	statfile_pool_t *statfile_pool;								/**< shared statfiles pool							*/
	GHashTable *workers;                                        /**< workers pool indexed by pid                    */
	rspamd_hash_t *counters;									/**< symbol cache counters							*/
	rspamd_logger_t *logger;
	uid_t workers_uid;											/**< worker's uid running to 						*/
	gid_t workers_gid;											/**< worker's gid running to						*/
	gboolean is_privilleged;									/**< true if run in privilleged mode 				*/
	struct roll_history *history;								/**< rolling history								*/
};

struct counter_data {
	guint64 value;
	gint number;
};

/**
 * Structure to point exception in text from processing
 */
struct process_exception {
	gsize pos;
	gsize len;
};

/**
 * Control session object
 */
struct controller_command;
struct controller_session;
typedef gboolean (*controller_func_t)(gchar **args, struct controller_session *session);

struct controller_session {
	struct rspamd_worker *worker;								/**< pointer to worker structure (controller in fact) */
	enum {
		STATE_COMMAND,
		STATE_HEADER,
		STATE_LEARN,
		STATE_LEARN_SPAM_PRE,
		STATE_LEARN_SPAM,
		STATE_REPLY,
		STATE_QUIT,
		STATE_OTHER,
		STATE_WAIT,
		STATE_WEIGHTS
	} state;													/**< current session state							*/
	gint sock;													/**< socket descriptor								*/
	/* Access to authorized commands */
	gboolean authorized;										/**< whether this session is authorized				*/
	gboolean restful;											/**< whether this session is a restful session		*/
	GHashTable *kwargs;											/**< keyword arguments for restful command			*/
	struct controller_command *cmd;								/**< real command									*/
	memory_pool_t *session_pool;								/**< memory pool for session 						*/
	struct config_file *cfg;									/**< pointer to config file							*/
	gchar *learn_rcpt;											/**< recipient for learning							*/
	gchar *learn_from;											/**< from address for learning						*/
	struct classifier_config *learn_classifier;
	gchar *learn_symbol;											/**< symbol to train								*/
	double learn_multiplier;									/**< multiplier for learning						*/
	rspamd_io_dispatcher_t *dispatcher;							/**< IO dispatcher object							*/
	f_str_t *learn_buf;											/**< learn input									*/
	GList *parts;												/**< extracted mime parts							*/
	gint in_class;												/**< positive or negative learn						*/
	gboolean (*other_handler)(struct controller_session *session,
			f_str_t *in);					/**< other command handler to execute at the end of processing */
	void *other_data;											/**< and its data 									*/
	controller_func_t custom_handler;							/**< custom command handler							*/
	struct rspamd_async_session* s;								/**< async session object							*/
	struct worker_task *learn_task;
	struct rspamd_dns_resolver *resolver;						/**< DNS resolver									*/
	struct event_base *ev_base;									/**< Event base										*/
};

/**
 * Worker task structure
 */
struct worker_task {
	struct rspamd_worker *worker;								/**< pointer to worker object						*/
	enum {
		READ_COMMAND,
		READ_HEADER,
		READ_MESSAGE,
		WRITE_REPLY,
		WRITE_ERROR,
		WAIT_PRE_FILTER,
		WAIT_FILTER,
		WAIT_POST_FILTER,
		CLOSING_CONNECTION,
		WRITING_REPLY
	} state;													/**< current session state							*/
	size_t content_length;										/**< length of user's input							*/
	enum rspamd_protocol proto;									/**< protocol (rspamc or spamc)						*/
	guint proto_ver;											/**< protocol version								*/
	enum rspamd_command cmd;									/**< command										*/
	struct custom_command *custom_cmd;							/**< custom command if any							*/	
	gint sock;													/**< socket descriptor								*/
	gboolean is_mime;                                           /**< if this task is mime task                      */
	gboolean is_json;											/**< output is JSON									*/
	gboolean is_http;											/**< output is HTTP									*/
	gboolean allow_learn;										/**< allow learning									*/
	gboolean is_skipped;                                        /**< whether message was skipped by configuration   */

	gchar *helo;													/**< helo header value								*/
	gchar *from;													/**< from header value								*/
	gchar *queue_id;												/**< queue id if specified							*/
	const gchar *message_id;										/**< message id										*/
	GList *rcpt;												/**< recipients list								*/
	guint nrcpt;											/**< number of recipients							*/
#ifdef HAVE_INET_PTON
	struct {
		union {
			struct in_addr in4;
			struct in6_addr in6;
		} d;
		gboolean ipv6;
		gboolean has_addr;
	} from_addr;
#else
	struct in_addr from_addr;									/**< client addr in numeric form					*/
#endif
	struct in_addr client_addr;									/**< client addr in numeric form					*/
	gchar *deliver_to;											/**< address to deliver								*/
	gchar *user;													/**< user to deliver								*/
	gchar *subject;												/**< subject (for non-mime)							*/
	gchar *hostname;											/**< hostname reported by MTA						*/
	gchar *statfile;											/**< statfile for learning							*/
	f_str_t *msg;												/**< message buffer									*/
	rspamd_io_dispatcher_t *dispatcher;							/**< IO dispatcher object							*/
	struct rspamd_async_session* s;								/**< async session object							*/
	gint parts_count;											/**< mime parts count								*/
	GMimeMessage *message;										/**< message, parsed with GMime						*/
	GMimeObject *parser_parent_part;							/**< current parent part							*/
	InternetAddressList *rcpts;									/**< list of all recipients 						*/
	GList *parts;												/**< list of parsed parts							*/
	GList *text_parts;											/**< list of text parts								*/
	gchar *raw_headers_str;											/**< list of raw headers							*/
	GList *received;											/**< list of received headers						*/
	GTree *urls;												/**< list of parsed urls							*/
	GTree *emails;												/**< list of parsed emails							*/
	GList *images;												/**< list of images									*/
	GHashTable *raw_headers;									/**< list of raw headers							*/
	GHashTable *results;										/**< hash table of metric_result indexed by 
	 *    metric's name									*/
	GHashTable *tokens;											/**< hash table of tokens indexed by tokenizer
	 *    pointer 										*/
	GList *messages;											/**< list of messages that would be reported		*/
	GHashTable *re_cache;										/**< cache for matched or not matched regexps		*/
	struct config_file *cfg;									/**< pointer to config object						*/
	gchar *last_error;											/**< last error										*/
	gint error_code;												/**< code of last error								*/
	memory_pool_t *task_pool;									/**< memory pool for task							*/
#ifdef HAVE_CLOCK_GETTIME
	struct timespec ts;											/**< time of connection								*/
#endif
	struct timeval tv;											/**< time of connection								*/
	struct rspamd_view *view;									/**< matching view									*/
	guint32 scan_milliseconds;									/**< how much milliseconds passed					*/
	gboolean view_checked;
	gboolean pass_all_filters;									/**< pass task throught every rule					*/
	guint32 parser_recursion;									/**< for avoiding recursion stack overflow			*/
	gboolean (*fin_callback)(void *arg);						/**< calback for filters finalizing					*/
	void *fin_arg;												/**< argument for fin callback						*/

	guint32 dns_requests;										/**< number of DNS requests per this task			*/

	struct rspamd_dns_resolver *resolver;						/**< DNS resolver									*/
	struct event_base *ev_base;									/**< Event base										*/

	struct {
		enum rspamd_metric_action action;						/**< Action of pre filters							*/
		gchar *str;												/**< String describing action						*/
	} pre_result;												/**< Result of pre-filters							*/
};

/**
 * Common structure representing C module context
 */
struct module_ctx {
	gint (*filter)(struct worker_task *task);					/**< pointer to headers process function			*/
};

/**
 * Common structure for C module
 */
struct c_module {
	const gchar *name;											/**< name											*/
	struct module_ctx *ctx;										/**< pointer to context								*/
};

/**
 * Register custom controller function
 */
void register_custom_controller_command (const gchar *name, controller_func_t handler, gboolean privilleged, gboolean require_message);

/**
 * If set, reopen log file on next write
 */
extern struct rspamd_main *rspamd_main;

/* Worker task manipulations */

/**
 * Construct new task for worker
 */
struct worker_task* construct_task (struct rspamd_worker *worker);
/**
 * Destroy task object and remove its IO dispatcher if it exists
 */
void free_task (struct worker_task *task, gboolean is_soft);
void free_task_hard (gpointer ud);
void free_task_soft (gpointer ud);

/**
 * Set counter for a symbol
 */
double set_counter (const gchar *name, guint32 value);

#ifndef HAVE_SA_SIGINFO
typedef void (*rspamd_sig_handler_t) (gint);
#else
typedef void (*rspamd_sig_handler_t) (gint, siginfo_t *, void *);
#endif

/**
 * Prepare worker's startup
 * @param worker worker structure
 * @param name name of the worker
 * @param sig_handler handler of main signals
 * @param accept_handler handler of accept event for listen sockets
 * @return event base suitable for a worker
 */
struct event_base *
prepare_worker (struct rspamd_worker *worker, const char *name,
		rspamd_sig_handler_t sig_handler,
		void (*accept_handler)(int, short, void *));

/**
 * Stop accepting new connections for a worker
 * @param worker
 */
void worker_stop_accept (struct rspamd_worker *worker);

#endif

/* 
 * vi:ts=4 
 */
