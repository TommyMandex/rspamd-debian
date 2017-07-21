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

/***MODULE:fuzzy
 * rspamd module that checks fuzzy checksums for messages
 *
 * Allowed options:
 * - symbol (string): symbol to insert (default: 'R_FUZZY')
 * - max_score (double): maximum score to that weights of hashes would be normalized (default: 0 - no normalization)
 *
 * - fuzzy_map (string): a string that contains map in format { fuzzy_key => [ symbol, weight ] } where fuzzy_key is number of 
 *   fuzzy list. This string itself should be in format 1:R_FUZZY_SAMPLE1:10,2:R_FUZZY_SAMPLE2:1 etc, where first number is fuzzy
 *   key, second is symbol to insert and third - weight for normalization
 *
 * - min_length (integer): minimum length (in characters) for text part to be checked for fuzzy hash (default: 0 - no limit)
 * - whitelist (map string): map of ip addresses that should not be checked with this module
 * - servers (string): list of fuzzy servers in format "server1:port,server2:port" - these servers would be used for checking and storing
 *   fuzzy hashes
 */

#include "config.h"
#include "main.h"
#include "message.h"
#include "cfg_file.h"
#include "expressions.h"
#include "util.h"
#include "view.h"
#include "map.h"
#include "images.h"
#include "fuzzy_storage.h"
#include "cfg_xml.h"

#define DEFAULT_SYMBOL "R_FUZZY_HASH"
#define DEFAULT_UPSTREAM_ERROR_TIME 10
#define DEFAULT_UPSTREAM_DEAD_TIME 300
#define DEFAULT_UPSTREAM_MAXERRORS 10

#define DEFAULT_IO_TIMEOUT 500
#define DEFAULT_PORT 11335

struct storage_server {
	struct upstream up;
	gchar *name;
	gchar *addr;
	guint16 port;
};

struct fuzzy_mapping {
	guint64 fuzzy_flag;
	const gchar *symbol;
	double weight;
};

struct fuzzy_mime_type {
	gchar *type;
	gchar *subtype;
};

struct fuzzy_rule {
	struct storage_server *servers;
	gint servers_num;
	const gchar *symbol;
	GHashTable *mappings;
	GList *mime_types;
	double max_score;
	gboolean read_only;
	gboolean skip_unknown;
};

struct fuzzy_ctx {
	gint (*filter) (struct worker_task * task);
	memory_pool_t *fuzzy_pool;
	GList *fuzzy_rules;
	const gchar *default_symbol;
	guint32 min_hash_len;
	radix_tree_t *whitelist;
	guint32 min_bytes;
	guint32 min_height;
	guint32 min_width;
	guint32 io_timeout;
};

struct fuzzy_client_session {
	gint state;
	fuzzy_hash_t *h;
	struct event ev;
	struct timeval tv;
	struct worker_task *task;
	struct storage_server *server;
	struct fuzzy_rule *rule;
	gint fd;
};

struct fuzzy_learn_session {
	struct event ev;
	fuzzy_hash_t *h;
	gint cmd;
	gint value;
	gint flag;
	gint *saved;
	GError **err;
	struct timeval tv;
	struct controller_session *session;
	struct storage_server *server;
	struct fuzzy_rule *rule;
	struct worker_task *task;
	gint fd;
};

static struct fuzzy_ctx *fuzzy_module_ctx = NULL;
static const gchar hex_digits[] = "0123456789abcdef";

static void fuzzy_symbol_callback (struct worker_task *task, void *unused);
static gboolean fuzzy_add_handler (gchar **args, struct controller_session *session);
static gboolean fuzzy_delete_handler (gchar **args,
		struct controller_session *session);

/* Initialization */
gint fuzzy_check_module_init (struct config_file *cfg, struct module_ctx **ctx);
gint fuzzy_check_module_config (struct config_file *cfg);
gint fuzzy_check_module_reconfig (struct config_file *cfg);

module_t fuzzy_check_module = {
	"fuzzy_check",
	fuzzy_check_module_init,
	fuzzy_check_module_config,
	fuzzy_check_module_reconfig
};

static void
parse_flags (struct fuzzy_rule *rule, struct config_file *cfg, ucl_object_t *val)
{
	ucl_object_t *elt;
	struct fuzzy_mapping *map;
	const gchar *sym = NULL;

	if (val->type == UCL_STRING) {
		msg_err ("string mappings are deprecated and no longer supported, use new style configuration");
	}
	else if (val->type == UCL_OBJECT) {
		elt = ucl_object_find_key (val, "symbol");
		if (elt == NULL || !ucl_object_tostring_safe (elt, &sym)) {
			sym = ucl_object_key (val);
		}
		if (sym != NULL) {
			map =  memory_pool_alloc (fuzzy_module_ctx->fuzzy_pool, sizeof (struct fuzzy_mapping));
			map->symbol = sym;
			elt = ucl_object_find_key (val, "flag");
			if (elt != NULL && ucl_obj_toint_safe (elt, &map->fuzzy_flag)) {
				elt = ucl_object_find_key (val, "max_score");
				if (elt != NULL) {
					map->weight = ucl_obj_todouble (elt);
				}
				else {
					map->weight = rule->max_score;
				}
				/* Add flag to hash table */
				g_hash_table_insert (rule->mappings, GINT_TO_POINTER (map->fuzzy_flag), map);
				register_virtual_symbol (&cfg->cache, map->symbol, 1.0);
			}
			else {
				msg_err ("fuzzy_map parameter has no flag definition");
			}
		}
		else {
			msg_err ("fuzzy_map parameter has no symbol definition");
		}
	}
	else {
		msg_err ("fuzzy_map parameter is of an unsupported type");
	}
}

