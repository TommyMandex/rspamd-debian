/*
 * Functions for handling with fixed size strings
 */

#ifndef FSTRING_H
#define FSTRING_H

#include "config.h"
#include "mem_pool.h"

#define update_buf_size(x) (x)->free = (x)->buf->size - ((x)->pos - (x)->buf->begin); (x)->buf->len = (x)->pos - (x)->buf->begin

typedef struct f_str_s {
	gchar *begin;
	size_t len;
	size_t size;
} f_str_t;

typedef struct f_str_buf_s {
	f_str_t *buf;
	gchar *pos;
	size_t free;
} f_str_buf_t;

typedef struct f_tok_s {
	f_str_t word;
	size_t pos;
} f_tok_t;

/*
 * Search first occurence of character in string
 */
ssize_t fstrchr (f_str_t *src, gchar c);

/*
 * Search last occurence of character in string
 */
ssize_t fstrrchr (f_str_t *src, gchar c);

/*
 * Search for pattern in orig
 */
ssize_t fstrstr (f_str_t *orig, f_str_t *pattern);

/*
 * Search for pattern in orig ignoring case
 */
ssize_t fstrstri (f_str_t *orig, f_str_t *pattern);

/*
 * Split string by tokens
 * word contains parsed word
 */
gint fstrtok (f_str_t *text, const gchar *sep, f_tok_t *state);

/*
 * Copy one string into other
 */
size_t fstrcpy (f_str_t *dest, f_str_t *src);

/*
 * Concatenate two strings
 */
size_t fstrcat (f_str_t *dest, f_str_t *src);

/*
 * Push one character to fstr
 */
gint fstrpush (f_str_t *dest, gchar c);

/*
 * Push one character to fstr
 */
gint fstrpush_unichar (f_str_t *dest, gunichar c);

/*
 * Allocate memory for f_str_t
 */
f_str_t* fstralloc (memory_pool_t *pool, size_t len);

/*
 * Allocate memory for f_str_t from temporary pool
 */
f_str_t* fstralloc_tmp (memory_pool_t *pool, size_t len);

/*
 * Truncate string to its len
 */
f_str_t* fstrtruncate (memory_pool_t *pool, f_str_t *orig);

/*
 * Enlarge string to new size
 */
f_str_t* fstrgrow (memory_pool_t *pool, f_str_t *orig, size_t newlen);

/*
 * Return specified character
 */
#define fstridx(str, pos) *((str)->begin + (pos))

/*
 * Return fast hash value for fixed string
 */
guint32 fstrhash (f_str_t *str);

/*
 * Return fast hash value for fixed string converted to lowercase
 */
guint32 fstrhash_lowercase (f_str_t *str, gboolean is_utf);
/*
 * Make copy of string to 0-terminated string
 */
gchar* fstrcstr (f_str_t *str, memory_pool_t *pool);

/*
 * Strip fstr string from space symbols
 */
void fstrstrip (f_str_t *str);

#endif
