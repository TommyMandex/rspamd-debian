#ifndef TOKENIZERS_H
#define TOKENIZERS_H

#include "config.h"
#include "mem_pool.h"
#include "fstring.h"
#include "main.h"
#include "stat_api.h"

#define RSPAMD_DEFAULT_TOKENIZER "osb"

/* Common tokenizer structure */
struct rspamd_stat_tokenizer {
	gchar *name;
	gpointer (*get_config) (struct rspamd_tokenizer_config *cf, gsize *len);
	gboolean (*compatible_config) (struct rspamd_tokenizer_config *cf,
			gpointer ptr, gsize len);
	gint (*tokenize_func)(struct rspamd_tokenizer_config *cf,
			rspamd_mempool_t *pool,
			GArray *words,
			GTree *result,
			gboolean is_utf);
};

/* Compare two token nodes */
gint token_node_compare_func (gconstpointer a, gconstpointer b);


/* Tokenize text into array of words (rspamd_fstring_t type) */
GArray * rspamd_tokenize_text (gchar *text, gsize len, gboolean is_utf,
		gsize min_len, GList *exceptions, gboolean compat);

/* OSB tokenize function */
gint rspamd_tokenizer_osb (struct rspamd_tokenizer_config *cf,
	rspamd_mempool_t *pool,
	GArray *input,
	GTree *tokens,
	gboolean is_utf);

gpointer rspamd_tokenizer_osb_get_config (struct rspamd_tokenizer_config *cf,
		gsize *len);

gboolean
rspamd_tokenizer_osb_compatible_config (struct rspamd_tokenizer_config *cf,
			gpointer ptr, gsize len);

#endif
/*
 * vi:ts=4
 */
