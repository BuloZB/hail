
/*
 * Copyright 2009 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define _GNU_SOURCE
#include "hail-config.h"

#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <syslog.h>
#include <glib.h>
#include <openssl/sha.h>
#include <chunk-private.h>
#include "chunkd.h"

static bool object_get_more(struct client *cli, struct client_write *wr,
			    bool done);

bool object_del(struct client *cli)
{
	int rc;
	enum chunk_errcode err = che_InternalError;
	bool rcb;
	struct chunksrv_resp *resp = NULL;

	resp = malloc(sizeof(*resp));
	if (!resp) {
		cli->state = evt_dispose;
		return true;
	}

	resp_init_req(resp, &cli->creq);

	rcb = fs_obj_delete(cli->table_id, cli->user,
			    cli->key, cli->key_len, &err);
	if (!rcb)
		return cli_err(cli, err, true);

	rc = cli_writeq(cli, resp, sizeof(*resp), cli_cb_free, resp);
	if (rc) {
		free(resp);
		return true;
	}

	return cli_write_start(cli);
}

void cli_out_end(struct client *cli)
{
	if (!cli)
		return;

	if (cli->out_bo) {
		fs_obj_free(cli->out_bo);
		cli->out_bo = NULL;
	}
	if (cli->out_ce) {
		objcache_put(&chunkd_srv.actives, cli->out_ce);
		cli->out_ce = NULL;
	}

	free(cli->out_user);
	cli->out_user = NULL;
}

static bool object_put_end(struct client *cli)
{
	unsigned char md[SHA_DIGEST_LENGTH];
	int rc;
	enum chunk_errcode err = che_InternalError;
	bool rcb;
	struct chunksrv_resp *resp = NULL;

	resp = malloc(sizeof(*resp));
	if (!resp) {
		cli->state = evt_dispose;
		return true;
	}

	resp_init_req(resp, &cli->creq);

	cli->state = evt_recycle;

	SHA1_Final(md, &cli->out_hash);

	rcb = fs_obj_write_commit(cli->out_bo, cli->out_user,
				  md, (cli->creq.flags & CHF_SYNC));
	if (!rcb)
		goto err_out;

	memcpy(resp->hash, md, sizeof(resp->hash));

	cli_out_end(cli);

	if (debugging)
		applog(LOG_DEBUG, "REQ(data-in) seq %x done code %d",
		       resp->nonce, resp->resp_code);

	rc = cli_writeq(cli, resp, sizeof(*resp), cli_cb_free, resp);
	if (rc) {
		free(resp);
		return true;
	}

	return cli_write_start(cli);

err_out:
	free(resp);
	cli_out_end(cli);
	return cli_err(cli, err, true);
}

bool cli_evt_data_in(struct client *cli, unsigned int events)
{
	char *p = cli->netbuf;
	ssize_t avail, bytes;
	size_t read_sz;

	if (!cli->out_len)
		return object_put_end(cli);

	read_sz = MIN(cli->out_len, CLI_DATA_BUF_SZ);

	if (debugging)
		applog(LOG_DEBUG, "REQ(data-in) seq %x, out_len %llu, read_sz %u",
		       cli->creq.nonce, cli->out_len, read_sz);

	if (cli->ssl) {
		int rc = SSL_read(cli->ssl, cli->netbuf, read_sz);
		if (rc <= 0) {
			if (rc == 0) {
				cli->state = evt_dispose;
				return true;
			}

			rc = SSL_get_error(cli->ssl, rc);
			if (rc == SSL_ERROR_WANT_READ)
				return false;
			if (rc == SSL_ERROR_WANT_WRITE) {
				cli->read_want_write = true;
				cli_wr_set_poll(cli, true);
				return false;
			}
			return cli_err(cli, che_InternalError, false);
		}
		avail = rc;
	} else {
		avail = read(cli->fd, cli->netbuf, read_sz);
		if (avail <= 0) {
			if (avail == 0) {
				applog(LOG_ERR, "object read(2) unexpected EOF");
				cli->state = evt_dispose;
				return true;
			}

			if (errno == EAGAIN) {
				if (debugging)
					applog(LOG_ERR, "object read(2) EAGAIN");
				return false;
			}

			cli_out_end(cli);
			applog(LOG_ERR, "object read(2) error: %s",
					strerror(errno));
			return cli_err(cli, che_InternalError, false);
		}
	}

	if (debugging && (avail != read_sz))
		applog(LOG_DEBUG, "REQ(data-in) avail %ld", (long)avail);

	while (avail > 0) {
		bytes = fs_obj_write(cli->out_bo, p, avail);
		if (bytes < 0) {
			cli_out_end(cli);
			return cli_err(cli, che_InternalError, false);
		}

		SHA1_Update(&cli->out_hash, p, bytes);

		cli->out_len -= bytes;
		p += bytes;
		avail -= bytes;
	}

	if (!cli->out_len)
		return object_put_end(cli);

	return true;
}

bool object_put(struct client *cli)
{
	const char *user = cli->user;
	uint64_t content_len = le64_to_cpu(cli->creq.data_len);
	enum chunk_errcode err;

	if (!user)
		return cli_err(cli, che_AccessDenied, true);

	cli->out_ce = objcache_get_dirty(&chunkd_srv.actives,
					 cli->key, cli->key_len);
	if (!cli->out_ce)
		return cli_err(cli, che_InternalError, true);

	cli->out_bo = fs_obj_new(cli->table_id, cli->key, cli->key_len,
				 content_len, &err);
	if (!cli->out_bo)
		return cli_err(cli, err, true);

	SHA1_Init(&cli->out_hash);
	cli->out_len = content_len;
	cli->out_user = strdup(user);

	if (!cli->out_len)
		return object_put_end(cli);

	cli->state = evt_data_in;

	return true;
}

void cli_in_end(struct client *cli)
{
	if (!cli)
		return;

	if (cli->in_obj) {
		fs_obj_free(cli->in_obj);
		cli->in_obj = NULL;
	}
}

static bool object_read_bytes(struct client *cli)
{
	if (use_sendfile(cli)) {
		if (!cli_wr_sendfile(cli, object_get_more))
			return false;
	} else {
		ssize_t bytes;

		bytes = fs_obj_read(cli->in_obj, cli->netbuf_out,
				    MIN(cli->in_len, CLI_DATA_BUF_SZ));
		if (bytes < 0)
			return false;
		if (bytes == 0 && cli->in_len != 0)
			return false;

		cli->in_len -= bytes;

		if (!cli->in_len)
			cli_in_end(cli);

		if (cli_writeq(cli, cli->netbuf_out, bytes,
			       cli->in_len ? object_get_more : NULL, NULL))
			return false;
	}

	return true;
}

static bool object_get_more(struct client *cli, struct client_write *wr,
			    bool done)
{
	/* do not queue more, if !completion or fd was closed early */
	if (!done)
		goto err_out_buf;

	if (!cli->in_len)
		cli_in_end(cli);
	else if (!object_read_bytes(cli))
		goto err_out;

	return true;

