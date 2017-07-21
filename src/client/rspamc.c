/*
 * Copyright (c) 2011, Vsevolod Stakhov
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
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "util.h"
#include "../../lib/client/librspamdclient.h"

#define PRINT_FUNC printf

#define DEFAULT_PORT 11333
#define DEFAULT_CONTROL_PORT 11334

static gchar                   *connect_str = "localhost";
static gchar                   *password = NULL;
static gchar                   *ip = NULL;
static gchar                   *from = NULL;
static gchar                   *deliver_to = NULL;
static gchar                   *rcpt = NULL;
static gchar                   *user = NULL;
static gchar                   *helo = NULL;
static gchar                   *hostname = NULL;
static gchar                   *classifier = "bayes";
static gchar                   *local_addr = NULL;
static gint                     weight = 1;
static gint                     flag;
static gint                     timeout = 5;
static gboolean                 pass_all;
static gboolean                 tty = FALSE;
static gboolean                 verbose = FALSE;
static gboolean                 print_commands = FALSE;
static struct rspamd_client    *client = NULL;

static GOptionEntry entries[] =
{
		{ "connect", 'h', 0, G_OPTION_ARG_STRING, &connect_str, "Specify host and port", NULL },
		{ "password", 'P', 0, G_OPTION_ARG_STRING, &password, "Specify control password", NULL },
		{ "classifier", 'c', 0, G_OPTION_ARG_STRING, &classifier, "Classifier to learn spam or ham", NULL },
		{ "weight", 'w', 0, G_OPTION_ARG_INT, &weight, "Weight for fuzzy operations", NULL },
		{ "flag", 'f', 0, G_OPTION_ARG_INT, &flag, "Flag for fuzzy operations", NULL },
		{ "pass-all", 'p', 0, G_OPTION_ARG_NONE, &pass_all, "Pass all filters", NULL },
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "More verbose output", NULL },
		{ "ip", 'i', 0, G_OPTION_ARG_STRING, &ip, "Emulate that message was received from specified ip address", NULL },
		{ "user", 'u', 0, G_OPTION_ARG_STRING, &user, "Emulate that message was from specified user", NULL },
		{ "deliver", 'd', 0, G_OPTION_ARG_STRING, &deliver_to, "Emulate that message is delivered to specified user", NULL },
		{ "from", 'F', 0, G_OPTION_ARG_STRING, &from, "Emulate that message is from specified user", NULL },
		{ "rcpt", 'r', 0, G_OPTION_ARG_STRING, &rcpt, "Emulate that message is for specified user", NULL },
		{ "helo", 0, 0, G_OPTION_ARG_STRING, &helo, "Imitate SMTP HELO passing from MTA", NULL },
		{ "hostname", 0, 0, G_OPTION_ARG_STRING, &hostname, "Imitate hostname passing from MTA", NULL },
		{ "timeout", 't', 0, G_OPTION_ARG_INT, &timeout, "Timeout for waiting for a reply", NULL },
		{ "bind", 'b', 0, G_OPTION_ARG_STRING, &local_addr, "Bind to specified ip address", NULL },
		{ "commands", 0, 0, G_OPTION_ARG_NONE, &print_commands, "List available commands", NULL },
		{ NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

enum rspamc_command {
	RSPAMC_COMMAND_UNKNOWN = 0,
	RSPAMC_COMMAND_SYMBOLS,
	RSPAMC_COMMAND_LEARN_SPAM,
	RSPAMC_COMMAND_LEARN_HAM,
	RSPAMC_COMMAND_FUZZY_ADD,
	RSPAMC_COMMAND_FUZZY_DEL,
	RSPAMC_COMMAND_STAT,
	RSPAMC_COMMAND_STAT_RESET,
	RSPAMC_COMMAND_COUNTERS,
	RSPAMC_COMMAND_UPTIME,
	RSPAMC_COMMAND_ADD_SYMBOL,
	RSPAMC_COMMAND_ADD_ACTION
};

struct {
	enum rspamc_command cmd;
	const char *name;
	const char *description;
	gboolean is_controller;
	gboolean is_privileged;
} rspamc_command_help[] = {
	{
		.cmd = RSPAMC_COMMAND_SYMBOLS,
		.name = "symbols",
		.description = "scan message and show symbols (default command)",
		.is_controller = FALSE,
		.is_privileged = FALSE
	},
	{
		.cmd = RSPAMC_COMMAND_LEARN_SPAM,
		.name = "learn_spam",
		.description = "learn message as spam",
		.is_controller = TRUE,
		.is_privileged = TRUE
	},
	{
		.cmd = RSPAMC_COMMAND_LEARN_HAM,
		.name = "learn_ham",
		.description = "learn message as ham",
		.is_controller = TRUE,
		.is_privileged = TRUE
	},
	{
		.cmd = RSPAMC_COMMAND_FUZZY_ADD,
		.name = "fuzzy_add",
		.description = "add message to fuzzy storage (check -f and -w options for this command)",
		.is_controller = TRUE,
		.is_privileged = TRUE
	},
	{
		.cmd = RSPAMC_COMMAND_FUZZY_DEL,
		.name = "fuzzy_del",
		.description = "delete message from fuzzy storage (check -f option for this command)",
		.is_controller = TRUE,
		.is_privileged = TRUE
	},
	{
		.cmd = RSPAMC_COMMAND_STAT,
		.name = "stat",
		.description = "show rspamd statistics",
		.is_controller = TRUE,
		.is_privileged = FALSE
	},
	{
		.cmd = RSPAMC_COMMAND_STAT_RESET,
		.name = "stat_reset",
		.description = "show and reset rspamd statistics (useful for graphs)",
		.is_controller = TRUE,
		.is_privileged = TRUE
	},
	{
		.cmd = RSPAMC_COMMAND_COUNTERS,
		.name = "counters",
		.description = "display rspamd symbols statistics",
		.is_controller = TRUE,
		.is_privileged = FALSE
	},
	{
		.cmd = RSPAMC_COMMAND_UPTIME,
		.name = "uptime",
		.description = "show rspamd uptime",
		.is_controller = TRUE,
		.is_privileged = FALSE
	},
	{
		.cmd = RSPAMC_COMMAND_ADD_SYMBOL,
		.name = "add_symbol",
		.description = "add or modify symbol settings in rspamd",
		.is_controller = TRUE,
		.is_privileged = TRUE
	},
	{
		.cmd = RSPAMC_COMMAND_ADD_ACTION,
		.name = "add_action",
		.description = "add or modify action settings",
		.is_controller = TRUE,
		.is_privileged = TRUE
	}
};

/*
 * Parse command line
 */
