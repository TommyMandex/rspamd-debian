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

/*
 * Rspamd fuzzy storage server
 */

#include "config.h"
#include "util.h"
#include "main.h"
#include "protocol.h"
#include "upstream.h"
#include "cfg_file.h"
#include "url.h"
#include "message.h"
#include "fuzzy.h"
#include "bloom.h"
#include "map.h"
#include "fuzzy_storage.h"
#include "fuzzy_backend.h"
#include "ottery.h"

/* This number is used as expire time in seconds for cache items  (2 days) */
#define DEFAULT_EXPIRE 172800L
/* Resync value in seconds */
#define DEFAULT_SYNC_TIMEOUT 60.0


#define INVALID_NODE_TIME (guint64) - 1

/* Init functions */
gpointer init_fuzzy (struct rspamd_config *cfg);
void start_fuzzy (struct rspamd_worker *worker);

worker_t fuzzy_worker = {
	"fuzzy",                    /* Name */
	init_fuzzy,                 /* Init function */
	start_fuzzy,                /* Start function */
	TRUE,                       /* No socket */
	TRUE,                       /* Unique */
	TRUE,                       /* Threaded */
	FALSE,                      /* Non killable */
	SOCK_DGRAM                  /* UDP socket */
};

/* For evtimer */
static struct timeval tmv;
static struct event tev;
static struct rspamd_stat *server_stat;

struct rspamd_fuzzy_storage_ctx {
	char *hashfile;
	gdouble expire;
	gdouble sync_timeout;
	radix_compressed_t *update_ips;
	gchar *update_map;
	struct event_base *ev_base;

	struct rspamd_fuzzy_backend *backend;
};

struct rspamd_legacy_fuzzy_node {
	gint32 value;
	gint32 flag;
	guint64 time;
	rspamd_fuzzy_t h;
};

struct fuzzy_session {
	struct rspamd_worker *worker;
	struct rspamd_fuzzy_cmd *cmd;
	gint fd;
	guint64 time;
	gboolean legacy;
	rspamd_inet_addr_t *addr;
	struct rspamd_fuzzy_storage_ctx *ctx;
};

static gboolean
rspamd_fuzzy_check_client (struct fuzzy_session *session)
{
	if (session->ctx->update_ips != NULL) {
		if (radix_find_compressed_addr (session->ctx->update_ips,
				session->addr) == RADIX_NO_VALUE) {
			return FALSE;
		}
	}
	return TRUE;
}

static void
rspamd_fuzzy_write_reply (struct fuzzy_session *session,
		struct rspamd_fuzzy_reply *rep)
{
	gint r;
	gchar buf[64];

	if (session->legacy) {
		if (rep->prob > 0.5) {
			if (session->cmd->cmd == FUZZY_CHECK) {
				r = rspamd_snprintf (buf, sizeof (buf), "OK %d %d" CRLF,
						rep->value, rep->flag);
			}
			else {
				r = rspamd_snprintf (buf, sizeof (buf), "OK" CRLF);
			}

		}
		else {
			r = rspamd_snprintf (buf, sizeof (buf), "ERR" CRLF);
		}
		r = rspamd_inet_address_sendto (session->fd, buf, r, 0, session->addr);
	}
	else {
		r = rspamd_inet_address_sendto (session->fd, rep, sizeof (*rep), 0,
				session->addr);
	}

	if (r == -1) {
		if (errno == EINTR) {
			rspamd_fuzzy_write_reply (session, rep);
		}
		else {
			msg_err ("error while writing reply: %s", strerror (errno));
		}
	}
}

static void
rspamd_fuzzy_process_command (struct fuzzy_session *session,
		enum rspamd_fuzzy_epoch epoch)
{
	struct rspamd_fuzzy_reply rep = {0, 0, 0, 0.0};
	gboolean res = FALSE;

	if (session->cmd->cmd == FUZZY_CHECK) {
		rep = rspamd_fuzzy_backend_check (session->ctx->backend, session->cmd,
				session->ctx->expire);
		/* XXX: actually, these updates are not atomic, but we don't care */
		server_stat->fuzzy_hashes_checked[epoch] ++;

		if (rep.prob > 0.5) {
			server_stat->fuzzy_hashes_found[epoch] ++;
		}
	}
	else {
		rep.flag = session->cmd->flag;
		if (rspamd_fuzzy_check_client (session)) {
			if (session->cmd->cmd == FUZZY_WRITE) {
				res = rspamd_fuzzy_backend_add (session->ctx->backend,
						session->cmd);
			}
			else {
				res = rspamd_fuzzy_backend_del (session->ctx->backend,
						session->cmd);
			}
			if (!res) {
				rep.value = 404;
				rep.prob = 0.0;
			}
			else {
				rep.value = 0;
				rep.prob = 1.0;
			}
		}
		else {
			rep.value = 403;
			rep.prob = 0.0;
		}
		server_stat->fuzzy_hashes = rspamd_fuzzy_backend_count (session->ctx->backend);
	}