static GList *
parse_mime_types (const gchar *str)
{
	gchar                           **strvec, *p;
	gint                            num, i;
	struct fuzzy_mime_type         *type;
	GList                          *res = NULL;

	strvec = g_strsplit_set (str, ",", 0);
	num = g_strv_length (strvec);
	for (i = 0; i < num; i++) {
		g_strstrip (strvec[i]);
		if ((p = strchr (strvec[i], '/')) != NULL) {
			*p = 0;
			type = memory_pool_alloc (fuzzy_module_ctx->fuzzy_pool, sizeof (struct fuzzy_mime_type));
			type->type = memory_pool_strdup (fuzzy_module_ctx->fuzzy_pool, strvec[i]);
			type->subtype = memory_pool_strdup (fuzzy_module_ctx->fuzzy_pool, p + 1);
			res = g_list_prepend (res, type);
		}
		else {
			msg_info ("bad content type: %s", strvec[i]);
		}
	}

	if (res != NULL) {
		memory_pool_add_destructor (fuzzy_module_ctx->fuzzy_pool, (pool_destruct_func)g_list_free, res);
	}

	return res;
}

static gboolean
fuzzy_check_content_type (struct fuzzy_rule *rule, GMimeContentType *type)
{
	struct fuzzy_mime_type         *ft;
	GList                          *cur;

	cur = rule->mime_types;
	while (cur) {
		ft = cur->data;
		if (g_mime_content_type_is_type (type, ft->type, ft->subtype)) {
			return TRUE;
		}
		cur = g_list_next (cur);
	}

	return FALSE;
}

static void
parse_servers_string (struct fuzzy_rule *rule, const gchar *str)
{
	gchar                           **strvec;
	gint                            i, num;
	struct storage_server          *cur;

	strvec = g_strsplit_set (str, ",", 0);
	num = g_strv_length (strvec);

	rule->servers = memory_pool_alloc0 (fuzzy_module_ctx->fuzzy_pool, sizeof (struct storage_server) * num);

	for (i = 0; i < num; i++) {
		g_strstrip (strvec[i]);

		cur = &rule->servers[rule->servers_num];
		if (parse_host_port (fuzzy_module_ctx->fuzzy_pool, strvec[i], &cur->addr, &cur->port)) {
			if (cur->port == 0) {
				cur->port = DEFAULT_PORT;
			}
			cur->name = memory_pool_strdup (fuzzy_module_ctx->fuzzy_pool, strvec[i]);
			rule->servers_num++;
		}
	}

	g_strfreev (strvec);

}

static double
fuzzy_normalize (gint32 in, double weight)
{
	if (weight == 0) {
		return 0;
	}
#ifdef HAVE_TANH
	return tanh (G_E * (double)in / weight);
#else
	return (in < weight ? in / weight : weight);
#endif
}

static const gchar *
fuzzy_to_string (fuzzy_hash_t *h)
{
	static gchar strbuf [FUZZY_HASHLEN * 2 + 1];
	const int max_print = 5;
	gint                            i;
	guint8 byte;

	for (i = 0; i < max_print; i ++) {
		byte = h->hash_pipe[i];
		if (byte == '\0') {
			break;
		}
		strbuf[i * 2] = hex_digits[byte >> 4];
		strbuf[i * 2 + 1] = hex_digits[byte & 0xf];
	}
	if (i == max_print) {
		memcpy (&strbuf[i * 2], "...", 4);
	}
	else {
		strbuf[i * 2] = '\0';
	}

	return strbuf;
}

static struct fuzzy_rule *
fuzzy_rule_new (const char *default_symbol, memory_pool_t *pool)
{
	struct fuzzy_rule *rule;

	rule = memory_pool_alloc0 (pool, sizeof (struct fuzzy_rule));

	rule->mappings = g_hash_table_new (g_direct_hash, g_direct_equal);
	rule->symbol = default_symbol;
	memory_pool_add_destructor (pool, (pool_destruct_func)g_hash_table_unref, rule->mappings);
	rule->read_only = FALSE;

	return rule;
}

static gint
fuzzy_parse_rule (struct config_file *cfg, ucl_object_t *obj)
{
	ucl_object_t *value, *cur;
	struct fuzzy_rule *rule;
	ucl_object_iter_t it = NULL;

	if (obj->type != UCL_OBJECT) {
		msg_err ("invalid rule definition");
		return -1;
	}

	rule = fuzzy_rule_new (fuzzy_module_ctx->default_symbol, fuzzy_module_ctx->fuzzy_pool);

	if ((value = ucl_object_find_key (obj, "mime_types")) != NULL) {
		if (value->type == UCL_ARRAY) {
			value = value->value.av;
		}
		LL_FOREACH (value, cur) {
			rule->mime_types = g_list_concat (rule->mime_types,
					parse_mime_types (ucl_obj_tostring (cur)));
		}
	}

	if ((value = ucl_object_find_key (obj, "max_score")) != NULL) {
		rule->max_score = ucl_obj_todouble (value);
	}
	if ((value = ucl_object_find_key (obj,  "symbol")) != NULL) {
		rule->symbol = ucl_obj_tostring (value);
	}
	if ((value = ucl_object_find_key (obj, "read_only")) != NULL) {
		rule->read_only = ucl_obj_toboolean (value);
	}
	if ((value = ucl_object_find_key (obj, "skip_unknown")) != NULL) {
		rule->skip_unknown = ucl_obj_toboolean (value);
	}

	if ((value = ucl_object_find_key (obj, "servers")) != NULL) {
		if (value->type == UCL_ARRAY) {
			value = value->value.av;
		}
		LL_FOREACH (value, cur) {
			parse_servers_string (rule, ucl_obj_tostring (cur));
		}
	}
	if ((value = ucl_object_find_key (obj, "fuzzy_map")) != NULL) {
		while ((cur = ucl_iterate_object (value, &it, true)) != NULL) {
			parse_flags (rule, cfg, cur);
		}
	}

	if (rule->servers_num == 0) {
		msg_err ("no servers defined for fuzzy rule with symbol: %s", rule->symbol);
		return -1;
	}
	else {
		fuzzy_module_ctx->fuzzy_rules = g_list_prepend (fuzzy_module_ctx->fuzzy_rules, rule);
		if (rule->symbol != fuzzy_module_ctx->default_symbol) {
			register_virtual_symbol (&cfg->cache, rule->symbol, 1.0);
		}
	}

	return 0;
}