static void
read_cmd_line (gint *argc, gchar ***argv)
{
	GError                         *error = NULL;
	GOptionContext                 *context;

	/* Prepare parser */
	context = g_option_context_new ("- run rspamc client");
	g_option_context_set_summary (context, "Summary:\n  Rspamd client version " RVERSION "\n  Release id: " RID);
	g_option_context_add_main_entries (context, entries, NULL);

	/* Parse options */
	if (!g_option_context_parse (context, argc, argv, &error)) {
		fprintf (stderr, "option parsing failed: %s\n", error->message);
		exit (EXIT_FAILURE);
	}

	/* Argc and argv are shifted after this function */
}

/*
 * Check rspamc command from string (used for arguments parsing)
 */
static enum rspamc_command
check_rspamc_command (const gchar *cmd)
{
	if (g_ascii_strcasecmp (cmd, "SYMBOLS") == 0 ||
		g_ascii_strcasecmp (cmd, "CHECK") == 0 ||
		g_ascii_strcasecmp (cmd, "REPORT") == 0) {
		/* These all are symbols, don't use other commands */
		return RSPAMC_COMMAND_SYMBOLS;
	}
	else if (g_ascii_strcasecmp (cmd, "LEARN_SPAM") == 0) {
		return RSPAMC_COMMAND_LEARN_SPAM;
	}
	else if (g_ascii_strcasecmp (cmd, "LEARN_HAM") == 0) {
		return RSPAMC_COMMAND_LEARN_HAM;
	}
	else if (g_ascii_strcasecmp (cmd, "FUZZY_ADD") == 0) {
		return RSPAMC_COMMAND_FUZZY_ADD;
	}
	else if (g_ascii_strcasecmp (cmd, "FUZZY_DEL") == 0) {
		return RSPAMC_COMMAND_FUZZY_DEL;
	}
	else if (g_ascii_strcasecmp (cmd, "STAT") == 0) {
		return RSPAMC_COMMAND_STAT;
	}
	else if (g_ascii_strcasecmp (cmd, "STAT_RESET") == 0) {
		return RSPAMC_COMMAND_STAT_RESET;
	}
	else if (g_ascii_strcasecmp (cmd, "COUNTERS") == 0) {
		return RSPAMC_COMMAND_COUNTERS;
	}
	else if (g_ascii_strcasecmp (cmd, "UPTIME") == 0) {
		return RSPAMC_COMMAND_UPTIME;
	}
	else if (g_ascii_strcasecmp (cmd, "ADD_SYMBOL") == 0) {
		return RSPAMC_COMMAND_ADD_SYMBOL;
	}
	else if (g_ascii_strcasecmp (cmd, "ADD_ACTION") == 0) {
		return RSPAMC_COMMAND_ADD_ACTION;
	}

	return RSPAMC_COMMAND_UNKNOWN;
}