	rep.tag = session->cmd->tag;
	rspamd_fuzzy_write_reply (session, &rep);
}


static enum rspamd_fuzzy_epoch
rspamd_fuzzy_command_valid (struct rspamd_fuzzy_cmd *cmd, gint r)
{
	enum rspamd_fuzzy_epoch ret = RSPAMD_FUZZY_EPOCH_MAX;

	if (cmd->version == RSPAMD_FUZZY_VERSION) {
		if (cmd->shingles_count > 0) {
			if (r == sizeof (struct rspamd_fuzzy_shingle_cmd)) {
				ret = RSPAMD_FUZZY_EPOCH9;
			}
		}
		else {
			if (r == sizeof (*cmd)) {
				ret = RSPAMD_FUZZY_EPOCH9;
			}
		}
	}
	else if (cmd->version == 2) {
		/*
		 * rspamd 0.8 has slightly different tokenizer then it might be not
		 * 100% compatible
		 */
		if (cmd->shingles_count > 0) {
			if (r == sizeof (struct rspamd_fuzzy_shingle_cmd)) {
				ret = RSPAMD_FUZZY_EPOCH8;
			}
		}
		else {
			ret = RSPAMD_FUZZY_EPOCH8;
		}
	}

	return ret;
}
/*
 * Accept new connection and construct task
 */
static void
accept_fuzzy_socket (gint fd, short what, void *arg)
{
	struct rspamd_worker *worker = (struct rspamd_worker *)arg;
	struct fuzzy_session session;
	gint r;
	guint8 buf[2048];
	struct rspamd_fuzzy_cmd *cmd = NULL, lcmd;
	struct legacy_fuzzy_cmd *l;
	enum rspamd_fuzzy_epoch epoch = RSPAMD_FUZZY_EPOCH_MAX;

	session.worker = worker;
	session.fd = fd;
	session.ctx = worker->ctx;
	session.time = (guint64)time (NULL);

	/* Got some data */
	if (what == EV_READ) {
		while ((r = rspamd_inet_address_recvfrom (fd, buf, sizeof (buf), 0,
			&session.addr)) == -1) {
			if (errno == EINTR) {
				continue;
			}
			msg_err ("got error while reading from socket: %d, %s",
				errno,
				strerror (errno));
			return;
		}

		if ((guint)r == sizeof (struct legacy_fuzzy_cmd)) {
			session.legacy = TRUE;
			l = (struct legacy_fuzzy_cmd *)buf;
			lcmd.version = 2;
			memcpy (lcmd.digest, l->hash, sizeof (lcmd.digest));
			lcmd.cmd = l->cmd;
			lcmd.flag = l->flag;
			lcmd.shingles_count = 0;
			lcmd.value = l->value;
			lcmd.tag = 0;
			cmd = &lcmd;
			epoch = RSPAMD_FUZZY_EPOCH6;
		}
		else if ((guint)r >= sizeof (struct rspamd_fuzzy_cmd)) {
			/* Check shingles count sanity */
			session.legacy = FALSE;
			cmd = (struct rspamd_fuzzy_cmd *)buf;
			epoch = rspamd_fuzzy_command_valid (cmd, r);
			if (epoch == RSPAMD_FUZZY_EPOCH_MAX) {
				/* Bad input */
				msg_debug ("invalid fuzzy command of size %d received", r);
				cmd = NULL;
			}
		}
		else {
			/* Discard input */
			msg_debug ("invalid fuzzy command of size %d received", r);
		}
		if (cmd != NULL) {
			session.cmd = cmd;
			rspamd_fuzzy_process_command (&session, epoch);
		}

		rspamd_inet_address_destroy (session.addr);
	}
}