gint
fuzzy_check_module_init (struct config_file *cfg, struct module_ctx **ctx)
{
	fuzzy_module_ctx = g_malloc0 (sizeof (struct fuzzy_ctx));

	fuzzy_module_ctx->fuzzy_pool = memory_pool_new (memory_pool_get_size ());

	*ctx = (struct module_ctx *)fuzzy_module_ctx;

	return 0;
}

gint
fuzzy_check_module_config (struct config_file *cfg)
{
	ucl_object_t             *value, *cur;
	gint                      res = TRUE;

	if ((value = get_module_opt (cfg, "fuzzy_check", "symbol")) != NULL) {
		fuzzy_module_ctx->default_symbol = ucl_obj_tostring (value);
	}
	else {
		fuzzy_module_ctx->default_symbol = DEFAULT_SYMBOL;
	}

	if ((value = get_module_opt (cfg, "fuzzy_check", "min_length")) != NULL) {
		fuzzy_module_ctx->min_hash_len = ucl_obj_toint (value);
	}
	else {
		fuzzy_module_ctx->min_hash_len = 0;
	}
	if ((value = get_module_opt (cfg, "fuzzy_check", "min_bytes")) != NULL) {
		fuzzy_module_ctx->min_bytes = ucl_obj_toint (value);
	}
	else {
		fuzzy_module_ctx->min_bytes = 0;
	}
	if ((value = get_module_opt (cfg, "fuzzy_check", "min_height")) != NULL) {
		fuzzy_module_ctx->min_height = ucl_obj_toint (value);
	}
	else {
		fuzzy_module_ctx->min_height = 0;
	}
	if ((value = get_module_opt (cfg, "fuzzy_check", "min_width")) != NULL) {
		fuzzy_module_ctx->min_width = ucl_obj_toint (value);
	}
	else {
		fuzzy_module_ctx->min_width = 0;
	}
	if ((value = get_module_opt (cfg, "fuzzy_check", "timeout")) != NULL) {
		fuzzy_module_ctx->io_timeout = ucl_obj_todouble (value) * 1000;
	}
	else {
		fuzzy_module_ctx->io_timeout = DEFAULT_IO_TIMEOUT;
	}

	if ((value = get_module_opt (cfg, "fuzzy_check", "whitelist")) != NULL) {
		fuzzy_module_ctx->whitelist = radix_tree_create ();
		if (!add_map (cfg, ucl_obj_tostring (value),
				"Fuzzy whitelist", read_radix_list, fin_radix_list,
				(void **)&fuzzy_module_ctx->whitelist)) {
			msg_err ("cannot add whitelist '%s'", ucl_obj_tostring (value));
		}
	}
	else {
		fuzzy_module_ctx->whitelist = NULL;
	}

	if ((value = get_module_opt (cfg, "fuzzy_check", "rule")) != NULL) {
		LL_FOREACH (value, cur) {
			if (fuzzy_parse_rule (cfg, cur) == -1) {
				return -1;
			}
		}
	}

	if (fuzzy_module_ctx->fuzzy_rules != NULL) {
		register_callback_symbol (&cfg->cache, fuzzy_module_ctx->default_symbol,
				1.0, fuzzy_symbol_callback, NULL);

		register_custom_controller_command ("fuzzy_add", fuzzy_add_handler, TRUE, TRUE);
		register_custom_controller_command ("fuzzy_del", fuzzy_delete_handler, TRUE, TRUE);
	}
	else {
		msg_warn ("fuzzy module is enabled but no rules are defined");
	}

	return res;
}

gint
fuzzy_check_module_reconfig (struct config_file *cfg)
{
	memory_pool_delete (fuzzy_module_ctx->fuzzy_pool);

	fuzzy_module_ctx->fuzzy_pool = memory_pool_new (memory_pool_get_size ());
	
	return fuzzy_check_module_config (cfg);
}

/* Finalize IO */
static void
fuzzy_io_fin (void *ud)
{
	struct fuzzy_client_session    *session = ud;

	event_del (&session->ev);
	close (session->fd);
}