static void
print_commands_list (void)
{
	guint                            i;

	PRINT_FUNC ("Rspamc commands summary:\n");
	for (i = 0; i < G_N_ELEMENTS (rspamc_command_help); i ++) {
		if (tty) {
			PRINT_FUNC ("  \033[1m%10s\033[0m (%7s%1s)\t%s\n", rspamc_command_help[i].name,
					rspamc_command_help[i].is_controller ? "control" : "normal",
					rspamc_command_help[i].is_privileged ? "*" : "",
					rspamc_command_help[i].description);
		}
		else {
			PRINT_FUNC ("  %10s (%7s%1s)\t%s\n", rspamc_command_help[i].name,
					rspamc_command_help[i].is_controller ? "control" : "normal",
					rspamc_command_help[i].is_privileged ? "*" : "",
					rspamc_command_help[i].description);
		}
	}
	PRINT_FUNC ("\n* is for privileged commands that may need password (see -P option)\n");
	PRINT_FUNC ("control commands use port 11334 while normal use 11333 by default (see -h option)\n");
}

/*
 * Parse connect_str and add server to librspamdclient
 */
static void
add_rspamd_server (gboolean is_control)
{
	gchar                         **vec, *err_str;
	guint16                         port;
	GError                         *err = NULL;

	if (connect_str == NULL) {
		fprintf (stderr, "cannot connect to rspamd server - empty string\n");
		exit (EXIT_FAILURE);
	}
	if (*connect_str != '/') {
		vec = g_strsplit_set (connect_str, ":", 2);
		if (vec == NULL || *vec == NULL) {
			fprintf (stderr, "cannot connect to rspamd server: %s\n", connect_str);
			exit (EXIT_FAILURE);
		}

		if (vec[1] == NULL) {
			port = is_control ? DEFAULT_CONTROL_PORT : DEFAULT_PORT;
		}
		else {
			port = strtoul (vec[1], &err_str, 10);
			if (*err_str != '\0') {
				fprintf (stderr, "cannot connect to rspamd server: %s, at pos %s\n", connect_str, err_str);
				exit (EXIT_FAILURE);
			}
		}
		if (! rspamd_add_server (client, vec[0], port, port, &err)) {
			fprintf (stderr, "cannot connect to rspamd server: %s, error: %s\n", connect_str, err->message);
			exit (EXIT_FAILURE);
		}
	}
	else {
		/* Unix socket version */
		if (! rspamd_add_server (client, connect_str, 0, 0, &err)) {
			fprintf (stderr, "cannot connect to rspamd server: %s, error: %s\n", connect_str, err->message);
			exit (EXIT_FAILURE);
		}
	}

}

static void
show_symbol_result (gpointer key, gpointer value, gpointer ud)
{
	struct rspamd_symbol            *s = value;
	GList                           *cur;
	static gboolean                  first = TRUE;

	if (verbose) {
		if (tty) {
			PRINT_FUNC ("\n\033[1mSymbol\033[0m - %s(%.2f)", s->name, s->weight);
		}
		else {
			PRINT_FUNC ("\nSymbol - %s(%.2f)", s->name, s->weight);
		}
		if (s->options) {
			PRINT_FUNC (": ");
			cur = g_list_first (s->options);
			while (cur) {
				if (cur->next) {
					PRINT_FUNC ("%s,", (const gchar *)cur->data);
				}
				else {
					PRINT_FUNC ("%s", (const gchar *)cur->data);
				}
				cur = g_list_next (cur);
			}
		}
		if (s->description) {
			PRINT_FUNC (" - \"%s\"", s->description);
		}
	}
	else {
		if (! first) {
			PRINT_FUNC (", ");
		}
		else {
			first = FALSE;
		}
		PRINT_FUNC ("%s(%.2f)", s->name, s->weight);

		if (s->options) {
			PRINT_FUNC ("(");
			cur = g_list_first (s->options);
			while (cur) {
				if (cur->next) {
					PRINT_FUNC ("%s,", (const gchar *)cur->data);
				}
				else {
					PRINT_FUNC ("%s)", (const gchar *)cur->data);
				}
				cur = g_list_next (cur);
			}
		}
	}
}

