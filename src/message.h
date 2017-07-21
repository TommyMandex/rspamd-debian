/**
 * @file message.h
 * Message processing functions and structures
 */

#ifndef RSPAMD_MESSAGE_H
#define RSPAMD_MESSAGE_H

#include "config.h"
#include "fuzzy.h"

struct worker_task;
struct controller_session;

struct mime_part {
	GMimeContentType *type;
	GByteArray *content;
	GMimeObject *parent;
	gchar *checksum;
	const gchar *filename;
};

struct mime_text_part {
	gboolean is_html;
	gboolean is_raw;
	gboolean is_balanced;
	gboolean is_empty;
	gboolean is_utf;
	const gchar *real_charset;
	GByteArray *orig;
	GByteArray *content;
	GNode *html_nodes;
	GList *urls_offset;										    /**< list of offsets of urls						*/
	fuzzy_hash_t *fuzzy;
	fuzzy_hash_t *double_fuzzy;
	GMimeObject *parent;
	GUnicodeScript script;
	f_str_t *diff_str;
};

struct received_header {
	gchar *from_hostname;
	gchar *from_ip;
	gchar *real_hostname;
	gchar *real_ip;
	gchar *by_hostname;
	gint is_error;
};

struct raw_header {
	gchar *name;
	gchar *value;
	gboolean tab_separated;
	gboolean empty_separator;
	gchar *separator;
	struct raw_header *next;
};

/**
 * Process message with all filters/statfiles, extract mime parts, urls and 
 * call metrics consolidation functions
 * @param task worker_task object
 * @return 0 if we have delayed filters to process and 1 if we have finished with processing
 */
gint process_message (struct worker_task *task);

/*
 * Set header with specified name and value
 */
void message_set_header (GMimeMessage *message, const gchar *field, const gchar *value);

/*
 * Get a list of header's values with specified header's name
 * @param pool if not NULL this pool would be used for storing header's values
 * @param message g_mime_message object
 * @param field header's name
 * @param strong if this flag is TRUE header's name is case sensitive, otherwise it is not
 * @return A list of header's values or NULL. If list is not NULL it MUST be freed. If pool is NULL elements must be freed as well.
 */
GList* message_get_header (memory_pool_t *pool, GMimeMessage *message, const gchar *field, gboolean strong);

/*
 * Get a list of header's values with specified header's name using raw headers
 * @param task worker task structure
 * @param field header's name
 * @param strong if this flag is TRUE header's name is case sensitive, otherwise it is not
 * @return A list of header's values or NULL. Unlike previous function it is NOT required to free list or values. I should rework one of these functions some time.
 */
GList* message_get_raw_header (struct worker_task *task, const gchar *field, gboolean strong);

#endif