/* Call this whenever we got data from fuzzy storage */
static void
fuzzy_io_callback (gint fd, short what, void *arg)
{
	struct fuzzy_client_session    *session = arg;
	struct fuzzy_cmd                cmd;
	struct fuzzy_mapping           *map;
	gchar                           buf[62], *err_str;
	const gchar                    *symbol;
	gint                            value = 0, flag = 0, r;
	double                          nval;

	if (what == EV_WRITE) {
		/* Send command to storage */
		memset (&cmd, 0, sizeof (cmd));
		cmd.blocksize = session->h->block_size;
		cmd.value = 0;
		memcpy (cmd.hash, session->h->hash_pipe, sizeof (cmd.hash));
		cmd.cmd = FUZZY_CHECK;
		cmd.flag = 0;
		if (write (fd, &cmd, sizeof (struct fuzzy_cmd)) == -1) {
			goto err;
		}
		else {
			event_del (&session->ev);
			event_set (&session->ev, fd, EV_READ, fuzzy_io_callback, session);
			event_add (&session->ev, &session->tv);
		}
	}
	else if (what == EV_READ) {
		/* Got reply */
		if ((r = read (fd, buf, sizeof (buf) - 1)) == -1) {
			goto err;
		}
		else if (buf[0] == 'O' && buf[1] == 'K') {
			buf[r] = 0;
			/* Now try to get value */
			value = strtol (buf + 3, &err_str, 10);
			if (*err_str == ' ') {
				/* Now read flag */
				flag = strtol (err_str + 1, &err_str, 10);
			}
			*err_str = '\0';
			/* Get mapping by flag */
			if ((map = g_hash_table_lookup (session->rule->mappings, GINT_TO_POINTER (flag))) == NULL) {
				/* Default symbol and default weight */
				symbol = session->rule->symbol;
				nval = fuzzy_normalize (value, session->rule->max_score);
			}
			else {
				/* Get symbol and weight from map */
				symbol = map->symbol;
				nval = fuzzy_normalize (value, map->weight);
			}
			msg_info ("<%s>, found fuzzy hash '%s' with weight: %.2f, in list: %s:%d%s",
					session->task->message_id, fuzzy_to_string (session->h), nval, symbol,
					flag, map == NULL ? "(unknown)" : "");
			if (map != NULL || !session->rule->skip_unknown) {
				rspamd_snprintf (buf, sizeof (buf), "%d: %d / %.2f", flag, value, nval);
				insert_result_single (session->task, symbol, nval, g_list_prepend (NULL,
						memory_pool_strdup (session->task->task_pool, buf)));
			}
		}
		goto ok;
	}
	else {
		errno = ETIMEDOUT;
		goto err;	
	}

	return;

  err:
	msg_err ("got error on IO with server %s, %d, %s", session->server->name, errno, strerror (errno));
  ok:
	remove_normal_event (session->task->s, fuzzy_io_fin, session);
}

static void
fuzzy_learn_fin (void *arg)
{
	struct fuzzy_learn_session     *session = arg;

	event_del (&session->ev);
	close (session->fd);
}

static void
fuzzy_learn_callback (gint fd, short what, void *arg)
{
	struct fuzzy_learn_session     *session = arg;
	struct fuzzy_cmd                cmd;
	gchar                           buf[512];
	const gchar                     *cmd_name;
	gint                            r;

	cmd_name = (session->cmd == FUZZY_WRITE ? "add" : "delete");
	if (what == EV_WRITE) {
		/* Send command to storage */
		cmd.blocksize = session->h->block_size;
		memcpy (cmd.hash, session->h->hash_pipe, sizeof (cmd.hash));
		cmd.cmd = session->cmd;
		cmd.value = session->value;
		cmd.flag = session->flag;
		if (write (fd, &cmd, sizeof (struct fuzzy_cmd)) == -1) {
			if (*(session->err) == NULL) {
				g_set_error (session->err,
						g_quark_from_static_string ("fuzzy check"),
						errno, "write socket error: %s", strerror (errno));
			}
			goto err;
		}
		else {
			event_del (&session->ev);
			event_set (&session->ev, fd, EV_READ, fuzzy_learn_callback, session);
			event_add (&session->ev, &session->tv);
		}
	}
	else if (what == EV_READ) {
		if (read (fd, buf, sizeof (buf)) == -1) {
			msg_info ("cannot %s fuzzy hash for message <%s>, list %s:%d", cmd_name,
					session->task->message_id, session->rule->symbol, session->flag);
			if (*(session->err) == NULL) {
				g_set_error (session->err,
						g_quark_from_static_string ("fuzzy check"),
						errno, "read socket error: %s", strerror (errno));
			}
			goto err;
		}
		else if (buf[0] == 'O' && buf[1] == 'K') {
			msg_info ("%s fuzzy hash '%s', list: %s:%d for message <%s>", cmd_name,
					fuzzy_to_string (session->h), session->rule->symbol,
					session->flag, session->task->message_id);
			goto ok;
		}
		else {
			msg_info ("cannot %s fuzzy hash '%s' for message <%s>, list %s:%d", cmd_name,
					fuzzy_to_string (session->h), session->task->message_id,
					session->rule->symbol, session->flag);
			if (*(session->err) == NULL) {
				g_set_error (session->err,
						g_quark_from_static_string ("fuzzy check"), EINVAL, "%s fuzzy error", cmd_name);
			}
			goto ok;
		}
	}
	else {
		errno = ETIMEDOUT;
		if (*(session->err) == NULL) {
			g_set_error (session->err,
					g_quark_from_static_string ("fuzzy check"), EINVAL, "%s fuzzy, IO timeout", cmd_name);
		}
		goto err;	
	}

	return;

err:
	msg_err ("got error in IO with server %s, %d, %s",
			session->server->name, errno, strerror (errno));
ok:
	if (--(*(session->saved)) == 0) {
		session->session->state = STATE_REPLY;
		if (*(session->err) != NULL) {
			if (session->session->restful) {
				r = rspamd_snprintf (buf, sizeof (buf), "HTTP/1.0 %d Write hash error: %s" CRLF CRLF, (*session->err)->code, (*session->err)->message);
			}
			else {
				r = rspamd_snprintf (buf, sizeof (buf), "write error: %s" CRLF "END" CRLF, (*session->err)->message);
			}
			g_error_free (*session->err);
			if (r > 0 && ! rspamd_dispatcher_write (session->session->dispatcher, buf, r, FALSE, FALSE)) {
				return;
			}
		}
		else {
			if (session->session->restful) {
				r = rspamd_snprintf (buf, sizeof (buf), "HTTP/1.0 200 OK" CRLF CRLF);
			}
			else {
				r = rspamd_snprintf (buf, sizeof (buf), "OK" CRLF "END" CRLF);
			}
			if (! rspamd_dispatcher_write (session->session->dispatcher, buf, r, FALSE, FALSE)) {
				return;
			}
		}
		rspamd_dispatcher_restore (session->session->dispatcher);

	}
	remove_normal_event (session->session->s, fuzzy_learn_fin, session);
}