static void
show_metric_result (gpointer key, gpointer value, gpointer ud)
{
	struct rspamd_metric            *metric = value;

	if (metric->is_skipped) {
		PRINT_FUNC ("\n%s: Skipped\n", (const gchar *)key);
	}
	else {
		if (tty) {
			PRINT_FUNC ("\n\033[1m%s:\033[0m %s [ %.2f / %.2f ]\n", (const gchar *)key,
						metric->score > metric->required_score ? "True" : "False",
						metric->score, metric->required_score);
		}
		else {
			PRINT_FUNC ("\n%s: %s [ %.2f / %.2f ]\n", (const gchar *)key,
						metric->score > metric->required_score ? "True" : "False",
						metric->score, metric->required_score);
		}
		if (tty) {
			if (metric->action) {
				PRINT_FUNC ("\033[1mAction:\033[0m %s\n", metric->action);
			}
			PRINT_FUNC ("\033[1mSymbols: \033[0m");
		}
		else {
			if (metric->action) {
				PRINT_FUNC ("Action: %s\n", metric->action);
			}
			PRINT_FUNC ("Symbols: ");
		}
		if (metric->symbols) {
			g_hash_table_foreach (metric->symbols, show_symbol_result, NULL);
		}
		PRINT_FUNC ("\n");
	}
}

static void
show_header_result (gpointer key, gpointer value, gpointer ud)
{
	if (tty) {
		PRINT_FUNC ("\033[1m%s:\033[0m %s\n", (const gchar *)key, (const gchar *)value);
	}
	else {
		PRINT_FUNC ("%s: %s\n", (const gchar *)key, (const gchar *)value);
	}
}

static void
print_rspamd_result (struct rspamd_result *res, const gchar *filename)
{
	g_assert (res != 0);

	if (tty) {
		printf ("\033[1m");
	}
	PRINT_FUNC ("Results for host: %s\n", connect_str);
	if (filename != NULL) {
		PRINT_FUNC ("Filename: %s\n", filename);
	}
	if (tty) {
		printf ("\033[0m");
	}
	g_hash_table_foreach (res->metrics, show_metric_result, NULL);
	/* Show other headers */
	PRINT_FUNC ("\n");
	g_hash_table_foreach (res->headers, show_header_result, NULL);
	PRINT_FUNC ("\n");
}

static void
add_options (GHashTable *opts)
{
	if (ip != NULL) {
		g_hash_table_insert (opts, "Ip", ip);
	}
	if (from != NULL) {
		g_hash_table_insert (opts, "From", from);
	}
	if (user != NULL) {
		g_hash_table_insert (opts, "User", user);
	}
	if (rcpt != NULL) {
		g_hash_table_insert (opts, "Rcpt", rcpt);
	}
	if (deliver_to != NULL) {
		g_hash_table_insert (opts, "Deliver-To", deliver_to);
	}
	if (helo != NULL) {
		g_hash_table_insert (opts, "Helo", helo);
	}
	if (hostname != NULL) {
		g_hash_table_insert (opts, "Hostname", hostname);
	}
	if (pass_all) {
		g_hash_table_insert (opts, "Pass", "all");
	}
}

/*
 * Scan STDIN
 */
static void
scan_rspamd_stdin (void)
{
	gchar                           *in_buf;

	gint                             r = 0, len;
	GError                          *err = NULL;
	struct rspamd_result            *res;
	GHashTable                      *opts;

	/* Init options hash */
	opts = g_hash_table_new (rspamd_str_hash, rspamd_str_equal);
	add_options (opts);
	/* Add server */
	add_rspamd_server (FALSE);

	/* Allocate input buffer */
	len = BUFSIZ;
	in_buf = g_malloc (len);

	/* Read stdin */
	while (!feof (stdin)) {
		r += fread (in_buf + r, 1, len - r, stdin);
		if (len - r < len / 2) {
			/* Grow buffer */
			len *= 2;
			in_buf = g_realloc (in_buf, len);
		}
	}
	res = rspamd_scan_memory (client, in_buf, r, opts, &err);
	g_hash_table_destroy (opts);
	if (err != NULL) {
		fprintf (stderr, "cannot scan message: %s\n", err->message);
		exit (EXIT_FAILURE);
	}
	print_rspamd_result (res, "stdin");
	rspamd_free_result (res);
}

