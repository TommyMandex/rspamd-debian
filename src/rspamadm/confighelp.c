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
#include <ucl.h>
#include "config.h"
#include "rspamadm.h"
#include "cfg_file.h"
#include "cfg_rcl.h"
#include "rspamd.h"
#include "lua/lua_common.h"
#include "confighelp.lua.h"

static gboolean json = FALSE;
static gboolean compact = FALSE;
static gboolean keyword = FALSE;
static const gchar *plugins_path = RSPAMD_PLUGINSDIR;
extern struct rspamd_main *rspamd_main;
/* Defined in modules.c */
extern module_t *modules[];
extern worker_t *workers[];

static void rspamadm_confighelp (gint argc, gchar **argv);

static const char *rspamadm_confighelp_help (gboolean full_help);

struct rspamadm_command confighelp_command = {
		.name = "confighelp",
		.flags = 0,
		.help = rspamadm_confighelp_help,
		.run = rspamadm_confighelp
};

static GOptionEntry entries[] = {
		{"json",    'j', 0, G_OPTION_ARG_NONE, &json,
				"Output json",      NULL},
		{"compact", 'c', 0, G_OPTION_ARG_NONE, &compact,
				"Output compacted", NULL},
		{"keyword", 'k', 0, G_OPTION_ARG_NONE, &keyword,
				"Search by keyword", NULL},
		{"plugins", 'P', 0, G_OPTION_ARG_STRING, &plugins_path,
				"Use the following plugin path (" RSPAMD_PLUGINSDIR ")", NULL},
		{NULL,     0,   0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};

static const char *
rspamadm_confighelp_help (gboolean full_help)
{
	const char *help_str;

	if (full_help) {
		help_str = "Shows help for the specified configuration options\n\n"
				"Usage: rspamadm confighelp [option[, option...]]\n"
				"Where options are:\n\n"
				"-c: output compacted JSON\n"
				"-j: output pretty formatted JSON\n"
				"-k: search by keyword in doc string\n"
				"-P: use specific Lua plugins path\n"
				"--no-color: show colored output\n"
				"--short: show only option names\n"
				"--no-examples: do not show examples (impied by --short)\n"
				"--help: shows available options and commands";
	}
	else {
		help_str = "Shows help for configuration options";
	}

	return help_str;
}

static void
rspamadm_confighelp_show (struct rspamd_config *cfg, gint argc, gchar **argv,
		const char *key, const ucl_object_t *obj)
{
	rspamd_fstring_t *out;

	out = rspamd_fstring_new ();

	if (json) {
		rspamd_ucl_emit_fstring (obj, UCL_EMIT_JSON, &out);
	}
	else if (compact) {
		rspamd_ucl_emit_fstring (obj, UCL_EMIT_JSON_COMPACT, &out);
	}
	else {
		/* TODO: add lua helper for output */
		if (key) {
			rspamd_fprintf (stdout, "Showing help for %s%s:\n",
					keyword ? "keyword " : "", key);
		}
		else {
			rspamd_fprintf (stdout, "Showing help for all options:\n");
		}

		rspamadm_execute_lua_ucl_subr (cfg->lua_state,
				argc,
				argv,
				obj,
				rspamadm_script_confighelp);

		rspamd_fstring_free (out);
		return;
	}

	rspamd_fprintf (stdout, "%V", out);
	rspamd_fprintf (stdout, "\n");

	rspamd_fstring_free (out);
}

static void
rspamadm_confighelp_search_word_step (const ucl_object_t *obj,
		ucl_object_t *res,
		const gchar *str,
		gsize len,
		GString *path)
{
	ucl_object_iter_t it = NULL;
	const ucl_object_t *cur, *elt;
	const gchar *dot_pos;

	while ((cur = ucl_object_iterate (obj, &it, true)) != NULL) {
		if (cur->keylen > 0) {
			rspamd_printf_gstring (path, ".%*s", (int) cur->keylen, cur->key);

			if (rspamd_substring_search_caseless (cur->key,
					cur->keylen,
					str,
					len) != -1) {
				ucl_object_insert_key (res, ucl_object_ref (cur),
						path->str, path->len, true);
				goto fin;
			}
		}

		if (ucl_object_type (cur) == UCL_OBJECT) {
			elt = ucl_object_lookup (cur, "data");

			if (elt != NULL && ucl_object_type (elt) == UCL_STRING) {
				if (rspamd_substring_search_caseless (elt->value.sv,
						elt->len,
						str,
						len) != -1) {
					ucl_object_insert_key (res, ucl_object_ref (cur),
							path->str, path->len, true);
					goto fin;
				}
			}

			rspamadm_confighelp_search_word_step (cur, res, str, len, path);
		}

		fin:
		/* Remove the last component of the path */
		dot_pos = strrchr (path->str, '.');

		if (dot_pos) {
			g_string_erase (path, dot_pos - path->str,
					path->len - (dot_pos - path->str));
		}
	}
}

static ucl_object_t *
rspamadm_confighelp_search_word (const ucl_object_t *obj, const gchar *str)
{
	gsize len = strlen (str);
	GString *path = g_string_new ("");
	ucl_object_t *res;


	res = ucl_object_typed_new (UCL_OBJECT);

	rspamadm_confighelp_search_word_step (obj, res, str, len, path);

	return res;
}

static void
rspamadm_confighelp (gint argc, gchar **argv)
{
	struct rspamd_config *cfg;
	ucl_object_t *doc_obj;
	const ucl_object_t *elt;
	GOptionContext *context;
	GError *error = NULL;
	module_t *mod, **pmod;
	worker_t **pworker;
	struct module_ctx *mod_ctx;
	gint i = 1, ret = 0, processed_args = 0;

	context = g_option_context_new (
			"confighelp - displays help for the configuration options");
	g_option_context_set_summary (context,
			"Summary:\n  Rspamd administration utility version "
					RVERSION
					"\n  Release id: "
					RID);
	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_set_ignore_unknown_options (context, TRUE);

	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		rspamd_fprintf (stderr, "option parsing failed: %s\n", error->message);
		g_error_free (error);
		exit (1);
	}

	pworker = &workers[0];
	while (*pworker) {
		/* Init string quarks */
		(void) g_quark_from_static_string ((*pworker)->name);
		pworker++;
	}

	cfg = rspamd_config_new ();
	cfg->compiled_modules = modules;
	cfg->compiled_workers = workers;

	rspamd_rcl_config_init (cfg);
	lua_pushboolean (cfg->lua_state, true);
	lua_setglobal (cfg->lua_state, "confighelp");
	rspamd_rcl_add_lua_plugins_path (cfg, plugins_path, NULL);

	/* Init modules to get documentation strings */
	for (pmod = cfg->compiled_modules; pmod != NULL && *pmod != NULL; pmod++) {
		mod = *pmod;
		mod_ctx = g_slice_alloc0 (sizeof (struct module_ctx));

		if (mod->module_init_func (cfg, &mod_ctx) == 0) {
			g_hash_table_insert (cfg->c_modules,
					(gpointer) mod->name,
					mod_ctx);
			mod_ctx->mod = mod;
		}
	}
	/* Also init all workers */
	for (pworker = cfg->compiled_workers; *pworker != NULL; pworker ++) {
		(*pworker)->worker_init_func (cfg);
	}

	/* Init lua modules */
	rspamd_init_lua_filters (cfg, TRUE, ucl_vars);

	if (argc > 1) {
		for (i = 1; i < argc; i ++) {
			if (argv[i][0] != '-') {

				if (keyword) {
					doc_obj = rspamadm_confighelp_search_word (cfg->doc_strings,
							argv[i]);
				}
				else {
					doc_obj = ucl_object_typed_new (UCL_OBJECT);
					elt = ucl_object_lookup_path (cfg->doc_strings, argv[i]);

					if (elt) {
						ucl_object_insert_key (doc_obj, ucl_object_ref (elt),
								argv[i], 0, false);
					}
				}

				if (doc_obj != NULL) {
					rspamadm_confighelp_show (cfg, argc, argv, argv[i], doc_obj);
					ucl_object_unref (doc_obj);
				}
				else {
					rspamd_fprintf (stderr,
							"Cannot find help for %s\n",
							argv[i]);
					ret = EXIT_FAILURE;
				}
				processed_args ++;
			}
		}
	}

	if (processed_args == 0) {
		/* Show all documentation strings */
		rspamadm_confighelp_show (cfg, argc, argv, NULL, cfg->doc_strings);
	}

	rspamd_config_free (cfg);

	exit (ret);
}
