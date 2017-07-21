#include "../src/config.h"
#include "../src/main.h"
#include "../src/cfg_file.h"
#include "../src/memcached.h"
#include "tests.h"

static const u_char *buf = "test";

static void 
memcached_callback (memcached_ctx_t *ctx, memc_error_t error, void *data)
{
	struct timeval tv;

	switch (ctx->op) {
		case CMD_CONNECT:
			if (error != OK) {
				msg_warn ("Connect failed, skipping test");
				memc_close_ctx (ctx);
				tv.tv_sec = 0;
				tv.tv_usec = 0;
				event_loopexit (&tv);
			}
			msg_debug ("Connect ok");
			memc_set (ctx, ctx->param, 60);
			break;
		case CMD_READ:
			g_assert (error == OK);
			g_assert (!memcmp(ctx->param->buf, buf, ctx->param->bufsize));
			msg_debug ("Read ok");
			memc_close_ctx (ctx);
			tv.tv_sec = 0;
			tv.tv_usec = 0;
			event_loopexit (&tv);
			break;
		case CMD_WRITE:
			if (error != OK) {
				msg_warn ("Connect failed, skipping test");
				memc_close_ctx (ctx);
				tv.tv_sec = 0;
				tv.tv_usec = 0;
				event_loopexit (&tv);
			}
			msg_debug ("Write ok");
			ctx->param->buf = g_malloc (sizeof (buf));
			bzero (ctx->param->buf, sizeof (buf));
			memc_get (ctx, ctx->param);
			break;
		default:
			return;
	}
}
			
void
rspamd_memcached_test_func ()
{
	memcached_ctx_t *ctx;
	memcached_param_t *param;
	struct in_addr addr;

	ctx = g_malloc (sizeof (memcached_ctx_t));
	param = g_malloc (sizeof (memcached_param_t));
	bzero (ctx, sizeof (memcached_ctx_t));
	bzero (param, sizeof (memcached_param_t));

	event_init ();

	ctx->callback = memcached_callback;
	ctx->callback_data = (void *)param;
	ctx->protocol = TCP_TEXT;
	inet_aton ("127.0.0.1", &addr);
	memcpy (&ctx->addr, &addr, sizeof (struct in_addr));
	ctx->port = htons (11211);
	ctx->timeout.tv_sec = 1;
	ctx->timeout.tv_usec = 0;
	ctx->sock = -1;
	ctx->options = MEMC_OPT_DEBUG;
	rspamd_strlcpy (param->key, buf, sizeof (param->key));
	param->buf = buf;
	param->bufsize = strlen (buf);
	ctx->param = param;
	g_assert (memc_init_ctx (ctx) != -1);

	event_loop (0);
}