static void
sync_callback (gint fd, short what, void *arg)
{
	struct rspamd_worker *worker = (struct rspamd_worker *)arg;
	struct rspamd_fuzzy_storage_ctx *ctx;
	gdouble next_check;

	ctx = worker->ctx;
	/* Timer event */
	evtimer_set (&tev, sync_callback, worker);
	event_base_set (ctx->ev_base, &tev);
	/* Plan event with jitter */
	next_check = ctx->sync_timeout * (1. + ((gdouble)ottery_rand_uint32 ()) /
			G_MAXUINT32);
	double_to_tv (next_check, &tmv);
	evtimer_add (&tev, &tmv);

	/* Call backend sync */
	rspamd_fuzzy_backend_sync (ctx->backend, ctx->expire);

	server_stat->fuzzy_hashes_expired = rspamd_fuzzy_backend_expired (ctx->backend);
}

gpointer
init_fuzzy (struct rspamd_config *cfg)
{
	struct rspamd_fuzzy_storage_ctx *ctx;
	GQuark type;

	type = g_quark_try_string ("fuzzy");

	ctx = g_malloc0 (sizeof (struct rspamd_fuzzy_storage_ctx));

	ctx->sync_timeout = DEFAULT_SYNC_TIMEOUT;

	rspamd_rcl_register_worker_option (cfg, type, "hashfile",
		rspamd_rcl_parse_struct_string, ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, hashfile), 0);

	rspamd_rcl_register_worker_option (cfg, type, "hash_file",
		rspamd_rcl_parse_struct_string, ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, hashfile), 0);

	rspamd_rcl_register_worker_option (cfg, type, "file",
		rspamd_rcl_parse_struct_string, ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, hashfile), 0);

	rspamd_rcl_register_worker_option (cfg, type, "database",
		rspamd_rcl_parse_struct_string, ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, hashfile), 0);

	rspamd_rcl_register_worker_option (cfg, type, "sync",
		rspamd_rcl_parse_struct_time, ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx,
		sync_timeout), RSPAMD_CL_FLAG_TIME_FLOAT);

	rspamd_rcl_register_worker_option (cfg, type, "expire",
		rspamd_rcl_parse_struct_time, ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx,
		expire), RSPAMD_CL_FLAG_TIME_FLOAT);

	rspamd_rcl_register_worker_option (cfg, type, "allow_update",
		rspamd_rcl_parse_struct_string, ctx,
		G_STRUCT_OFFSET (struct rspamd_fuzzy_storage_ctx, update_map), 0);


	return ctx;
}

/*
 * Start worker process
 */
void
start_fuzzy (struct rspamd_worker *worker)
{
	struct rspamd_fuzzy_storage_ctx *ctx = worker->ctx;
	GError *err = NULL;
	gdouble next_check;

	ctx->ev_base = rspamd_prepare_worker (worker,
			"fuzzy",
			accept_fuzzy_socket);
	server_stat = worker->srv->stat;


	if ((ctx->backend = rspamd_fuzzy_backend_open (ctx->hashfile, &err)) == NULL) {
		msg_err ("cannot open backend: %e", err);
		g_error_free (err);
		exit (EXIT_FAILURE);
	}

	server_stat->fuzzy_hashes = rspamd_fuzzy_backend_count (ctx->backend);

	/* Timer event */
	evtimer_set (&tev, sync_callback, worker);
	event_base_set (ctx->ev_base, &tev);
	/* Plan event with jitter */
	next_check = ctx->sync_timeout * (1. + ((gdouble)ottery_rand_uint32 ()) /
			G_MAXUINT32);
	double_to_tv (next_check, &tmv);
	evtimer_add (&tev, &tmv);

	/* Create radix tree */
	if (ctx->update_map != NULL) {
		if (!rspamd_map_add (worker->srv->cfg, ctx->update_map,
			"Allow fuzzy updates from specified addresses",
			rspamd_radix_read, rspamd_radix_fin, (void **)&ctx->update_ips)) {
			if (!radix_add_generic_iplist (ctx->update_map,
				&ctx->update_ips)) {
				msg_warn ("cannot load or parse ip list from '%s'",
					ctx->update_map);
			}
		}
	}

	/* Maps events */
	rspamd_map_watch (worker->srv->cfg, ctx->ev_base);

	event_base_loop (ctx->ev_base, 0);

	rspamd_fuzzy_backend_sync (ctx->backend, ctx->expire);
	rspamd_fuzzy_backend_close (ctx->backend);
	rspamd_log_close (rspamd_main->logger);
	exit (EXIT_SUCCESS);
}