err_out:
	cli_in_end(cli);
err_out_buf:
	return false;
}

bool object_get(struct client *cli, bool want_body)
{
	int rc;
	enum chunk_errcode err = che_InternalError;
	struct backend_obj *obj;
	struct chunksrv_resp_get *get_resp = NULL;

	get_resp = calloc(1, sizeof(*get_resp));
	if (!get_resp) {
		cli->state = evt_dispose;
		return true;
	}

	resp_init_req(&get_resp->resp, &cli->creq);

	cli->in_obj = obj = fs_obj_open(cli->table_id, cli->user, cli->key,
					cli->key_len, &err);
	if (!obj) {
		free(get_resp);
		return cli_err(cli, err, true);
	}

	cli->in_len = obj->size;

	get_resp->resp.data_len = cpu_to_le64(obj->size);
	memcpy(get_resp->resp.hash, obj->hash, sizeof(obj->hash));
	get_resp->mtime = cpu_to_le64(obj->mtime);

	rc = cli_writeq(cli, get_resp, sizeof(*get_resp), cli_cb_free, get_resp);
	if (rc) {
		free(get_resp);
		return true;
	}

	if (!want_body) {
		cli_in_end(cli);

		goto start_write;
	}

	if (!cli->in_len) {
		applog(LOG_INFO, "zero-sized object");
		cli_in_end(cli);
		goto start_write;
	}

	if (!object_read_bytes(cli)) {
		cli_in_end(cli);
		return cli_err(cli, err, false);
	}

start_write:
	return cli_write_start(cli);
}