static void
scan_rspamd_file (const gchar *file)
{
	GError                          *err = NULL;
	struct rspamd_result            *res;
	GHashTable                      *opts;

	/* Add server */
	add_rspamd_server (FALSE);
	/* Init options hash */
	opts = g_hash_table_new (rspamd_str_hash, rspamd_str_equal);
	add_options (opts);
	res = rspamd_scan_file (client, file, opts, &err);
	g_hash_table_destroy (opts);
	if (err != NULL) {
		fprintf (stderr, "cannot scan message: %s\n", err->message);
		return;
	}
	print_rspamd_result (res, file);
	if (res) {
		rspamd_free_result (res);
	}
}

static void
learn_rspamd_stdin (gboolean is_spam)
{
	gchar                           *in_buf;
	gint                             r = 0, len;
	GError                          *err = NULL;
	GHashTable						*params;
	GList							*results, *cur;
	struct rspamd_controller_result	*res;

	if (classifier == NULL) {
		fprintf (stderr, "cannot learn message without password and symbol/classifier name\n");
		exit (EXIT_FAILURE);
	}
	/* Add server */
	add_rspamd_server (TRUE);

	/* Allocate input buffer */
	len = BUFSIZ;
	in_buf = g_malloc (len);

	/* Read stdin */
	while (!feof (stdin)) {
		r += fread (in_buf + r, 1, len - r, stdin);
		if (len - r < len / 2) {
			/* Grow buffer */
			len *= 2;
			in_buf = g_realloc (in_buf, len);
		}
	}

	params = g_hash_table_new (rspamd_str_hash, rspamd_str_equal);
	g_hash_table_insert (params, "Classifier", classifier);

	results = rspamd_controller_command_memory (client, is_spam ? "learn_spam" : "learn_ham", password, params, in_buf, r, &err);
	g_hash_table_destroy (params);
	if (results == NULL || err != NULL) {
		if (err != NULL) {
			fprintf (stderr, "cannot learn message: %s\n", err->message);
		}
		else {
			fprintf (stderr, "cannot learn message\n");
		}
		exit (EXIT_FAILURE);
	}
	else {
		cur = results;
		while (cur) {
			res = cur->data;
			if (tty) {
				printf ("\033[1m");
			}
			PRINT_FUNC ("Results for host: %s: %d, %s\n", res->server_name, res->code, res->result->str);
			if (tty) {
				printf ("\033[0m");
			}
			rspamd_free_controller_result (res);
			cur = g_list_next (cur);
		}
		g_list_free (results);
	}
}

static void
learn_rspamd_file (gboolean is_spam, const gchar *file)
{
	GError                          *err = NULL;
	GHashTable						*params;
	GList							*results, *cur;
	struct rspamd_controller_result	*res;

	if (classifier == NULL) {
		fprintf (stderr, "cannot learn message without password and symbol/classifier name\n");
		exit (EXIT_FAILURE);
	}

	/* Add server */
	add_rspamd_server (TRUE);
	params = g_hash_table_new (rspamd_str_hash, rspamd_str_equal);
	g_hash_table_insert (params, "Classifier", classifier);

	results = rspamd_controller_command_file (client, is_spam ? "learn_spam" : "learn_ham", password, params, file, &err);
	g_hash_table_destroy (params);
	if (results == NULL || err != NULL) {
		if (err != NULL) {
			fprintf (stderr, "cannot learn message: %s\n", err->message);
		}
		else {
			fprintf (stderr, "cannot learn message\n");
		}
		exit (EXIT_FAILURE);
	}
	else {
		cur = results;
		while (cur) {
			res = cur->data;
			if (tty) {
				printf ("\033[1m");
			}
			PRINT_FUNC ("Results for host: %s: %d, %s, file: %s\n",
					res->server_name, res->code, res->result->str, file);
			if (tty) {
				printf ("\033[0m");
			}
			rspamd_free_controller_result (res);
			cur = g_list_next (cur);
		}
		g_list_free (results);
	}
}