static inline void
register_fuzzy_call (struct worker_task *task, struct fuzzy_rule *rule, fuzzy_hash_t *h)
{
	struct fuzzy_client_session    *session;
	struct storage_server          *selected;
	gint                            sock;

	/* Get upstream */
#ifdef HAVE_CLOCK_GETTIME
	selected = (struct storage_server *)get_upstream_by_hash (rule->servers, rule->servers_num,
			sizeof (struct storage_server), task->ts.tv_sec,
			DEFAULT_UPSTREAM_ERROR_TIME, DEFAULT_UPSTREAM_DEAD_TIME, DEFAULT_UPSTREAM_MAXERRORS,
			h->hash_pipe, sizeof (h->hash_pipe));
#else
	selected = (struct storage_server *)get_upstream_by_hash (rule->servers, rule->servers_num,
			sizeof (struct storage_server), task->tv.tv_sec,
			DEFAULT_UPSTREAM_ERROR_TIME, DEFAULT_UPSTREAM_DEAD_TIME, DEFAULT_UPSTREAM_MAXERRORS, h->hash_pipe, sizeof (h->hash_pipe));
#endif
	if (selected) {
		if ((sock = make_universal_socket (selected->addr, selected->port, SOCK_DGRAM, TRUE, FALSE, FALSE)) == -1) {
			msg_warn ("cannot connect to %s, %d, %s", selected->name, errno, strerror (errno));
		}
		else {
			/* Create session for a socket */
			session = memory_pool_alloc (task->task_pool, sizeof (struct fuzzy_client_session));
			event_set (&session->ev, sock, EV_WRITE, fuzzy_io_callback, session);
			msec_to_tv (fuzzy_module_ctx->io_timeout, &session->tv);
			session->state = 0;
			session->h = h;
			session->task = task;
			session->fd = sock;
			session->server = selected;
			session->rule = rule;
			event_add (&session->ev, &session->tv);
			register_async_event (task->s, fuzzy_io_fin, session, g_quark_from_static_string ("fuzzy check"));
		}
	}
}

static void
fuzzy_check_rule (struct worker_task *task, struct fuzzy_rule *rule)
{
	struct mime_text_part          *part;
	struct mime_part               *mime_part;
	struct rspamd_image            *image;
	gchar                           *checksum;
	gsize                            hashlen;
	GList                          *cur;
	fuzzy_hash_t                   *fake_fuzzy;

	cur = task->text_parts;

	while (cur) {
		part = cur->data;
		if (part->is_empty) {
			cur = g_list_next (cur);
			continue;
		}

		/* Check length of part */
		if (fuzzy_module_ctx->min_bytes > part->content->len) {
			msg_info ("<%s>, part is shorter than %d symbols, skip fuzzy check",
					task->message_id, fuzzy_module_ctx->min_bytes);
			cur = g_list_next (cur);
			continue;
		}
		/* Check length of hash */
		hashlen = strlen (part->fuzzy->hash_pipe);
		if (hashlen == 0) {
			msg_info ("<%s>, part hash empty, skip fuzzy check",
					task->message_id, fuzzy_module_ctx->min_hash_len);
			cur = g_list_next (cur);
			continue;
		}
		if (fuzzy_module_ctx->min_hash_len != 0 &&
				hashlen * part->fuzzy->block_size < fuzzy_module_ctx->min_hash_len) {
			msg_info ("<%s>, part hash is shorter than %d symbols, skip fuzzy check",
					task->message_id, fuzzy_module_ctx->min_hash_len);
			cur = g_list_next (cur);
			continue;
		}

		register_fuzzy_call (task, rule, part->fuzzy);
		register_fuzzy_call (task, rule, part->double_fuzzy);

		cur = g_list_next (cur);
	}
	/* Process images */
	cur = task->images;
	while (cur) {
		image = cur->data;
		if (image->data->len > 0) {
			if (fuzzy_module_ctx->min_height <= 0 || image->height >= fuzzy_module_ctx->min_height) {
				if (fuzzy_module_ctx->min_width <= 0 || image->width >= fuzzy_module_ctx->min_width) {
					checksum = g_compute_checksum_for_data (G_CHECKSUM_MD5, image->data->data, image->data->len);
					/* Construct fake fuzzy hash */
					fake_fuzzy = memory_pool_alloc0 (task->task_pool, sizeof (fuzzy_hash_t));
					rspamd_strlcpy (fake_fuzzy->hash_pipe, checksum, sizeof (fake_fuzzy->hash_pipe));
					register_fuzzy_call (task, rule, fake_fuzzy);
					g_free (checksum);
				}
			}
		}
		cur = g_list_next (cur);
	}
	/* Process other parts */
	cur = task->parts;
	while (cur) {
		mime_part = cur->data;
		if (mime_part->content->len > 0 && fuzzy_check_content_type (rule, mime_part->type)) {
			if (fuzzy_module_ctx->min_bytes <= 0 || mime_part->content->len >= fuzzy_module_ctx->min_bytes) {
					checksum = g_compute_checksum_for_data (G_CHECKSUM_MD5,
							mime_part->content->data, mime_part->content->len);
					/* Construct fake fuzzy hash */
					fake_fuzzy = memory_pool_alloc0 (task->task_pool, sizeof (fuzzy_hash_t));
					rspamd_strlcpy (fake_fuzzy->hash_pipe, checksum, sizeof (fake_fuzzy->hash_pipe));
					register_fuzzy_call (task, rule, fake_fuzzy);
					g_free (checksum);
			}
		}
		cur = g_list_next (cur);
	}
}