bool object_get_part(struct client *cli)
{
	static const uint64_t max_getpart = CHUNK_MAX_GETPART * CHUNK_BLK_SZ;
	int rc;
	enum chunk_errcode err = che_InternalError;
	struct backend_obj *obj;
	struct chunksrv_resp_get *get_resp = NULL;
	uint64_t offset, length, remain;
	uint64_t aligned_ofs, aligned_len, aligned_rem;
	ssize_t rrc;
	void *mem = NULL;

	get_resp = calloc(1, sizeof(*get_resp));
	if (!get_resp) {
		cli->state = evt_dispose;
		return true;
	}

	resp_init_req(&get_resp->resp, &cli->creq);

	cli->in_obj = obj = fs_obj_open(cli->table_id, cli->user, cli->key,
					cli->key_len, &err);
	if (!obj) {
		free(get_resp);
		return cli_err(cli, err, true);
	}

	cli->in_len = obj->size;

	/* obtain requested offset */
	offset = le64_to_cpu(cli->creq_getpart.offset);
	if (offset > obj->size) {
		err = che_InvalidSeek;
		free(get_resp);
		return cli_err(cli, err, true);
	}

	/* align to block boundary */
	aligned_ofs = offset & ~CHUNK_BLK_MASK;
	remain = obj->size - offset;
	aligned_rem = obj->size - aligned_ofs;

	/* obtain requested length; 0 == "until end of object" */
	length = le64_to_cpu(cli->creq.data_len);
	if (length == 0 || length > remain)
		length = remain;
	if (length > max_getpart)
		length = max_getpart;

	/* calculate length based on block size */
	aligned_len = length + (offset - aligned_ofs);
	if (aligned_len & CHUNK_BLK_MASK)
		aligned_len += (CHUNK_BLK_SZ - (aligned_len & CHUNK_BLK_MASK));
	if (aligned_len > aligned_rem)
		aligned_len = aligned_rem;

	if (length) {
		/* seek to offset */
		rc = fs_obj_seek(obj, aligned_ofs);
		if (rc) {
			err = che_InvalidSeek;
			free(get_resp);
			return cli_err(cli, err, true);
		}

		/* allocate buffer to hold all get_part request data */
		mem = malloc(aligned_len);
		if (!mem) {
			free(get_resp);
			return cli_err(cli, err, true);
		}

		/* read requested data in its entirety */
		rrc = fs_obj_read(obj, mem, aligned_len);
		if (rrc != aligned_len) {
			free(mem);
			free(get_resp);
			return cli_err(cli, err, true);
		}
	}

	/* fill in response */
	if (length == remain)
		get_resp->resp.flags |= CHF_GET_PART_LAST;
	get_resp->resp.data_len = cpu_to_le64(length);
	SHA1(mem, aligned_len, get_resp->resp.hash);
	get_resp->mtime = cpu_to_le64(obj->mtime);

	/* write response header */
	rc = cli_writeq(cli, get_resp, sizeof(*get_resp), cli_cb_free, get_resp);
	if (rc) {
		free(mem);
		free(get_resp);
		return true;
	}

	if (length) {
		/* write response data */
		rc = cli_writeq(cli, mem + (offset - aligned_ofs),
				length, cli_cb_free, mem);
		if (rc) {
			free(mem);
			free(get_resp);	/* FIXME: double-free due to
					   cli_wq success? */
			return true;
		}
	}

	cli_in_end(cli);

	return cli_write_start(cli);
}

static void worker_cp_thr(struct worker_info *wi)
{
	void *buf = NULL;
	struct client *cli = wi->cli;
	struct backend_obj *obj = NULL, *out_obj = NULL;
	enum chunk_errcode err = che_InternalError;
	unsigned char md[SHA_DIGEST_LENGTH];

	buf = malloc(CLI_DATA_BUF_SZ);
	if (!buf)
		goto out;

	cli->in_obj = obj = fs_obj_open(cli->table_id, cli->user, cli->key2,
					cli->var_len, &err);
	if (!obj)
		goto out;

	cli->in_len = obj->size;

	cli->out_ce = objcache_get_dirty(&chunkd_srv.actives,
					 cli->key, cli->key_len);
	if (!cli->out_ce)
		goto out;

	cli->out_bo = out_obj = fs_obj_new(cli->table_id,
					   cli->key, cli->key_len,
					   obj->size, &err);
	if (!cli->out_bo)
		goto out;

	SHA1_Init(&cli->out_hash);

	while (cli->in_len > 0) {
		ssize_t rrc, wrc;

		rrc = fs_obj_read(obj, buf, MIN(cli->in_len, CLI_DATA_BUF_SZ));
		if (rrc < 0)
			goto err_out;
		if (rrc == 0)
			break;

		SHA1_Update(&cli->out_hash, buf, rrc);
		cli->in_len -= rrc;

		while (rrc > 0) {
			wrc = fs_obj_write(out_obj, buf, rrc);
			if (wrc < 0)
				goto err_out;

			rrc -= wrc;
		}
	}

	SHA1_Final(md, &cli->out_hash);

	if (!fs_obj_write_commit(out_obj, cli->user, md, false))
		goto err_out;

	err = che_Success;

out:
	free(buf);
	cli_in_end(cli);
	cli_out_end(cli);
	wi->err = err;
	worker_pipe_signal(wi);
	return;

err_out:
	/* FIXME: remove half-written destination object */
	goto out;
}

static void worker_cp_pipe(struct worker_info *wi)
{
	struct client *cli = wi->cli;
	bool rcb;

	cli_rd_set_poll(cli, true);

	rcb = cli_err(cli, wi->err, (wi->err == che_Success) ? true : false);
	if (rcb) {
		short events = POLLIN;
		if (cli->writing)
			events |= POLLOUT;
		tcp_cli_event(cli->fd, events, cli);
	}

	memset(wi, 0xffffffff, sizeof(*wi));	/* poison */
	free(wi);
}

bool object_cp(struct client *cli)
{
	enum chunk_errcode err = che_InternalError;
	struct worker_info *wi;

	cli_rd_set_poll(cli, false);

	wi = calloc(1, sizeof(*wi));
	if (!wi) {
		cli_rd_set_poll(cli, true);
		return cli_err(cli, err, false);
	}

	wi->thr_ev = worker_cp_thr;
	wi->pipe_ev = worker_cp_pipe;
	wi->cli = cli;

	g_thread_pool_push(chunkd_srv.workers, wi, NULL);

	return false;
}

