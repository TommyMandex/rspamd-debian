/**
 * @file protocol.h
 * Rspamd protocol definition
 */

#ifndef RSPAMD_PROTOCOL_H
#define RSPAMD_PROTOCOL_H

#include "config.h"
#include "filter.h"

#define RSPAMD_FILTER_ERROR 1
#define RSPAMD_NETWORK_ERROR 2
#define RSPAMD_PROTOCOL_ERROR 3
#define RSPAMD_LENGTH_ERROR 4
#define RSPAMD_STATFILE_ERROR 5

#define RSPAMC_PROTO_1_0 "1.0"
#define RSPAMC_PROTO_1_1 "1.1"
#define RSPAMC_PROTO_1_2 "1.2"
#define RSPAMC_PROTO_1_3 "1.3"

/*
 * Reply messages
 */
#define RSPAMD_REPLY_BANNER "RSPAMD"
#define SPAMD_REPLY_BANNER "SPAMD"
#define SPAMD_OK "EX_OK"
/* XXX: try to convert rspamd errors to spamd errors */
#define SPAMD_ERROR "EX_ERROR"

struct worker_task;
struct metric;

enum rspamd_protocol {
	SPAMC_PROTO,
	RSPAMC_PROTO,
};

enum rspamd_command {
	CMD_CHECK,
	CMD_SYMBOLS,
	CMD_REPORT,
	CMD_REPORT_IFSPAM,
	CMD_SKIP,
	CMD_PING,
	CMD_PROCESS,
	CMD_LEARN,
	CMD_OTHER,
};


typedef gint (*protocol_reply_func)(struct worker_task *task);

struct custom_command {
	const gchar *name;
	protocol_reply_func func;
};

/**
 * Find a character in command in and return pointer to the first part of the string, in is modified to point to the second part of string
 * @param in f_str_t input
 * @param c separator character
 * @return pointer to the first part of string or NULL if there is no separator found
 */
gchar* separate_command (f_str_t * in, gchar c);

/**
 * Read one line of user's input for specified task
 * @param task task object
 * @param line line of user's input
 * @return 0 if line was successfully parsed and -1 if we have protocol error
 */
gboolean read_rspamd_input_line (struct worker_task *task, f_str_t *line);

/**
 * Write reply for specified task command
 * @param task task object
 * @return 0 if we wrote reply and -1 if there was some error
 */
gboolean write_reply (struct worker_task *task) G_GNUC_WARN_UNUSED_RESULT;


/**
 * Register custom fucntion to extend protocol
 * @param name symbolic name of custom function
 * @param func callback function for writing reply
 */
void register_protocol_command (const gchar *name, protocol_reply_func func);

#endif