/* This callback is called when we check message via fuzzy hashes storage */
static void
fuzzy_symbol_callback (struct worker_task *task, void *unused)
{
	struct fuzzy_rule *rule;
	GList *cur;

	/* Check whitelist */
#ifdef HAVE_INET_PTON
	if (fuzzy_module_ctx->whitelist && !task->from_addr.ipv6 && task->from_addr.d.in4.s_addr != INADDR_NONE) {
		if (radix32tree_find (fuzzy_module_ctx->whitelist, ntohl ((guint32) task->from_addr.d.in4.s_addr)) != RADIX_NO_VALUE) {
			msg_info ("<%s>, address %s is whitelisted, skip fuzzy check",
					task->message_id, inet_ntoa (task->from_addr.d.in4));
			return;
		}
	}
#else
	if (fuzzy_module_ctx->whitelist && task->from_addr.s_addr != 0) {
		if (radix32tree_find (fuzzy_module_ctx->whitelist, ntohl ((guint32) task->from_addr.s_addr)) != RADIX_NO_VALUE) {
			msg_info ("<%s>, address %s is whitelisted, skip fuzzy check",
					task->message_id, inet_ntoa (task->from_addr));
			return;
		}
	}
#endif

	cur = fuzzy_module_ctx->fuzzy_rules;
	while (cur) {
		rule = cur->data;
		fuzzy_check_rule (task, rule);
		cur = g_list_next (cur);
	}
}

static inline gboolean
register_fuzzy_controller_call (struct controller_session *session,
		struct fuzzy_rule *rule, struct worker_task *task, fuzzy_hash_t *h,
		gint cmd, gint value, gint flag, gint *saved, GError **err)
{
	struct fuzzy_learn_session     *s;
	struct storage_server          *selected;
	gint                            sock;

	/* Get upstream */
#ifdef HAVE_CLOCK_GETTIME
	selected = (struct storage_server *)get_upstream_by_hash (rule->servers, rule->servers_num,
			sizeof (struct storage_server), task->ts.tv_sec,
			DEFAULT_UPSTREAM_ERROR_TIME, DEFAULT_UPSTREAM_DEAD_TIME, DEFAULT_UPSTREAM_MAXERRORS,
			h->hash_pipe, sizeof (h->hash_pipe));
#else
	selected = (struct storage_server *)get_upstream_by_hash (rule->servers, rule->servers_num,
			sizeof (struct storage_server), task->tv.tv_sec,
			DEFAULT_UPSTREAM_ERROR_TIME, DEFAULT_UPSTREAM_DEAD_TIME, DEFAULT_UPSTREAM_MAXERRORS,
			h->hash_pipe, sizeof (h->hash_pipe));
#endif
	if (selected) {
		/* Create UDP socket */
		if ((sock = make_universal_socket (selected->addr, selected->port, SOCK_DGRAM, TRUE, FALSE, FALSE)) == -1) {
			return FALSE;
		}
		else {
			/* Socket is made, create session */
			s = memory_pool_alloc (session->session_pool, sizeof (struct fuzzy_learn_session));
			event_set (&s->ev, sock, EV_WRITE, fuzzy_learn_callback, s);
			msec_to_tv (fuzzy_module_ctx->io_timeout, &s->tv);
			s->task = task;
			s->h = memory_pool_alloc (session->session_pool, sizeof (fuzzy_hash_t));
			memcpy (s->h, h, sizeof (fuzzy_hash_t));
			s->session = session;
			s->server = selected;
			s->cmd = cmd;
			s->value = value;
			s->flag = flag;
			s->saved = saved;
			s->fd = sock;
			s->err = err;
			s->rule = rule;
			event_add (&s->ev, &s->tv);
			(*saved)++;
			register_async_event (session->s, fuzzy_learn_fin, s, g_quark_from_static_string ("fuzzy check"));
			return TRUE;
		}
	}

	return FALSE;
}

static int
fuzzy_process_rule (struct controller_session *session, struct fuzzy_rule *rule,
		struct worker_task *task, GError **err, gint cmd, gint flag, gint value, gint *saved)
{
	struct mime_text_part          *part;
	struct mime_part               *mime_part;
	struct rspamd_image            *image;
	GList                           *cur;
	gchar                           *checksum;
	fuzzy_hash_t                    fake_fuzzy;
	gint                            processed = 0;

	/* Plan new event for writing */
	cur = task->text_parts;

