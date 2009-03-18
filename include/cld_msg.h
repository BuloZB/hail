#ifndef __CLD_MSG_H__
#define __CLD_MSG_H__

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


#include <stdint.h>

#define CLD_MAGIC	"CLDv1cli"

enum {
	CLD_MAGIC_SZ		= 8,
	CLD_SID_SZ		= 8,
	CLD_MSGID_SZ		= 8,

	CLD_MAX_DATA_MSGS	= 1024,		/* max data msgs in a stream */
};

enum cld_msg_ops {
	/* client -> server */
	cmo_nop			= 0,		/* no op */
	cmo_new_sess		= 1,		/* new session */
	cmo_open		= 2,		/* open file */
	cmo_get_meta		= 3,		/* get metadata */
	cmo_get			= 4,		/* get metadata + data */
	cmo_data		= 5,		/* data message */
	cmo_put			= 6,		/* put data */
	cmo_close		= 7,		/* close file */
	cmo_del			= 8,		/* delete file */
	cmo_lock		= 9,		/* lock */
	cmo_unlock		= 10,		/* unlock */
	cmo_trylock		= 11,		/* trylock */
	cmo_ack			= 12,		/* client ack of msgid rx'd */
	cmo_end_sess		= 13,		/* end session */

	/* server -> client */
	cmo_ping		= 30,		/* server to client ping */
	cmo_not_master		= 31,		/* I am not the master! */
	cmo_event		= 32,		/* server->cli async event */
};

enum cle_err_codes {
	CLE_OK			= 0,		/* success / no error */
	CLE_SESS_EXISTS		= 1,		/* session exists */
	CLE_SESS_INVAL		= 2,		/* session doesn't exist */
	CLE_DB_ERR		= 3,		/* db error */
	CLE_BAD_PKT		= 4,		/* invalid/corrupted packet */
	CLE_INODE_INVAL		= 5,		/* inode doesn't exist */
	CLE_NAME_INVAL		= 6,		/* inode name invalid */
	CLE_OOM			= 7,		/* server out of memory */
	CLE_FH_INVAL		= 8,		/* file handle invalid */
	CLE_DATA_INVAL		= 9,		/* invalid data pkt */
	CLE_LOCK_INVAL		= 10,		/* invalid lock */
	CLE_LOCK_CONFLICT	= 11,		/* conflicting lock held */
	CLE_LOCK_PENDING	= 12,		/* lock waiting to be acq. */
	CLE_MODE_INVAL		= 13,		/* op incompat. w/ file mode */
	CLE_INODE_EXISTS	= 14,		/* inode exists */
	CLE_DIR_NOTEMPTY	= 15,		/* dir not empty */
};

enum cld_open_modes {
	COM_READ		= (1 << 0),	/* read */
	COM_WRITE		= (1 << 1),	/* write */
	COM_LOCK		= (1 << 2),	/* lock */
	COM_ACL			= (1 << 3),	/* ACL update */
	COM_CREATE		= (1 << 4),	/* create file, if not exist */
	COM_EXCL		= (1 << 5),	/* fail create if file exists */
	COM_DIRECTORY		= (1 << 6),	/* operate on a directory */
};

enum cld_events {
	CE_UPDATED		= (1 << 0),	/* contents updated */
	CE_DELETED		= (1 << 1),	/* inode deleted */
	CE_LOCKED		= (1 << 2),	/* lock acquired */
	CE_MASTER_FAILOVER	= (1 << 3),	/* master failover */
};

enum cld_lock_flags {
	CLF_SHARED		= (1 << 0),	/* a shared (read) lock */
};

struct cld_msg_hdr {
	uint8_t		magic[CLD_MAGIC_SZ];	/* magic number; constant */
	uint8_t		msgid[CLD_MSGID_SZ];	/* message id */
	uint8_t		sid[CLD_SID_SZ];	/* client id */
	uint8_t		op;			/* operation code */
	uint8_t		res1[7];
};

struct cld_msg_resp {
	struct cld_msg_hdr	hdr;

	uint32_t		code;		/* error code, CLE_xxx */
};

struct cld_msg_open {
	struct cld_msg_hdr	hdr;

	uint32_t		mode;		/* open mode, COM_xxx */
	uint32_t		events;		/* events mask, CE_xxx */
	uint16_t		name_len;	/* length of file name */
	/* inode name */
};

struct cld_msg_resp_open {
	struct cld_msg_hdr	hdr;

	uint32_t		code;		/* error code, CLE_xxx */
	uint64_t		fh;		/* handle opened */
};

struct cld_msg_get {
	struct cld_msg_hdr	hdr;

	uint64_t		fh;		/* open file handle */
};

struct cld_msg_get_resp {
	struct cld_msg_hdr	hdr;

	/* should mirror struct raw_inode, except that inum's type
	 * should always be uint64_t, regardless of server's
	 * cldino_t definition
	 */
	uint64_t		inum;		/* unique inode number */
	uint32_t		ino_len;	/* inode name len */
	uint32_t		size;		/* data size */
	uint64_t		version;	/* inode version */
	uint64_t		time_create;
	uint64_t		time_modify;
	uint32_t		flags;		/* inode flags; CIFL_xxx */
	/* inode name */
};

struct cld_msg_data {
	struct cld_msg_hdr	hdr;

	uint8_t			strid[CLD_MSGID_SZ]; /* stream id */
	uint32_t		seg;		/* segment number */
	uint32_t		seg_len;	/* segment length */
};

struct cld_msg_data_resp {
	struct cld_msg_hdr	hdr;

	uint8_t			strid[CLD_MSGID_SZ]; /* stream id */
	uint32_t		seg;		/* segment number */
	uint32_t		seg_len;	/* segment length */
};

struct cld_msg_put {
	struct cld_msg_hdr	hdr;

	uint64_t		fh;		/* open file handle */
	uint32_t		data_size;	/* total size of data */
};

struct cld_msg_close {
	struct cld_msg_hdr	hdr;

	uint64_t		fh;		/* open file handle */
};

struct cld_msg_del {
	struct cld_msg_hdr	hdr;

	uint16_t		name_len;	/* length of file name */
	/* inode name */
};

struct cld_msg_unlock {
	struct cld_msg_hdr	hdr;

	uint64_t		fh;		/* open file handle */
};

struct cld_msg_lock {
	struct cld_msg_hdr	hdr;

	uint64_t		fh;		/* open file handle */
	uint32_t		flags;		/* CLF_xxx */
};

struct cld_msg_event {
	struct cld_msg_hdr	hdr;

	uint64_t		fh;		/* open file handle */
	uint32_t		events;		/* CE_xxx */
};

#endif /* __CLD_MSG_H__ */