static void
fuzzy_rspamd_stdin (gboolean delete)
{
	gchar                           *in_buf;
	gint                             r = 0, len;
	GError                          *err = NULL;
	GHashTable						*params;
	GList							*results, *cur;
	gchar							 valuebuf[sizeof("65535")], flagbuf[sizeof("65535")];
	struct rspamd_controller_result	*res;

	params = g_hash_table_new (rspamd_str_hash, rspamd_str_equal);
	rspamd_snprintf (valuebuf, sizeof (valuebuf), "%d", weight);
	rspamd_snprintf (flagbuf, sizeof (flagbuf), "%d", flag);
	g_hash_table_insert (params, "Value", valuebuf);
	g_hash_table_insert (params, "Flag", flagbuf);

	/* Add server */
	add_rspamd_server (TRUE);

	/* Allocate input buffer */
	len = BUFSIZ;
	in_buf = g_malloc (len);

	/* Read stdin */
	while (!feof (stdin)) {
		r += fread (in_buf + r, 1, len - r, stdin);
		if (len - r < len / 2) {
			/* Grow buffer */
			len *= 2;
			in_buf = g_realloc (in_buf, len);
		}
	}
	results = rspamd_controller_command_memory (client, delete ? "fuzzy_del" : "fuzzy_add", password, params, in_buf, r, &err);
	g_hash_table_destroy (params);
	if (results == NULL || err != NULL) {
		if (err != NULL) {
			fprintf (stderr, "cannot process fuzzy for message: %s\n", err->message);
		}
		else {
			fprintf (stderr, "cannot process fuzzy for message\n");
		}
		exit (EXIT_FAILURE);
	}
	else {
		cur = results;
		while (cur) {
			res = cur->data;
			if (tty) {
				printf ("\033[1m");
			}
			PRINT_FUNC ("Results for host: %s: %d, %s\n", res->server_name, res->code, res->result->str);
			if (tty) {
				printf ("\033[0m");
			}
			rspamd_free_controller_result (res);
			cur = g_list_next (cur);
		}
		g_list_free (results);
	}
}

static void
fuzzy_rspamd_file (const gchar *file, gboolean delete)
{
	GError                          *err = NULL;
	GHashTable						*params;
	GList							*results, *cur;
	gchar							 valuebuf[sizeof("65535")], flagbuf[sizeof("65535")];
	struct rspamd_controller_result	*res;

	/* Add server */
	add_rspamd_server (TRUE);

	params = g_hash_table_new (rspamd_str_hash, rspamd_str_equal);
	rspamd_snprintf (valuebuf, sizeof (valuebuf), "%d", weight);
	rspamd_snprintf (flagbuf, sizeof (flagbuf), "%d", flag);
	g_hash_table_insert (params, "Value", valuebuf);
	g_hash_table_insert (params, "Flag", flagbuf);

	results = rspamd_controller_command_file (client, delete ? "fuzzy_del" : "fuzzy_add", password, params, file, &err);
	g_hash_table_destroy (params);
	if (results == NULL || err != NULL) {
		if (err != NULL) {
			fprintf (stderr, "cannot process fuzzy for message: %s\n", err->message);
		}
		else {
			fprintf (stderr, "cannot process fuzzy for message\n");
		}
		exit (EXIT_FAILURE);
	}
	else {
		cur = results;
		while (cur) {
			res = cur->data;
			if (tty) {
				printf ("\033[1m");
			}
			PRINT_FUNC ("Results for host: %s: %d, %s, file: %s\n",
					res->server_name, res->code, res->result->str, file);
			if (tty) {
				printf ("\033[0m");
			}
			rspamd_free_controller_result (res);
			cur = g_list_next (cur);
		}
		g_list_free (results);
	}
}

static void
rspamc_do_controller_simple_command (gchar *command, GHashTable *kwattrs)
{
	GError                          *err = NULL;
	GList							*results, *cur;
	struct rspamd_controller_result	*res;
	/* Add server */
	add_rspamd_server (TRUE);

	results = rspamd_controller_command_simple (client, command, password, kwattrs, &err);
	if (results == NULL || err != NULL) {
		if (err != NULL) {
			fprintf (stderr, "cannot perform command: %s\n", err->message);
		}
		else {
			fprintf (stderr, "cannot perform command:\n");
		}
		exit (EXIT_FAILURE);
	}
	else {
		cur = results;
		while (cur) {
			res = cur->data;
			if (tty) {
				printf ("\033[1m");
			}
			PRINT_FUNC ("Results for host: %s: %d, %s\n", res->server_name, res->code, res->result->str);
			if (tty) {
				printf ("\033[0m");
			}
			if (res->data) {
				PRINT_FUNC ("%*s\n", (gint)res->data->len, res->data->str);
			}
			else {
				PRINT_FUNC ("No results\n");
			}
			rspamd_free_controller_result (res);
			cur = g_list_next (cur);
		}
		g_list_free (results);
	}
}

struct rspamd_client_counter {
	gchar name[128];
	gint frequency;
	gdouble weight;
	gdouble time;
};