	while (cur) {
		part = cur->data;
		if (part->is_empty || part->fuzzy == NULL || part->fuzzy->hash_pipe[0] == '\0' ||
				(fuzzy_module_ctx->min_bytes > 0 && part->content->len < fuzzy_module_ctx->min_bytes)) {
			/* Skip empty parts */
			cur = g_list_next (cur);
			continue;
		}
		if (! register_fuzzy_controller_call (session, rule, task,
				part->fuzzy, cmd, value, flag, saved, err)) {
			goto err;
		}
		if (! register_fuzzy_controller_call (session, rule, task,
				part->double_fuzzy, cmd, value, flag, saved, err)) {
			/* Cannot write hash */
			goto err;
		}
		processed ++;
		cur = g_list_next (cur);
	}

	/* Process images */
	cur = task->images;
	while (cur) {
		image = cur->data;
		if (image->data->len > 0) {
			if (fuzzy_module_ctx->min_height <= 0 || image->height >= fuzzy_module_ctx->min_height) {
				if (fuzzy_module_ctx->min_width <= 0 || image->width >= fuzzy_module_ctx->min_width) {
					checksum = g_compute_checksum_for_data (G_CHECKSUM_MD5, image->data->data, image->data->len);
					/* Construct fake fuzzy hash */
					fake_fuzzy.block_size = 0;
					memset (fake_fuzzy.hash_pipe, 0, sizeof (fake_fuzzy.hash_pipe));
					rspamd_strlcpy (fake_fuzzy.hash_pipe, checksum, sizeof (fake_fuzzy.hash_pipe));
					if (! register_fuzzy_controller_call (session, rule, task,
							&fake_fuzzy, cmd, value, flag, saved, err)) {
						g_free (checksum);
						goto err;
					}

					msg_info ("save hash of image: [%s] to list: %d", checksum, flag);
					g_free (checksum);
					processed ++;
				}
			}
		}
		cur = g_list_next (cur);
	}
	/* Process other parts */
	cur = task->parts;
	while (cur) {
		mime_part = cur->data;
		if (mime_part->content->len > 0 && fuzzy_check_content_type (rule, mime_part->type)) {
			if (fuzzy_module_ctx->min_bytes <= 0 || mime_part->content->len >= fuzzy_module_ctx->min_bytes) {
				checksum = g_compute_checksum_for_data (G_CHECKSUM_MD5,
						mime_part->content->data, mime_part->content->len);
				/* Construct fake fuzzy hash */
				fake_fuzzy.block_size = 0;
				memset (fake_fuzzy.hash_pipe, 0, sizeof (fake_fuzzy.hash_pipe));
				rspamd_strlcpy (fake_fuzzy.hash_pipe, checksum, sizeof (fake_fuzzy.hash_pipe));
				if (! register_fuzzy_controller_call (session, rule, task,
						&fake_fuzzy, cmd, value, flag, saved, err)) {
					goto err;
				}
				msg_info ("save hash of part of type: %s/%s: [%s] to list %d",
						mime_part->type->type, mime_part->type->subtype,
						checksum, flag);
				g_free (checksum);
				processed ++;
			}
		}
		cur = g_list_next (cur);
	}

	return processed;

err:
	free_task (task, FALSE);
	return -1;
}

static gboolean
fuzzy_process_handler (struct controller_session *session, f_str_t * in)
{
	struct fuzzy_rule *rule;
	gboolean processed = FALSE, res = TRUE;
	GList *cur;
	struct worker_task *task;
	GError **err;
	gint r, cmd = 0, value = 0, flag = 0, *saved, *sargs;
	gchar out_buf[BUFSIZ];

	/* Extract arguments */
	if (session->other_data) {
		sargs = session->other_data;
		cmd = sargs[0];
		value = sargs[1];
		flag = sargs[2];
	}

	/* Prepare task */
	task = construct_task (session->worker);
	session->other_data = task;
	session->state = STATE_WAIT;

	/* Allocate message from string */
	task->msg = memory_pool_alloc (task->task_pool, sizeof (f_str_t));
	task->msg->begin = in->begin;
	task->msg->len = in->len;


	saved = memory_pool_alloc0 (session->session_pool, sizeof (gint));
	err = memory_pool_alloc0 (session->session_pool, sizeof (GError *));
	r = process_message (task);
	if (r == -1) {
		msg_warn ("processing of message failed");
		free_task (task, FALSE);
		session->state = STATE_REPLY;
		if (session->restful) {
			r = rspamd_snprintf (out_buf, sizeof (out_buf), "HTTP/1.0 500 Cannot process message" CRLF CRLF);
		}
		else {
			r = rspamd_snprintf (out_buf, sizeof (out_buf), "cannot process message" CRLF "END" CRLF);
		}
		if (! rspamd_dispatcher_write (session->dispatcher, out_buf, r, FALSE, FALSE)) {
			msg_warn ("write error");
		}
		rspamd_dispatcher_restore (session->dispatcher);
		return FALSE;
	}
	cur = fuzzy_module_ctx->fuzzy_rules;
	while (cur && res) {
		rule = cur->data;

		if (rule->read_only) {
			cur = g_list_next (cur);
			continue;
		}

		/* Check for flag */
		if (g_hash_table_lookup (rule->mappings, GINT_TO_POINTER (flag)) == NULL) {
			cur = g_list_next (cur);
			continue;
		}

		res = fuzzy_process_rule (session, rule, task, err, cmd, flag, value, saved);

		if (res) {
			processed = TRUE;
		}
		else if (res == -1) {
			break;
		}

		cur = g_list_next (cur);
	}

	memory_pool_add_destructor (session->session_pool, (pool_destruct_func)free_task_soft, task);

	if (res == -1) {
		session->state = STATE_REPLY;
		if (session->restful) {
			r = rspamd_snprintf (out_buf, sizeof (out_buf), "HTTP/1.0 500 Hash write error" CRLF CRLF);
		}
		else {
			r = rspamd_snprintf (out_buf, sizeof (out_buf), "cannot write hashes" CRLF "END" CRLF);
		}
		if (! rspamd_dispatcher_write (session->dispatcher, out_buf, r, FALSE, FALSE)) {
			return FALSE;
		}
		return FALSE;
	}
	else if (!processed) {
		session->state = STATE_REPLY;
		if (session->restful) {
			r = rspamd_snprintf (out_buf, sizeof (out_buf), "HTTP/1.0 404 No fuzzy rules matched" CRLF CRLF);
		}
		else {
			r = rspamd_snprintf (out_buf, sizeof (out_buf), "no fuzzy rules matched" CRLF "END" CRLF);
		}
		msg_info ("no rules matched fuzzy_add command");
		if (! rspamd_dispatcher_write (session->dispatcher, out_buf, r, FALSE, FALSE)) {
			return FALSE;
		}
		return FALSE;
	}

	return TRUE;
}

static gboolean
fuzzy_controller_handler (gchar **args, struct controller_session *session, gint cmd)
{
	gchar                           *arg, out_buf[BUFSIZ], *err_str;
	guint32                         size;
	gint                            r, value = 1, flag = 0, *sargs;

	if (session->restful) {
		/* Get size */
		arg = g_hash_table_lookup (session->kwargs, "content-length");
		if (!arg || *arg == '\0') {
			msg_info ("empty content length");
			r = rspamd_snprintf (out_buf, sizeof (out_buf), "HTTP/1.0 500 Fuzzy command requires Content-Length" CRLF CRLF);
			if (! rspamd_dispatcher_write (session->dispatcher, out_buf, r, FALSE, FALSE)) {
				return FALSE;
			}
			session->state = STATE_REPLY;
			rspamd_dispatcher_restore (session->dispatcher);
			return FALSE;
		}
		errno = 0;
		size = strtoul (arg, &err_str, 10);
		if (errno != 0 || (err_str && *err_str != '\0')) {
			r = rspamd_snprintf (out_buf, sizeof (out_buf), "HTTP/1.0 500 Learn size is invalid" CRLF CRLF);
			if (! rspamd_dispatcher_write (session->dispatcher, out_buf, r, FALSE, FALSE)) {
				return FALSE;
			}
			session->state = STATE_REPLY;
			rspamd_dispatcher_restore (session->dispatcher);
			return FALSE;
		}
		arg = g_hash_table_lookup (session->kwargs, "value");
		if (arg) {
			errno = 0;
			value = strtol (arg, &err_str, 10);
			if (errno != 0 || *err_str != '\0') {
				msg_info ("error converting numeric argument %s", arg);
				value = 0;
			}
		}
		arg = g_hash_table_lookup (session->kwargs, "flag");
		if (arg) {
			errno = 0;
			flag = strtol (arg, &err_str, 10);
			if (errno != 0 || *err_str != '\0') {
				msg_info ("error converting numeric argument %s", arg);
				flag = 0;
			}
		}
	}
	else {
		/* Process size */
		arg = args[0];
		if (!arg || *arg == '\0') {
			msg_info ("empty content length");
			r = rspamd_snprintf (out_buf, sizeof (out_buf), "fuzzy command requires length as argument" CRLF "END" CRLF);
			if (! rspamd_dispatcher_write (session->dispatcher, out_buf, r, FALSE, FALSE)) {
				return FALSE;
			}
			session->state = STATE_REPLY;
			return FALSE;
		}
		errno = 0;
		size = strtoul (arg, &err_str, 10);
		if (errno != 0 || (err_str && *err_str != '\0')) {
			r = rspamd_snprintf (out_buf, sizeof (out_buf), "learn size is invalid" CRLF);
			if (! rspamd_dispatcher_write (session->dispatcher, out_buf, r, FALSE, FALSE)) {
				return FALSE;
			}
			session->state = STATE_REPLY;
			return FALSE;
		}
		/* Process value */
		arg = args[1];
		if (arg && *arg != '\0') {
			errno = 0;
			value = strtol (arg, &err_str, 10);
			if (errno != 0 || *err_str != '\0') {
				msg_info ("error converting numeric argument %s", arg);
				value = 0;
			}
		}
		/* Process flag */
		arg = args[2];
		if (arg && *arg != '\0') {
			errno = 0;
			flag = strtol (arg, &err_str, 10);
			if (errno != 0 || *err_str != '\0') {
				msg_info ("error converting numeric argument %s", arg);
				flag = 0;
			}
		}
	}

	session->state = STATE_OTHER;
	rspamd_set_dispatcher_policy (session->dispatcher, BUFFER_CHARACTER, size);
	session->other_handler = fuzzy_process_handler;
	/* Prepare args */
	sargs = memory_pool_alloc (session->session_pool, sizeof (gint) * 3);
	sargs[0] = cmd;
	sargs[1] = value;
	sargs[2] = flag;
	session->other_data = sargs;

	return TRUE;
}

static gboolean
fuzzy_add_handler (gchar **args, struct controller_session *session)
{
	return fuzzy_controller_handler (args, session, FUZZY_WRITE);
}

static gboolean
fuzzy_delete_handler (gchar **args, struct controller_session *session)
{
	return fuzzy_controller_handler (args, session, FUZZY_DEL);
}