static void
print_rspamd_counters (struct rspamd_client_counter *counters, gint count)
{
	gint                             i, max_len = 24, l;
	struct rspamd_client_counter   *cur;
	gchar                            fmt_buf[64], dash_buf[82];

	/* Find maximum width of symbol's name */
	for (i = 0; i < count; i ++) {
		cur = &counters[i];
		l = strlen (cur->name);
		if (l > max_len) {
			max_len = MIN (40, l);
		}
	}

	rspamd_snprintf (fmt_buf, sizeof (fmt_buf), "| %%3s | %%%ds | %%6s | %%9s | %%9s |\n", max_len);
	memset (dash_buf, '-', 40 + max_len);
	dash_buf[40 + max_len] = '\0';

	PRINT_FUNC ("Symbols cache\n");
	PRINT_FUNC (" %s \n", dash_buf);
	if (tty) {
		printf ("\033[1m");
	}
	PRINT_FUNC (fmt_buf, "Pri", "Symbol", "Weight", "Frequency", "Avg. time");
	if (tty) {
		printf ("\033[0m");
	}
	rspamd_snprintf (fmt_buf, sizeof (fmt_buf), "| %%3d | %%%ds | %%6.1f | %%9d | %%9.3f |\n", max_len);
	for (i = 0; i < count; i ++) {
		cur = &counters[i];
		PRINT_FUNC (" %s \n", dash_buf);
		PRINT_FUNC (fmt_buf, i, cur->name, cur->weight, cur->frequency, cur->time);
	}
	PRINT_FUNC (" %s \n", dash_buf);
}

static void
show_rspamd_counters (void)
{
	GError                          *err = NULL;
	GList							*results, *cur;
	struct rspamd_controller_result	*res;
	gchar                          **str_counters, **tmpv;
	gint                             counters_num, i, cnum = 0;
	struct rspamd_client_counter   *counters = NULL, *cur_counter;

	/* Add server */
	add_rspamd_server (TRUE);

	results = rspamd_controller_command_simple (client, "counters", password, NULL, &err);
	if (results == NULL || err != NULL) {
		if (err != NULL) {
			fprintf (stderr, "cannot perform command: %s\n", err->message);
		}
		else {
			fprintf (stderr, "cannot perform command:\n");
		}
		exit (EXIT_FAILURE);
	}
	else {
		cur = results;
		while (cur) {
			res = cur->data;
			if (tty) {
				printf ("\033[1m");
			}
			PRINT_FUNC ("Results for host: %s: %d, %s\n", res->server_name, res->code, res->result->str);
			if (tty) {
				printf ("\033[0m");
			}
			str_counters = g_strsplit_set (res->data->str, "\n", -1);
			if (str_counters != NULL) {
				counters_num = g_strv_length (str_counters);
				if (counters_num > 0) {
					counters = g_malloc0 (counters_num * sizeof (struct rspamd_client_counter));
					for (i = 0; i < counters_num; i ++) {
						cur_counter = &counters[cnum];
						tmpv = g_strsplit_set (str_counters[i], " \t", -1);
						if (g_strv_length (tmpv) == 4) {
							rspamd_strlcpy (cur_counter->name, tmpv[0], sizeof (cur_counter->name));
							cur_counter->weight = strtod (tmpv[1], NULL);
							cur_counter->frequency = strtoul (tmpv[2], NULL, 10);
							cur_counter->time = strtod (tmpv[3], NULL);
							cnum ++;
						}
						g_strfreev (tmpv);
					}
					print_rspamd_counters (counters, cnum);
				}
				g_strfreev (str_counters);
			}
			rspamd_free_controller_result (res);
			cur = g_list_next (cur);
		}
		g_list_free (results);
	}
}


gint
main (gint argc, gchar **argv, gchar **env)
{
	enum rspamc_command              cmd;
	gint                             i;
	struct in_addr					 ina;
	GHashTable						*kwattrs;


	kwattrs = g_hash_table_new (rspamd_str_hash, rspamd_str_equal);

	read_cmd_line (&argc, &argv);

	tty = isatty (STDOUT_FILENO);

	if (print_commands) {
		print_commands_list ();
		exit (EXIT_SUCCESS);
	}

	if (local_addr) {
		if (inet_aton (local_addr, &ina) != 0) {
			client = rspamd_client_init_binded (&ina);
		}
		else {
			fprintf (stderr, "%s is not a valid ip address\n", local_addr);
			exit (EXIT_FAILURE);
		}
	}
	else {
		client = rspamd_client_init ();
	}

	rspamd_set_timeout (client, 1000, timeout * 1000);
	/* Now read other args from argc and argv */
	if (argc == 1) {
		/* No args, just read stdin */
		scan_rspamd_stdin ();
	}
	else if (argc == 2) {
		/* One argument is whether command or filename */
		if ((cmd = check_rspamc_command (argv[1])) != RSPAMC_COMMAND_UNKNOWN) {
			/* In case of command read stdin */
			switch (cmd) {
			case RSPAMC_COMMAND_SYMBOLS:
				scan_rspamd_stdin ();
				break;
			case RSPAMC_COMMAND_LEARN_SPAM:
				if (classifier != NULL) {
					learn_rspamd_stdin (TRUE);
				}
				else {
					fprintf (stderr, "no classifier specified\n");
					exit (EXIT_FAILURE);
				}
				break;
			case RSPAMC_COMMAND_LEARN_HAM:
				if (classifier != NULL) {
					learn_rspamd_stdin (FALSE);
				}
				else {
					fprintf (stderr, "no classifier specified\n");
					exit (EXIT_FAILURE);
				}
				break;
			case RSPAMC_COMMAND_FUZZY_ADD:
				fuzzy_rspamd_stdin (FALSE);
				break;
			case RSPAMC_COMMAND_FUZZY_DEL:
				fuzzy_rspamd_stdin (TRUE);
				break;
			case RSPAMC_COMMAND_STAT:
				rspamc_do_controller_simple_command ("stat", NULL);
				break;
			case RSPAMC_COMMAND_STAT_RESET:
				rspamc_do_controller_simple_command ("stat_reset", NULL);
				break;
			case RSPAMC_COMMAND_COUNTERS:
				show_rspamd_counters ();
				break;
			case RSPAMC_COMMAND_UPTIME:
				rspamc_do_controller_simple_command ("uptime", NULL);
				break;
			default:
				fprintf (stderr, "invalid arguments\n");
				exit (EXIT_FAILURE);
			}
		}
		else {
			scan_rspamd_file (argv[1]);
		}
	}
	else {
		if ((cmd = check_rspamc_command (argv[1])) != RSPAMC_COMMAND_UNKNOWN) {
			/* In case of command read arguments starting from 2 */
			if (cmd == RSPAMC_COMMAND_ADD_SYMBOL || cmd == RSPAMC_COMMAND_ADD_ACTION) {
				if (argc < 4 || argc > 5) {
					fprintf (stderr, "invalid arguments\n");
					exit (EXIT_FAILURE);
				}
				if (argc == 5) {
					g_hash_table_insert (kwattrs, "metric", argv[2]);
					g_hash_table_insert (kwattrs, "name", argv[3]);
					g_hash_table_insert (kwattrs, "value", argv[4]);
				}
				else {
					g_hash_table_insert (kwattrs, "name", argv[2]);
					g_hash_table_insert (kwattrs, "value", argv[3]);
				}
				rspamc_do_controller_simple_command (cmd == RSPAMC_COMMAND_ADD_SYMBOL ? "add_symbol" : "add_action", kwattrs);
			}
			else {
				for (i = 2; i < argc; i ++) {
					if (tty) {
						printf ("\033[1m");
					}
					PRINT_FUNC ("Results for file: %s\n\n", argv[i]);
					if (tty) {
						printf ("\033[0m");
					}
					switch (cmd) {
					case RSPAMC_COMMAND_SYMBOLS:
						scan_rspamd_file (argv[i]);
						break;
					case RSPAMC_COMMAND_LEARN_SPAM:
						if (classifier != NULL) {
							learn_rspamd_file (TRUE, argv[i]);
						}
						else {
							fprintf (stderr, "no classifier specified\n");
							exit (EXIT_FAILURE);
						}
						break;
					case RSPAMC_COMMAND_LEARN_HAM:
						if (classifier != NULL) {
							learn_rspamd_file (FALSE, argv[i]);
						}
						else {
							fprintf (stderr, "no classifier specified\n");
							exit (EXIT_FAILURE);
						}
						break;
					case RSPAMC_COMMAND_FUZZY_ADD:
						fuzzy_rspamd_file (argv[i], FALSE);
						break;
					case RSPAMC_COMMAND_FUZZY_DEL:
						fuzzy_rspamd_file (argv[i], TRUE);
						break;
					default:
						fprintf (stderr, "invalid arguments\n");
						exit (EXIT_FAILURE);
					}
				}
			}
		}
		else {
			for (i = 1; i < argc; i ++) {
				scan_rspamd_file (argv[i]);
			}
		}
	}

	rspamd_client_close (client);

	g_hash_table_destroy (kwattrs);

	return 0;
}
