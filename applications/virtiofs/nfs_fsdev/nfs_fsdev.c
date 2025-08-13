/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "priv_nfs_fsdev.h"
#include "nfs_fsdev.h"
#include "nfs_fsdev_io_device.h"
#include <doca_log.h>
#include "nfsc/libnfs-raw-nfs.h"
#include <doca_devemu_vfs_fuse_kernel.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/types.h>
#include <poll.h>
#include <sys/queue.h>
#include <sched.h>
#include <time.h>
#include <inttypes.h>
#include <stdlib.h>
#include <errno.h>
#include <doca_error.h>

DOCA_LOG_REGISTER(NFS_FSDEV)

/* File type bits (octal, as per POSIX/SUS) */
#define BITS_MASK 0170000    /* Mask for file type bits */
#define REGULAR_FILE 0100000 /* Regular file */

/* Attribute set flags for setattr operations */
#define _FSDEV_SET_ATTR_MODE (1 << 0)
#define _FSDEV_SET_ATTR_UID (1 << 1)
#define _FSDEV_SET_ATTR_GID (1 << 2)
#define _FSDEV_SET_ATTR_SIZE (1 << 3)
#define _FSDEV_SET_ATTR_ATIME (1 << 4)
#define _FSDEV_SET_ATTR_MTIME (1 << 5)
#define _FSDEV_SET_ATTR_ATIME_NOW (1 << 6)
#define _FSDEV_SET_ATTR_MTIME_NOW (1 << 7)
#define _FSDEV_SET_ATTR_CTIME (1 << 8)

/* Special constants */
#define INVALID_INODE 0
#define XID_OFFSET 400000
#define FUSE_COMPAT_ENTRY_OUT_SIZE 120

/**
 * @brief Calculate the XID for a given domain and request ID.
 * @param src_domain_id Source domain ID (must be < 256).
 * @param req_id Request ID (lower 24 bits used).
 * @return The calculated XID.
 */
static uint32_t nfs_fsdev_get_xid(uint16_t src_domain_id, uint64_t req_id)
{
	if (src_domain_id >= 256) {
		DOCA_LOG_ERR("src_domain_id=%u exceeds max allowed (255)", src_domain_id);
		return 0;
	}

	return (src_domain_id << 24) | (req_id & 0xFFFFFF);
}

struct unlink_cb_data {
	struct async_context *context;
	unsigned long inode_for_deletion;
};

static void nfs_fsdev_complete(struct async_context *context, size_t len, int status);
static void nimp(struct async_context *context);

static struct async_context *alloc_init_async_context(struct nfs_fsdev_global *global_fsdev,
						      struct nfs_fsdev *perthread_fsdev,
						      char *fuse_header,
						      char *cmnd_in_hdr,
						      char *datain,
						      char *fuse_out,
						      char *cmnd_out_hdr,
						      char *dataout,
						      nfs_fsdev_io_cb cb,
						      void *ctx,
						      uint16_t src_domain_id,
						      uint64_t req_id)
{
	struct async_context *res;

	if (SLIST_EMPTY(&perthread_fsdev->free_ios)) {
		DOCA_LOG_ERR("No free async_context objects in perthread_fsdev (src_domain_id=%u, req_id=%" PRIu64 ")",
			     src_domain_id,
			     req_id);
		return NULL;
	}

	res = SLIST_FIRST(&perthread_fsdev->free_ios);
	SLIST_REMOVE_HEAD(&perthread_fsdev->free_ios, entry);

	res->fsdev_global = global_fsdev;
	res->fsdev_perthread = perthread_fsdev;
	res->fuse_header = fuse_header;
	res->cmnd_in_hdr = cmnd_in_hdr;
	res->datain = datain;
	res->fuse_out = fuse_out;
	res->cmnd_out_hdr = cmnd_out_hdr;
	res->dataout = dataout;
	res->app_cb = cb;
	res->app_ctx = ctx;
	res->src_domain_id = src_domain_id;
	res->req_id = (uint32_t)req_id;

	return res;
}

/**
 * @brief Get the device number for a file attribute.
 * @param res File attribute structure.
 * @return Device number, or 0 if not a device.
 */
static uint32_t nfs_fsdev_get_rdev(const fattr3 *res)
{
	if (S_ISCHR(res->mode) || S_ISBLK(res->mode)) {
		return makedev(res->rdev.specdata1, res->rdev.specdata2);
	} else {
		return 0;
	}
}

/**
 * @brief Get the mode for a file attribute.
 * @param res File attribute structure.
 * @return Mode value.
 */
static uint32_t nfs_fsdev_get_file_mode(const fattr3 *res)
{
	uint32_t mode = 0;

	switch (res->type) {
	case NF3REG:
		mode = 0100000 + res->mode; // Regular file
		break;
	case NF3DIR:
		mode = 0040000 + res->mode; // Directory
		break;
	case NF3BLK:
		mode = 0060000 + res->mode; // Block special
		break;
	case NF3CHR:
		mode = 0020000 + res->mode; // Character special
		break;
	case NF3LNK:
		mode = 0120000 + res->mode; // Symbolic link
		break;
	case NF3SOCK:
		mode = 0140000 + res->mode; // Socket
		break;
	case NF3FIFO:
		mode = 0010000 + res->mode; // FIFO
		break;
	default:
		// Handle unexpected file type as Regular file
		mode = 0100000 + res->mode;
		DOCA_LOG_WARN("Unexpected file type (%d), treating as regular file", res->type);
		break;
	}

	return mode;
}

/**
 * @brief Fill a FUSE attribute structure from NFS file attributes.
 * @param attr FUSE attribute structure to fill.
 * @param res NFS file attribute structure.
 * @param ino Inode number.
 */
static void nfs_fsdev_fill_internal_attr(struct fuse_attr *attr, const fattr3 *res, int ino)
{
	attr->ino = ino;
	attr->mode = nfs_fsdev_get_file_mode(res);
	attr->nlink = res->nlink;
	attr->uid = res->uid;
	attr->gid = res->gid;
	attr->rdev = nfs_fsdev_get_rdev(res);
	attr->size = res->size;
	attr->blksize = 4096;
}

static void nfs_fsdev_fill_getattr_entry(struct fuse_attr_out *arg, fattr3 *res, int inode)
{
	arg->attr_valid = 300;
	arg->attr_valid_nsec = 0;
	nfs_fsdev_fill_internal_attr(&(arg->attr), res, inode);
}

static size_t nfs_fsdev_fill_lookup_entry(struct fuse_entry_out *arg, fattr3 *res, int inode)
{
	arg->nodeid = inode;
	arg->generation = 0;
	arg->entry_valid = 0;
	arg->entry_valid_nsec = 0;
	arg->attr_valid = 0;
	arg->attr_valid_nsec = 0;
	nfs_fsdev_fill_internal_attr(&(arg->attr), res, inode);

	return sizeof(*arg);
}

static void nfs_fsdev_getattr_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	struct async_context *ctx = private_data;
	pthread_mutex_lock(&ctx->fsdev_global->lock);
	struct fuse_in_header *hdr = (struct fuse_in_header *)(ctx->fuse_header);
	struct fuse_attr_out *outarg = (struct fuse_attr_out *)(ctx->cmnd_out_hdr);

	if (status == RPC_STATUS_ERROR) {
		DOCA_LOG_ERR("NFS getattr failed with error: %s", (char *)data);
		exit(1);
	} else if (status == RPC_STATUS_CANCEL) {
		DOCA_LOG_ERR("NFS getattr operation was cancelled");
		exit(1);
	}

	GETATTR3res *result = data;
	if (result->status != NFS3_OK) {
		DOCA_LOG_ERR("failed with error:%d\n", result->status);
		exit(1);
	}

	fattr3 *res = &result->GETATTR3res_u.resok.obj_attributes;
	nfs_fsdev_fill_getattr_entry(outarg, res, hdr->nodeid);
	nfs_fsdev_complete(ctx, sizeof(*outarg), 0);
}

static struct GETATTR3args nfs_fsdev_getattr_args(struct NfsFsdevEntry *entry)
{
	struct GETATTR3args args = {0};
	args.object.data.data_len = entry->fh_right_key.data.data_len;
	args.object.data.data_val = entry->fh_right_key.data.data_val;
	return args;
}

static void nfs_fsdev_getattr(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);

	if (check_if_exist_by_left(context->fsdev_global->db, hdr->nodeid) == false) {
		DOCA_LOG_ERR("No entry found for inode=%lu", hdr->nodeid);
		nfs_fsdev_complete(context, 0, -ENOENT);
		return;
		exit(1);
	}

	struct NfsFsdevEntry temp = get_entry_by_left(context->fsdev_global->db, hdr->nodeid);
	if (temp.state == NFS_FSDEV_PENDING_DELETION_STATE && temp.ref_count == 0) {
		DOCA_LOG_WARN("Attempt to get attributes for inode=%lu which is pending deletion", hdr->nodeid);
		exit(1);
	}

	struct GETATTR3args args = nfs_fsdev_getattr_args(&temp);

	pthread_mutex_unlock(&context->fsdev_global->lock);
	if (rpc_nfs3_getattr_async(nfs_get_rpc_context(context->fsdev_perthread->nfs),
				   nfs_fsdev_getattr_cb,
				   &args,
				   context)) {
		DOCA_LOG_WARN("Failed to start async NFS getattr for inode=%lu", hdr->nodeid);
		nfs_fsdev_complete(context, 0, -ENOENT);
		return;
	}
}

static void nfs_fsdev_setattr_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	struct async_context *ctx = private_data;
	pthread_mutex_lock(&ctx->fsdev_global->lock);
	struct fuse_in_header *hdr = (struct fuse_in_header *)(ctx->fuse_header);
	struct fuse_out_header *out_header = (struct fuse_out_header *)(ctx->fuse_out);
	struct fuse_attr_out *outarg = (struct fuse_attr_out *)(out_header + 1);

	if (status == RPC_STATUS_ERROR) {
		DOCA_LOG_ERR("NFS setattr failed with error: %s", (char *)data);
		exit(1);
	} else if (status == RPC_STATUS_CANCEL) {
		DOCA_LOG_ERR("NFS setattr operation was cancelled");
		exit(1);
	}

	struct SETATTR3res *result = data;
	fattr3 *res = &result->SETATTR3res_u.resok.obj_wcc.after.post_op_attr_u.attributes;

	memset(outarg, 0, sizeof(*outarg));

	outarg->attr_valid = 0;
	outarg->attr_valid_nsec = 0;
	outarg->attr.ino = hdr->nodeid;
	outarg->attr.mode = nfs_fsdev_get_file_mode(res);
	outarg->attr.nlink = res->nlink;
	outarg->attr.uid = res->uid;
	outarg->attr.gid = res->gid;
	outarg->attr.rdev = nfs_fsdev_get_rdev(res);
	outarg->attr.size = res->size;
	outarg->attr.blksize = 4096;
	outarg->attr.blocks = (res->size + 511) / 512;
	outarg->attr.atime = res->atime.seconds;
	outarg->attr.mtime = res->mtime.seconds;
	outarg->attr.ctime = res->ctime.seconds;
	outarg->attr.atimensec = res->atime.nseconds;
	outarg->attr.mtimensec = res->mtime.nseconds;
	outarg->attr.ctimensec = res->ctime.nseconds;

	nfs_fsdev_complete(ctx, sizeof(*outarg), 0);
}

static struct SETATTR3args nfs_fsdev_setattr_args(struct async_context *context, struct NfsFsdevEntry *entry)
{
	struct fuse_setattr_in *hdr_in = (struct fuse_setattr_in *)(context->cmnd_in_hdr);
	struct SETATTR3args args = {0};
	uint32_t to_set = hdr_in->valid;

	args.object.data.data_len = entry->fh_right_key.data.data_len;
	args.object.data.data_val = entry->fh_right_key.data.data_val;
	if (to_set & FATTR_FH) {
		to_set &= ~FATTR_FH;
	}

	to_set &= _FSDEV_SET_ATTR_MODE | _FSDEV_SET_ATTR_UID | _FSDEV_SET_ATTR_GID | _FSDEV_SET_ATTR_SIZE |
		  _FSDEV_SET_ATTR_ATIME | _FSDEV_SET_ATTR_MTIME | _FSDEV_SET_ATTR_ATIME_NOW |
		  _FSDEV_SET_ATTR_MTIME_NOW | _FSDEV_SET_ATTR_CTIME;

	if (to_set & _FSDEV_SET_ATTR_ATIME) {
		args.new_attributes.atime.set_it = 1;
		args.new_attributes.atime.set_atime_u.atime.seconds = hdr_in->atime;
		args.new_attributes.atime.set_atime_u.atime.nseconds = hdr_in->atimensec;
	}

	if (to_set & _FSDEV_SET_ATTR_MTIME) {
		args.new_attributes.mtime.set_it = 1;
		args.new_attributes.mtime.set_mtime_u.mtime.seconds = hdr_in->mtime;
		args.new_attributes.mtime.set_mtime_u.mtime.nseconds = hdr_in->mtimensec;
	}

	if (to_set & _FSDEV_SET_ATTR_ATIME_NOW) {
		args.new_attributes.atime.set_it = 2;
	}

	if (to_set & _FSDEV_SET_ATTR_MTIME_NOW) {
		args.new_attributes.mtime.set_it = 2;
	}

	if (to_set & _FSDEV_SET_ATTR_UID) {
		args.new_attributes.uid.set_it = 1;
		args.new_attributes.uid.set_uid3_u.uid = hdr_in->uid;
	}

	if (to_set & _FSDEV_SET_ATTR_GID) {
		args.new_attributes.gid.set_it = 1;
		args.new_attributes.gid.set_gid3_u.gid = hdr_in->gid;
	}

	if (to_set & _FSDEV_SET_ATTR_SIZE) {
		args.new_attributes.size.set_it = 1;
		args.new_attributes.size.set_size3_u.size = hdr_in->size;
	}

	if (to_set & _FSDEV_SET_ATTR_MODE) {
		args.new_attributes.mode.set_it = 1;
		args.new_attributes.mode.set_mode3_u.mode = hdr_in->mode & 0777; // should check about this part
	}

	return args;
}

static void nfs_fsdev_setattr(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);

	if (!check_if_exist_by_left(context->fsdev_global->db, hdr->nodeid)) {
		DOCA_LOG_ERR("unknown file - can't set attributes");
		nfs_fsdev_complete(context, 0, -ENOENT);
		return;
	}
	struct NfsFsdevEntry entry = get_entry_by_left(context->fsdev_global->db, hdr->nodeid);
	if (entry.state == NFS_FSDEV_PENDING_DELETION_STATE) {
		DOCA_LOG_WARN("Trying to make I/O request on inode that is pending deletion");
		nfs_fsdev_complete(context, 0, -ENOENT);
		return;
	}

	struct SETATTR3args args = nfs_fsdev_setattr_args(context, &entry);
	pthread_mutex_unlock(&context->fsdev_global->lock);
	if (rpc_nfs3_setattr_async(nfs_get_rpc_context(context->fsdev_perthread->nfs),
				   nfs_fsdev_setattr_cb,
				   &args,
				   context)) {
		DOCA_LOG_ERR("in setting attributes ");
		exit(1);
	}
}

static void nfs_fsdev_init(struct async_context *context)
{
	struct fuse_init_out *outarg = (struct fuse_init_out *)context->cmnd_out_hdr;

	outarg->major = 7;
	outarg->minor = 34;
	outarg->max_readahead = 131072;
	outarg->flags = 1004469371;
	outarg->max_background = 65535;
	outarg->congestion_threshold = 65535;
	outarg->max_write = 131072;
	outarg->time_gran = 1;
	outarg->max_pages = 32;
	outarg->map_alignment = 0;
	memset(outarg->unused, 0, sizeof(outarg->unused));

	nfs_fsdev_complete(context, sizeof(*outarg), 0);
}

static void nfs_fsdev_lookup_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	struct async_context *ctx = private_data;
	pthread_mutex_lock(&ctx->fsdev_global->lock);

	struct fuse_entry_out *entry_out = (struct fuse_entry_out *)ctx->cmnd_out_hdr;

	if (status == RPC_STATUS_ERROR) {
		DOCA_LOG_ERR("LOOKUP RPC failed with error [%s]", (char *)data);
		exit(1);
	} else if (status == RPC_STATUS_CANCEL) {
		DOCA_LOG_ERR("LOOKUP failed: operation was cancelled");
		exit(1);
	}

	struct LOOKUP3res *result = data;
	nfsstat3 ret = result->status;
	if (ret != NFS3_OK) {
		if (ret == NFS3ERR_NOENT) {
			DOCA_LOG_INFO("lookup result is NFS3ERR_NOENT");
			memset(entry_out, 0, sizeof(*entry_out));
			nfs_fsdev_complete(ctx, 0, -ENOENT);
		} else {
			DOCA_LOG_ERR("lookup result is other than OK or NOENT = [%d]", ret);
			exit(1);
		}

		return;
	}

	struct persistent_nfs_fh3 fh = {0};
	fh.data.data_len = result->LOOKUP3res_u.resok.object.data.data_len;
	memcpy(fh.data.data_val, result->LOOKUP3res_u.resok.object.data.data_val, fh.data.data_len);
	unsigned long inode;

	if (check_if_exist_by_right(ctx->fsdev_global->db, &fh)) {
		struct NfsFsdevEntry temp = get_entry_by_right(ctx->fsdev_global->db, &fh);
		if (temp.state == NFS_FSDEV_PENDING_DELETION_STATE && temp.ref_count == 0) {
			DOCA_LOG_ERR("Attempt to make I/O request on a file that is pending deletion");
			exit(1);
		}

		inode = temp.inode_left_key;
	} else {
		inode = generate_left_key(ctx->fsdev_global->db);
		if (!nfs_fsdev_db_insert(ctx->fsdev_global->db,
					 NFS_FSDEV_REGULAR_STATE,
					 0,
					 inode,
					 &result->LOOKUP3res_u.resok.object)) {
			DOCA_LOG_ERR("Failed to insert new entry to our data base");
			exit(1);
		}
	}

	fattr3 *res = &result->LOOKUP3res_u.resok.obj_attributes.post_op_attr_u.attributes;
	size_t size_temp = nfs_fsdev_fill_lookup_entry(entry_out, res, inode);
	nfs_fsdev_complete(ctx, size_temp, 0);
}

static void nfs_fsdev_lookup(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);
	char *name = strdup(context->datain);
	if (name == NULL) {
		DOCA_LOG_ERR("Failed to duplicate name");
		exit(1);
	}

	if (hdr->nodeid == 0) {
		DOCA_LOG_ERR("calling lookup with parent inode 0");
		exit(1);
	}

	if (check_if_exist_by_left(context->fsdev_global->db, hdr->nodeid) == false) {
		DOCA_LOG_ERR("parent directory not found");
		exit(1);
	}

	struct NfsFsdevEntry temp = get_entry_by_left(context->fsdev_global->db, hdr->nodeid);
	if (temp.state == NFS_FSDEV_PENDING_DELETION_STATE) {
		DOCA_LOG_WARN("Attempt to make I/O request on a file with parent directory that is pending deletion");
		exit(1);
	}

	struct LOOKUP3args args = {0};
	args.what.dir.data.data_len = temp.fh_right_key.data.data_len;
	args.what.dir.data.data_val = temp.fh_right_key.data.data_val;
	args.what.name = name;

	pthread_mutex_unlock(&context->fsdev_global->lock);
	if (rpc_nfs3_lookup_async(nfs_get_rpc_context(context->fsdev_perthread->nfs),
				  nfs_fsdev_lookup_cb,
				  &args,
				  context)) {
		DOCA_LOG_ERR("Failed to start lookup");
		exit(1);
	}
}

static size_t nfs_fsdev_fill_fuse_dirent(struct fuse_dirent *dirent,
					 entryplus3 *curr_entry,
					 unsigned long inode,
					 size_t remaining,
					 size_t ANTON)
{
	size_t namelen;
	size_t entlen;
	size_t entlen_padded;

	namelen = strlen(curr_entry->name);
	entlen = ANTON + namelen;
	entlen_padded = FUSE_DIRENT_ALIGN(entlen);

	if (entlen_padded > remaining) {
		DOCA_LOG_ERR(
			"BUFFER OVERFLOW: Entriesize=%lu, Remaining=%lu, ino=%lu (sz=%zu), off=%lu (sz=%zu), namelen=%lu (sz=%zu)",
			entlen_padded,
			remaining,
			inode,
			sizeof(dirent->ino),
			curr_entry->cookie,
			sizeof(dirent->off),
			namelen,
			sizeof(dirent->namelen));
		return 0;
	}

	dirent->ino = inode;
	dirent->off = curr_entry->cookie;
	dirent->namelen = namelen;
	dirent->type = (nfs_fsdev_get_file_mode(&curr_entry->name_attributes.post_op_attr_u.attributes) & 0170000) >>
		       12;
	memcpy(dirent->name, curr_entry->name, namelen);
	memset(dirent->name + namelen, 0, entlen_padded - entlen);
	return entlen_padded;
}

static void nfs_fsdev_readdir_cb_common(struct rpc_context *rpc, int status, void *data, void *private_data, bool isPlus)
{
	struct async_context *ctx = private_data;
	pthread_mutex_lock(&ctx->fsdev_global->lock);

	struct fuse_read_in *read_in = (struct fuse_read_in *)(ctx->cmnd_in_hdr);
	struct fuse_dirent *dirent = isPlus ? NULL : (struct fuse_dirent *)(ctx->dataout);
	struct fuse_entry_out *ent = isPlus ? (struct fuse_entry_out *)(ctx->dataout) : NULL;

	size_t total_len = 0;

	if (status == RPC_STATUS_ERROR) {
		DOCA_LOG_ERR("NFS readdir failed with error: %s", (char *)data);
		exit(1);
	} else if (status == RPC_STATUS_CANCEL) {
		DOCA_LOG_ERR("NFS readdir operation was cancelled");
		exit(1);
	}

	struct READDIRPLUS3res *res = data;
	dirlistplus3 list_head = res->READDIRPLUS3res_u.resok.reply;
	entryplus3 *curr_entry = list_head.entries;

	while (curr_entry != NULL) {
		struct persistent_nfs_fh3 fh = {0};
		fh.data.data_len = curr_entry->name_handle.post_op_fh3_u.handle.data.data_len;
		memcpy(fh.data.data_val, curr_entry->name_handle.post_op_fh3_u.handle.data.data_val, fh.data.data_len);

		unsigned long inode = 0;

		if (curr_entry->name_handle.post_op_fh3_u.handle.data.data_len == 0) {
			curr_entry = curr_entry->nextentry;
			continue;
		}

		if (check_if_exist_by_right(ctx->fsdev_global->db, &fh)) {
			struct NfsFsdevEntry temp = get_entry_by_right(ctx->fsdev_global->db, &fh);
			if (temp.state == NFS_FSDEV_PENDING_DELETION_STATE) {
				curr_entry = curr_entry->nextentry;
				continue;
			}

			inode = temp.inode_left_key;
		} else {
			inode = generate_left_key(ctx->fsdev_global->db);
			if (!nfs_fsdev_db_insert(ctx->fsdev_global->db,
						 NFS_FSDEV_REGULAR_STATE,
						 0,
						 inode,
						 &curr_entry->name_handle.post_op_fh3_u.handle)) {
				DOCA_LOG_ERR("Failed to insert new entry to our data base");
				exit(1);
			}
		}

		if (isPlus) {
			if (sizeof(struct fuse_entry_out) > read_in->size - total_len) {
				break;
			}

			total_len += nfs_fsdev_fill_lookup_entry(ent,
								 &curr_entry->name_attributes.post_op_attr_u.attributes,
								 inode);
			dirent = (struct fuse_dirent *)(ent + 1);

			if ((char *)dirent > (char *)(ctx->dataout + 1) + read_in->size) {
				break;
			}
		}

		size_t direntry_len =
			nfs_fsdev_fill_fuse_dirent(dirent,
						   curr_entry,
						   inode,
						   read_in->size - total_len,
						   isPlus ? FUSE_NAME_OFFSET_DIRENTPLUS : FUSE_NAME_OFFSET);

		if (isPlus) {
			ent = (struct fuse_entry_out *)(((char *)dirent) + direntry_len);
			if ((char *)ent > (char *)(ctx->dataout + 1) + read_in->size)
				break;
		} else {
			dirent = (struct fuse_dirent *)(((char *)dirent) + direntry_len);
			if ((char *)dirent > (char *)(ctx->dataout + 1) + read_in->size)
				break;
		}

		curr_entry = curr_entry->nextentry;
		total_len += (direntry_len);
	}

	nfs_fsdev_complete(ctx, total_len, 0);
}

static void nfs_fsdev_readdir_cb_regular(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	nfs_fsdev_readdir_cb_common(rpc, status, data, private_data, false);
}

static void nfs_fsdev_readdir_cb_plus(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	nfs_fsdev_readdir_cb_common(rpc, status, data, private_data, true);
}

static struct READDIRPLUS3args nfs_fsdev_readdir_args(size_t offset, struct NfsFsdevEntry *entry)
{
	struct READDIRPLUS3args args = {0};

	args.dir.data.data_len = entry->fh_right_key.data.data_len;
	args.dir.data.data_val = entry->fh_right_key.data.data_val;
	args.cookie = offset;
	args.dircount = 1000000;
	args.maxcount = 1000000;
	return args;
}

static void nfs_fsdev_readdir_common(struct async_context *context, bool isPlus)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);
	struct fuse_read_in *read_in = (struct fuse_read_in *)(context->cmnd_in_hdr);

	if (check_if_exist_by_left(context->fsdev_global->db, hdr->nodeid) == false) {
		DOCA_LOG_ERR("Directory inode=%lu does not exist in DB", hdr->nodeid);
		exit(1);
	}

	struct NfsFsdevEntry temp = get_entry_by_left(context->fsdev_global->db, hdr->nodeid);
	if (temp.state == NFS_FSDEV_PENDING_DELETION_STATE) {
		DOCA_LOG_ERR("Attempt to read directory inode=%lu which is pending deletion", hdr->nodeid);
		exit(1);
	}

	struct READDIRPLUS3args args = nfs_fsdev_readdir_args(read_in->offset, &temp);

	pthread_mutex_unlock(&context->fsdev_global->lock);
	if (rpc_nfs3_readdirplus_async(nfs_get_rpc_context(context->fsdev_perthread->nfs),
				       isPlus ? nfs_fsdev_readdir_cb_plus : nfs_fsdev_readdir_cb_regular,
				       &args,
				       context)) {
		DOCA_LOG_ERR("Failed to start readdir");
		exit(1);
	}
}

static void nfs_fsdev_readdir(struct async_context *context)
{
	nfs_fsdev_readdir_common(context, false);
}

static void nfs_fsdev_readdir_plus(struct async_context *context)
{
	nfs_fsdev_readdir_common(context, true);
}

struct mknod_cb_data {
	struct async_context *ctx;
	char *name;
};

static void nfs_fsdev_mknod_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	struct mknod_cb_data *cb_data = private_data;
	struct async_context *ctx = cb_data->ctx;
	pthread_mutex_lock(&ctx->fsdev_global->lock);

	free(cb_data->name);
	free(cb_data);
	if (status == RPC_STATUS_ERROR) {
		DOCA_LOG_ERR("Failed to create file: %s", (char *)data);
		exit(1);
	} else if (status == RPC_STATUS_CANCEL) {
		DOCA_LOG_ERR("Failed MKNOD with status of RPC_STATUS_CANCEL");
		exit(1);
	}
	struct CREATE3res *result = data;
	if (result->status != NFS3_OK) {
		DOCA_LOG_ERR("Create returned error [%d]", result->status);
		exit(1);
	}

	if (result->CREATE3res_u.resok.obj.post_op_fh3_u.handle.data.data_len > NFS_FSDEV_MAX_FH_DATA_LEN) {
		DOCA_LOG_ERR("File handle returned in mknod too long");
		exit(1);
	}

	struct persistent_nfs_fh3 fh = {0};
	fh.data.data_len = result->CREATE3res_u.resok.obj.post_op_fh3_u.handle.data.data_len;
	memcpy(fh.data.data_val, result->CREATE3res_u.resok.obj.post_op_fh3_u.handle.data.data_val, fh.data.data_len);

	if (check_if_exist_by_right(ctx->fsdev_global->db, &fh)) {
		struct NfsFsdevEntry real_entry = get_entry_by_right(ctx->fsdev_global->db, &fh);

		if (real_entry.state == NFS_FSDEV_PENDING_DELETION_STATE) {
			DOCA_LOG_ERR("Edge case - NFS target reused this file handle");
			exit(1);
		} else {
			fattr3 *res = &result->CREATE3res_u.resok.obj_attributes.post_op_attr_u.attributes;
			struct fuse_out_header *hdr_out = (struct fuse_out_header *)ctx->fuse_out;
			struct fuse_entry_out *entry_out = (struct fuse_entry_out *)(hdr_out + 1);

			nfs_fsdev_fill_lookup_entry(entry_out, res, real_entry.inode_left_key);

			goto COMPLETE;
		}
	}

	unsigned long new_inode = generate_left_key(ctx->fsdev_global->db);
	if (!nfs_fsdev_db_insert(ctx->fsdev_global->db,
				 NFS_FSDEV_REGULAR_STATE,
				 0,
				 new_inode,
				 &result->CREATE3res_u.resok.obj.post_op_fh3_u.handle)) {
		DOCA_LOG_ERR("Failed to insert new entry to our data base");
		exit(1);
	}

	fattr3 *res = &result->CREATE3res_u.resok.obj_attributes.post_op_attr_u.attributes;
	struct fuse_out_header *hdr_out = (struct fuse_out_header *)ctx->fuse_out;
	struct fuse_entry_out *entry_out = (struct fuse_entry_out *)(hdr_out + 1);
	nfs_fsdev_fill_lookup_entry(entry_out, res, new_inode);

COMPLETE:
	nfs_fsdev_complete(ctx, sizeof(struct fuse_entry_out), 0);
}

static struct CREATE3args nfs_fsdev_mknod_args(struct fuse_in_header *hdr_in,
					       struct fuse_mknod_in *mknod_in,
					       char *name,
					       struct NfsFsdevEntry *entry)
{
	/*
	 * Better safe than sorry Or GUARDED, or EXCLUSIVE (UNCHECKED mode
	 * creates the file regardless of whether it
	 * exists. GUARDED fails if the file exists.
	 * EXCLUSIVE is for atomic file creation.)
	 */
	struct CREATE3args args = {0};
	args.where.dir.data.data_len = entry->fh_right_key.data.data_len;
	args.where.dir.data.data_val = entry->fh_right_key.data.data_val;
	args.where.name = strdup(name);
	if (args.where.name == NULL) {
		DOCA_LOG_ERR("Failed to duplicate name");
		return args;
	}

	args.how.mode = UNCHECKED;
	args.how.createhow3_u.obj_attributes.mode.set_it = 1;
	args.how.createhow3_u.obj_attributes.mode.set_mode3_u.mode = mknod_in->mode & 0777;
	args.how.createhow3_u.obj_attributes.uid.set_it = 1;
	args.how.createhow3_u.obj_attributes.uid.set_uid3_u.uid = hdr_in->uid;
	args.how.createhow3_u.obj_attributes.gid.set_it = 1;
	args.how.createhow3_u.obj_attributes.gid.set_gid3_u.gid = hdr_in->gid;
	return args;
}

static void nfs_fsdev_mknod(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);
	struct fuse_mknod_in *mknod_in = (struct fuse_mknod_in *)(context->cmnd_in_hdr);
	char *name = (char *)context->datain;

	switch (mknod_in->mode & BITS_MASK) {
	case REGULAR_FILE:
		if (check_if_exist_by_left(context->fsdev_global->db, hdr->nodeid) == false) {
			DOCA_LOG_ERR("Attempt to create a new file in a directory that is pending deletion");
			exit(1);
		}

		struct NfsFsdevEntry temp = get_entry_by_left(context->fsdev_global->db, hdr->nodeid);
		assert(temp.state != NFS_FSDEV_PENDING_DELETION_STATE);
		struct CREATE3args args = nfs_fsdev_mknod_args(hdr, mknod_in, name, &temp);

		struct mknod_cb_data *cb_data = malloc(sizeof(*cb_data));
		if (!cb_data) {
			DOCA_LOG_ERR("Failed to allocate memory for cb_data");
			exit(1);
		}
		cb_data->ctx = context;
		cb_data->name = args.where.name; // Transfer ownership

		pthread_mutex_unlock(&context->fsdev_global->lock);
		if (rpc_nfs3_create_async(nfs_get_rpc_context(context->fsdev_perthread->nfs),
					  nfs_fsdev_mknod_cb,
					  &args,
					  cb_data)) {
			DOCA_LOG_ERR("Failed to start create");
			exit(1);
		}
		break;
	default:
		DOCA_LOG_ERR("Unexpected file type(HARD LINKS, SOFT LINKS, ETC) in mode: %o", mknod_in->mode);
		exit(1);
	}
}

static void nfs_fsdev_update_recovery(uint16_t src_domain_id,
				      uint64_t req_id,
				      unsigned long inode,
				      unsigned long expected_ref_count,
				      struct nfs_fsdev_recovery *ptr)
{
	uint32_t xid = req_id;
	int queue_number = src_domain_id;

	ptr->queues[queue_number].header_xid = xid;
	compiler_barrier();
	ptr->queues[queue_number].file_inode = inode;
	ptr->queues[queue_number].expected_ref_count = expected_ref_count;
	ptr->global_generation++;
	ptr->queues[queue_number].generation = ptr->global_generation;
	compiler_barrier();
	ptr->queues[queue_number].suffix_xid = xid;
	compiler_barrier();
}

static void nfs_fsdev_opendir(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);
	struct fuse_open_out *open_out = (struct fuse_open_out *)(context->cmnd_out_hdr);

	open_out->fh = hdr->nodeid;
	open_out->open_flags = FOPEN_DIRECT_IO;
	nfs_fsdev_complete(context, sizeof(*open_out), 0);
}

static void nfs_fsdev_open(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);
	struct fuse_open_in *open_in = (struct fuse_open_in *)(context->cmnd_in_hdr);
	struct fuse_open_out *open_out = (struct fuse_open_out *)(context->cmnd_out_hdr);

	unsigned long xid = context->req_id;
	if (xid <= context->fsdev_global->recovery->queues[context->src_domain_id].suffix_xid) {
		DOCA_LOG_WARN("Got and old I/O request");
		open_out->fh = hdr->nodeid;
		goto COMPLETE;
	}

	if (check_if_exist_by_left(context->fsdev_global->db, hdr->nodeid) == false) {
		DOCA_LOG_ERR("Attempt to open a unknown file");
		exit(1);
	}

	struct NfsFsdevEntry temp = get_entry_by_left(context->fsdev_global->db, hdr->nodeid);
	if (temp.state == NFS_FSDEV_PENDING_DELETION_STATE) {
		DOCA_LOG_ERR("Attempt to get new file descriptor for a file that is pending deletion");
		nfs_fsdev_complete(context, 0, -ENOENT);
		return;
	}

	temp.ref_count++;
	nfs_fsdev_update_recovery(context->src_domain_id,
				  context->req_id,
				  hdr->nodeid,
				  temp.ref_count,
				  context->fsdev_global->recovery);
	compiler_barrier();

	if (!update_entry_by_left(context->fsdev_global->db, &temp, temp.inode_left_key)) {
		DOCA_LOG_ERR("Failed to open I/O request - updating the data base ");
		exit(1);
	}

	/* Set FUSE response flags */
	open_out->open_flags = 0;
	if (open_in->flags & O_DIRECT) {
		open_out->open_flags |= FOPEN_DIRECT_IO;
	}

	open_out->open_flags |= FOPEN_KEEP_CACHE;
	open_out->fh = hdr->nodeid;

COMPLETE:
	nfs_fsdev_complete(context, sizeof(*open_out), 0);
}

static void nfs_fsdev_releasedir(struct async_context *context)
{
	nfs_fsdev_complete(context, 0, 0);
}

static void nfs_fsdev_delayed_unlink_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	struct async_context *context = private_data;
	pthread_mutex_lock(&context->fsdev_global->lock);
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);

	if (status == RPC_STATUS_ERROR) {
		DOCA_LOG_ERR("Unlink failed with error [%s]", (char *)data);
		exit(1);
	} else if (status == RPC_STATUS_CANCEL) {
		DOCA_LOG_ERR("Unlink failed with status of RPC_STATUS_CANCEL");
		exit(1);
	}
	if (!remove_entry_by_left(context->fsdev_global->db, hdr->nodeid)) {
		DOCA_LOG_ERR("Failed to remove entry from data base in unlink replay");
		exit(1);
	}

	nfs_fsdev_complete(context, 0, 0);
}

static void nfs_fsdev_release(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);
	unsigned long xid = context->req_id;

	if (xid <= context->fsdev_global->recovery->queues[context->src_domain_id].suffix_xid) {
		DOCA_LOG_WARN("Got and old I/O request");
		goto COMPLETE;
	}

	if (check_if_exist_by_left(context->fsdev_global->db, hdr->nodeid) == false) {
		DOCA_LOG_ERR("Error: trying to close fd that don't have an entry in data base ");
		exit(1);
	}

	struct NfsFsdevEntry temp = get_entry_by_left(context->fsdev_global->db, hdr->nodeid);
	temp.ref_count--;

	nfs_fsdev_update_recovery(context->src_domain_id,
				  context->req_id,
				  hdr->nodeid,
				  temp.ref_count,
				  context->fsdev_global->recovery);

	compiler_barrier();

	if (temp.ref_count == 0 && temp.state == NFS_FSDEV_PENDING_DELETION_STATE) {
		struct REMOVE3args args = {0};
		args.object.dir.data.data_val = temp.replay_unlink_params.parent_fh.data.data_val;
		args.object.dir.data.data_len = temp.replay_unlink_params.parent_fh.data.data_len;
		args.object.name = temp.replay_unlink_params.name;
		rpc_set_next_xid(nfs_get_rpc_context(context->fsdev_perthread->nfs),
				 nfs_fsdev_get_xid(context->src_domain_id, context->req_id) + 1);

		pthread_mutex_unlock(&context->fsdev_global->lock);
		if (rpc_nfs3_remove_async(nfs_get_rpc_context(context->fsdev_perthread->nfs),
					  nfs_fsdev_delayed_unlink_cb,
					  &args,
					  context)) {
			DOCA_LOG_ERR("Failed to unlink (replay) a file ");
			exit(1);
		}
		return;
	}

	if (!update_entry_by_left(context->fsdev_global->db, &temp, temp.inode_left_key)) {
		DOCA_LOG_ERR("Failed to release I/O request - updating the data base ");
		exit(1);
	}

COMPLETE:
	nfs_fsdev_complete(context, 0, 0);
}

static void nfs_fsdev_write_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	struct async_context *context = private_data;
	pthread_mutex_lock(&context->fsdev_global->lock);

	if (status == RPC_STATUS_ERROR) {
		DOCA_LOG_ERR("Write failed with error [%s]", (char *)data);
		exit(1);
	} else if (status == RPC_STATUS_CANCEL) {
		DOCA_LOG_ERR("Write failed with status of RPC_STATUS_CANCEL");
		exit(1);
	}

	struct fuse_write_in *write_in = (struct fuse_write_in *)(context->cmnd_in_hdr);
	struct fuse_write_out *write_out = (struct fuse_write_out *)(context->cmnd_out_hdr);

	write_out->padding = 0;
	write_out->size = write_in->size;
	nfs_fsdev_complete(context, sizeof(*write_out), 0);
}

static struct WRITE3args nfs_fsdev_write_args(struct async_context *context, struct NfsFsdevEntry *entry)
{
	struct fuse_write_in *write_in = (struct fuse_write_in *)(context->cmnd_in_hdr);
	char *buff = (char *)context->datain;

	struct WRITE3args args = {0};
	args.file.data.data_len = entry->fh_right_key.data.data_len;
	args.file.data.data_val = entry->fh_right_key.data.data_val;
	args.offset = write_in->offset;
	args.count = write_in->size;
	args.data.data_val = buff;
	args.data.data_len = write_in->size;
	args.stable = FILE_SYNC;
	/* The choice of stability affects the trade-off between performance and
	   data safety: UNSTABLE is fastest but least safe FILE_SYNC is slowest
	   but most safe DATA_SYNC is a middle ground
	*/
	return args;
}

static void nfs_fsdev_write(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);

	if (check_if_exist_by_left(context->fsdev_global->db, hdr->nodeid) == false) {
		DOCA_LOG_ERR("trying to write to a none existing file");
		exit(1);
	}

	struct NfsFsdevEntry temp = get_entry_by_left(context->fsdev_global->db, hdr->nodeid);
	if (temp.state == NFS_FSDEV_PENDING_DELETION_STATE) {
		DOCA_LOG_WARN("Trying to make I/O request on inode that is pending deletion");
		if (temp.ref_count == 0) {
			exit(1);
		}
	}

	struct WRITE3args args = nfs_fsdev_write_args(context, &temp);

	pthread_mutex_unlock(&context->fsdev_global->lock);
	if (rpc_nfs3_write_async(nfs_get_rpc_context(context->fsdev_perthread->nfs),
				 nfs_fsdev_write_cb,
				 &args,
				 context)) {
		DOCA_LOG_ERR("Error: in write opertion");
		exit(1);
	}
}

static void nfs_fsdev_read_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	struct async_context *context = private_data;
	pthread_mutex_lock(&context->fsdev_global->lock);

	if (status == RPC_STATUS_ERROR) {
		DOCA_LOG_ERR("Read failed with error [%s]", (char *)data);
		exit(1);
	} else if (status == RPC_STATUS_CANCEL) {
		DOCA_LOG_ERR("Read failed with status of RPC_STATUS_CANCEL");
		exit(1);
	}
	struct READ3res *result = data;

	// Check if the NFS read was successful
	if (result->status != NFS3_OK) {
		DOCA_LOG_ERR("NFS read failed with status [%d]", result->status);
		nfs_fsdev_complete(context, 0, -EIO);
		return;
	}

	// Get the actual read data from NFS response
	uint32_t bytes_read = result->READ3res_u.resok.count;
	char *nfs_data = result->READ3res_u.resok.data.data_val;

	// Get the FUSE output buffer (where we need to copy the data)
	char *fuse_buf = (char *)(context->dataout);

	// Copy the data from NFS response to FUSE output buffer
	if (bytes_read > 0 && nfs_data != NULL) {
		memcpy(fuse_buf, nfs_data, bytes_read);
	}

	nfs_fsdev_complete(context, bytes_read, 0);
}

static struct READ3args nfs_fsdev_read_args(struct async_context *context, struct NfsFsdevEntry *entry)
{
	struct fuse_read_in *read_in = (struct fuse_read_in *)(context->cmnd_in_hdr);
	struct READ3args args = {0};

	args.file.data.data_len = entry->fh_right_key.data.data_len;
	args.file.data.data_val = entry->fh_right_key.data.data_val;
	args.offset = read_in->offset;
	args.count = read_in->size;
	return args;
}

static void nfs_fsdev_read(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);

	if (check_if_exist_by_left(context->fsdev_global->db, hdr->nodeid) == false) {
		DOCA_LOG_ERR("Attempt to read a none existing file");
		exit(1);
	}

	struct NfsFsdevEntry temp = get_entry_by_left(context->fsdev_global->db, hdr->nodeid);
	if (temp.state == NFS_FSDEV_PENDING_DELETION_STATE) {
		DOCA_LOG_WARN("Attempt to make I/O request on inode that is pending deletion");
		if (temp.ref_count == 0) {
			exit(1);
		}
	}

	struct READ3args args = nfs_fsdev_read_args(context, &temp);
	pthread_mutex_unlock(&context->fsdev_global->lock);
	if (rpc_nfs3_read_async(nfs_get_rpc_context(context->fsdev_perthread->nfs), nfs_fsdev_read_cb, &args, context)) {
		DOCA_LOG_ERR("Falied to start read request ");
		exit(1);
	}
}

static struct unlink_cb_data *allocate_and_initialize_unlink_cb_data(struct async_context *context)
{
	struct unlink_cb_data *ret = calloc(1, sizeof(struct unlink_cb_data));
	if (ret == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for unlink callback data");
		return NULL;
	}

	ret->context = context;
	ret->inode_for_deletion = INVALID_INODE;
	return ret;
}

static void nfs_fsdev_unlink_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	struct unlink_cb_data *cb_data = private_data;
	pthread_mutex_lock(&cb_data->context->fsdev_global->lock);

	if (status == RPC_STATUS_ERROR) {
		DOCA_LOG_ERR("Unlink failed with error [%s]", (char *)data);
		free(cb_data->context);
		free(cb_data);
		exit(1);
	} else if (status == RPC_STATUS_CANCEL) {
		DOCA_LOG_ERR("Unlink failed with status of RPC_STATUS_CANCEL");
		free(cb_data->context);
		free(cb_data);
		exit(1);
	}

	if (!remove_entry_by_left(cb_data->context->fsdev_global->db, cb_data->inode_for_deletion)) {
		DOCA_LOG_ERR("Failed to remove entry from data base");
		free(cb_data->context);
		free(cb_data);
		exit(1);
	}

	nfs_fsdev_complete(cb_data->context, 0, 0);
	free(cb_data);
}

static void nfs_fsdev_unlink_dummy_lookup_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	struct unlink_cb_data *cb_data = private_data;
	pthread_mutex_lock(&cb_data->context->fsdev_global->lock);

	struct fuse_in_header *hdr = (struct fuse_in_header *)(cb_data->context->fuse_header);
	char *filename = (char *)(cb_data->context->datain);

	if (status == RPC_STATUS_ERROR) {
		DOCA_LOG_ERR("LOOKUP FROM UNLINK failed with error [%s]", (char *)data);
		free(cb_data->context);
		free(cb_data);
		exit(1);
	} else if (status == RPC_STATUS_CANCEL) {
		DOCA_LOG_ERR("LOOKUP FROM UNLINK failed with status of RPC_STATUS_CANCEL");
		free(cb_data->context);
		free(cb_data);
		exit(1);
	}

	struct LOOKUP3res *result = data;
	nfsstat3 ret = result->status;
	if (ret != NFS3_OK) {
		if (ret == NFS3ERR_NOENT) {
			DOCA_LOG_WARN("lookup result is NFS3ERR_NOENT - we assume this is an UNLIK replay and succeed");
			nfs_fsdev_complete(cb_data->context, 0, 0);
			free(cb_data);
			return;
		}

		DOCA_LOG_ERR("Lookup result is other than OK or NOENT = [%d]", ret);
		free(cb_data->context);
		free(cb_data);
		exit(1);
	}

	struct nfs_fh3 *fh = &(result->LOOKUP3res_u.resok.object);

	struct persistent_nfs_fh3 pfh = {0};
	pfh.data.data_len = fh->data.data_len;
	memcpy(pfh.data.data_val, fh->data.data_val, pfh.data.data_len);

	unsigned long ref_count = 0;
	struct NfsFsdevEntry entry = {0};
	if (check_if_exist_by_right(cb_data->context->fsdev_global->db, &pfh)) {
		entry = get_entry_by_right(cb_data->context->fsdev_global->db, &pfh);
		entry.state = NFS_FSDEV_PENDING_DELETION_STATE;
		fflush(stdout);

		if (!update_entry_by_left(cb_data->context->fsdev_global->db, &entry, entry.inode_left_key)) {
			DOCA_LOG_ERR("Failed to update entry");
			free(cb_data->context);
			free(cb_data);
			exit(1);
		}
		cb_data->inode_for_deletion = entry.inode_left_key;
		ref_count = entry.ref_count;
	} else {
		unsigned long new_inode = generate_left_key(cb_data->context->fsdev_global->db);

		if (nfs_fsdev_db_insert(cb_data->context->fsdev_global->db,
					NFS_FSDEV_PENDING_DELETION_STATE,
					0,
					new_inode,
					fh) == false) {
			DOCA_LOG_ERR("Failed to insert to the map.");
			free(cb_data->context);
			free(cb_data);
			exit(1);
		}

		DOCA_LOG_INFO("Unlink dummy lookup complete ");

		cb_data->inode_for_deletion = new_inode;
	}

	if (ref_count != 0) {
		DOCA_LOG_WARN("Attempt to UNLINK a file that has positive refrence count");

		strcpy(entry.replay_unlink_params.name, filename);

		struct NfsFsdevEntry parent_entry = get_entry_by_left(cb_data->context->fsdev_global->db, hdr->nodeid);

		entry.replay_unlink_params.parent_fh.data.data_len = parent_entry.fh_right_key.data.data_len;
		memcpy(entry.replay_unlink_params.parent_fh.data.data_val,
		       parent_entry.fh_right_key.data.data_val,
		       entry.replay_unlink_params.parent_fh.data.data_len);

		if (!update_entry_by_left(cb_data->context->fsdev_global->db, &entry, entry.inode_left_key)) {
			DOCA_LOG_ERR("Failed to update entry");
			free(cb_data);
			exit(1);
		}

		nfs_fsdev_complete(cb_data->context, 0, 0);
		free(cb_data);
		return;
	}

	if (check_if_exist_by_left(cb_data->context->fsdev_global->db, hdr->nodeid) == false) {
		DOCA_LOG_ERR("Parent entry in the map removed.");
		free(cb_data->context);
		free(cb_data);
		exit(1);
	}

	struct NfsFsdevEntry parent_temp_entry = get_entry_by_left(cb_data->context->fsdev_global->db, hdr->nodeid);
	struct REMOVE3args args = {0};
	args.object.dir.data.data_len = parent_temp_entry.fh_right_key.data.data_len;
	args.object.dir.data.data_val = parent_temp_entry.fh_right_key.data.data_val;
	args.object.name = filename;

	rpc_set_next_xid(nfs_get_rpc_context(cb_data->context->fsdev_perthread->nfs),
			 nfs_fsdev_get_xid(cb_data->context->src_domain_id, cb_data->context->req_id) + 1);
	pthread_mutex_unlock(&cb_data->context->fsdev_global->lock);
	if (rpc_nfs3_remove_async(nfs_get_rpc_context(cb_data->context->fsdev_perthread->nfs),
				  nfs_fsdev_unlink_cb,
				  &args,
				  cb_data)) {
		DOCA_LOG_ERR("Failed to unlink a file ");
		free(cb_data->context);
		free(cb_data);
		exit(1);
	}
}

static void nfs_fsdev_unlink(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);
	char *filename = (char *)(context->datain);

	if (hdr->nodeid == 0) {
		DOCA_LOG_ERR("Attempt to call LOOKUP with parent inode = 0");
		exit(1);
	}

	if (check_if_exist_by_left(context->fsdev_global->db, hdr->nodeid) == false) {
		DOCA_LOG_ERR("Attempt to delete a file that his parent is not in the map");
		exit(1);
	}

	if (strlen(filename) + 1 > NFS_FSDEV_MAX_FILE_NAME) {
		DOCA_LOG_ERR("File name is too long = [%s]", filename);
		exit(1);
	}

	struct NfsFsdevEntry parent = get_entry_by_left(context->fsdev_global->db, hdr->nodeid);

	if (parent.state == NFS_FSDEV_PENDING_DELETION_STATE) {
		DOCA_LOG_ERR("Attempt to delete a file that his parent is already pending deletion");
		exit(1);
	}

	struct LOOKUP3args args = {0};
	args.what.dir.data.data_val = parent.fh_right_key.data.data_val;
	args.what.dir.data.data_len = parent.fh_right_key.data.data_len;
	args.what.name = filename;

	struct unlink_cb_data *cb_data = allocate_and_initialize_unlink_cb_data(context);
	pthread_mutex_unlock(&context->fsdev_global->lock);
	if (rpc_nfs3_lookup_async(nfs_get_rpc_context(context->fsdev_perthread->nfs),
				  nfs_fsdev_unlink_dummy_lookup_cb,
				  &args,
				  cb_data)) {
		DOCA_LOG_ERR("Failed to call lookup from UNLINK function");
		exit(1);
	}
}

static void nfs_fsdev_mkdir_cb(struct rpc_context *rpc, int status, void *data, void *private_data)
{
	struct async_context *context = private_data;
	pthread_mutex_lock(&context->fsdev_global->lock);

	struct fuse_out_header *hdr_out = (struct fuse_out_header *)context->fuse_out;
	struct fuse_entry_out *entry_out = (struct fuse_entry_out *)(hdr_out + 1);

	if (status == RPC_STATUS_ERROR) {
		DOCA_LOG_ERR("NFS mkdir failed with error [%s]", (char *)data);
		exit(1);
	} else if (status == RPC_STATUS_CANCEL) {
		DOCA_LOG_ERR("NFS mkdir failed with status of RPC_STATUS_CANCEL");
		exit(1);
	}

	struct MKDIR3res *result = data;
	if (result->status != NFS3_OK) {
		DOCA_LOG_ERR("Problem in mkdir error code =[%d]", result->status);
		exit(1);
	}

	if (result->MKDIR3res_u.resok.obj.post_op_fh3_u.handle.data.data_len > NFS_FSDEV_MAX_FH_DATA_LEN) {
		DOCA_LOG_ERR("File handle returned in mkdir is too long");
		exit(1);
	}

	struct persistent_nfs_fh3 fh = {0};
	fh.data.data_len = result->MKDIR3res_u.resok.obj.post_op_fh3_u.handle.data.data_len;
	memcpy(fh.data.data_val, result->MKDIR3res_u.resok.obj.post_op_fh3_u.handle.data.data_val, fh.data.data_len);

	if (check_if_exist_by_right(context->fsdev_global->db, &fh)) {
		// the case that is pending deletion is also here.
		DOCA_LOG_ERR("Attempt to create a directory that already exist ");
		exit(1);
	}

	unsigned long new_inode = generate_left_key(context->fsdev_global->db);

	if (!nfs_fsdev_db_insert(context->fsdev_global->db,
				 NFS_FSDEV_REGULAR_STATE,
				 0,
				 new_inode,
				 &result->MKDIR3res_u.resok.obj.post_op_fh3_u.handle)) {
		DOCA_LOG_ERR("Failed to insert into data base new entry for a new directory ");
		exit(1);
	}

	fattr3 *res = &result->MKDIR3res_u.resok.obj_attributes.post_op_attr_u.attributes;
	size_t size_temp = nfs_fsdev_fill_lookup_entry(entry_out, res, new_inode);
	nfs_fsdev_complete(context, size_temp, 0);
}

static struct MKDIR3args nfs_fsdev_mkdir_args(struct async_context *context, struct NfsFsdevEntry *temp)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);
	struct fuse_mkdir_in *hdr_in = (struct fuse_mkdir_in *)(context->cmnd_in_hdr);
	char *dirname = (char *)(hdr_in + 1);
	struct MKDIR3args args = {0};

	args.where.dir.data.data_len = temp->fh_right_key.data.data_len;
	args.where.dir.data.data_val = temp->fh_right_key.data.data_val;
	args.where.name = dirname;
	args.attributes.gid.set_it = 1;
	args.attributes.gid.set_gid3_u.gid = hdr->gid;
	args.attributes.uid.set_it = 1;
	args.attributes.uid.set_uid3_u.uid = hdr->uid;
	args.attributes.mode.set_it = 1;
	args.attributes.mode.set_mode3_u.mode = hdr_in->mode & 0777;
	return args;
}

static void nfs_fsdev_mkdir(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)(context->fuse_header);

	if (!check_if_exist_by_left(context->fsdev_global->db, hdr->nodeid)) {
		DOCA_LOG_ERR("Attempt to create a directory in a not known parent directory");
		exit(1);
	}
	struct NfsFsdevEntry temp = get_entry_by_left(context->fsdev_global->db, hdr->nodeid);
	if (temp.state == NFS_FSDEV_PENDING_DELETION_STATE) {
		DOCA_LOG_WARN("Attempt to make I/O request on inode that is pending deletion");
		exit(1);
	}

	struct MKDIR3args args = nfs_fsdev_mkdir_args(context, &temp);
	pthread_mutex_unlock(&context->fsdev_global->lock);
	if (rpc_nfs3_mkdir_async(nfs_get_rpc_context(context->fsdev_perthread->nfs),
				 nfs_fsdev_mkdir_cb,
				 &args,
				 context)) {
		DOCA_LOG_ERR("Failed to call mkdir");
		exit(1);
	}
}

static void nfs_fsdev_forget(struct async_context *context)
{
	nfs_fsdev_complete(context, 0, 0);
}

static const struct {
	void (*func)(struct async_context *context);
	const char *name;
} fuse_ll_ops[] = {
	[FUSE_LOOKUP] = {nfs_fsdev_lookup, "LOOKUP"},
	[FUSE_FORGET] = {nfs_fsdev_forget, "FORGET"},
	[FUSE_GETATTR] = {nfs_fsdev_getattr, "GETATTR"},
	[FUSE_SETATTR] = {nfs_fsdev_setattr, "SETATTR"},
	[FUSE_READLINK] = {nimp, "READLINK"},
	[FUSE_SYMLINK] = {nimp, "SYMLINK"},
	[FUSE_MKNOD] = {nfs_fsdev_mknod, "MKNOD"},
	[FUSE_MKDIR] = {nfs_fsdev_mkdir, "MKDIR"},
	[FUSE_UNLINK] = {nfs_fsdev_unlink, "UNLINK"},
	[FUSE_RMDIR] = {nimp, "RMDIR"},
	[FUSE_RENAME] = {nimp, "RENAME"},
	[FUSE_LINK] = {nimp, "LINK"},
	[FUSE_OPEN] = {nfs_fsdev_open, "OPEN"},
	[FUSE_READ] = {nfs_fsdev_read, "READ"},
	[FUSE_WRITE] = {nfs_fsdev_write, "WRITE"},
	[FUSE_STATFS] = {nimp, "STATFS"},
	[FUSE_RELEASE] = {nfs_fsdev_release, "RELEASE"},
	[FUSE_FSYNC] = {nimp, "FSYNC"},
	[FUSE_SETXATTR] = {nimp, "SETXATTR"},
	[FUSE_GETXATTR] = {nimp, "GETXATTR"},
	[FUSE_LISTXATTR] = {nimp, "LISTXATTR"},
	[FUSE_REMOVEXATTR] = {nimp, "REMOVEXATTR"},
	[FUSE_FLUSH] = {nimp, "FLUSH"},
	[FUSE_INIT] = {nfs_fsdev_init, "INIT"},
	[FUSE_OPENDIR] = {nfs_fsdev_opendir, "OPENDIR"},
	[FUSE_READDIR] = {nfs_fsdev_readdir, "READDIR"},
	[FUSE_RELEASEDIR] = {nfs_fsdev_releasedir, "RELEASEDIR"},
	[FUSE_FSYNCDIR] = {nimp, "FSYNCDIR"},
	[FUSE_GETLK] = {nimp, "GETLK"},
	[FUSE_SETLK] = {nimp, "SETLK"},
	[FUSE_SETLKW] = {nimp, "SETLKW"},
	[FUSE_ACCESS] = {nimp, "ACCESS"},
	[FUSE_CREATE] = {nimp, "CREATE"},
	[FUSE_INTERRUPT] = {nimp, "INTERRUPT"},
	[FUSE_BMAP] = {nimp, "BMAP"},
	[FUSE_IOCTL] = {nimp, "IOCTL"},
	[FUSE_POLL] = {nimp, "POLL"},
	[FUSE_FALLOCATE] = {nimp, "FALLOCATE"},
	[FUSE_DESTROY] = {nimp, "DESTROY"},
	[FUSE_NOTIFY_REPLY] = {NULL, "NOTIFY_REPLY"},
	[FUSE_BATCH_FORGET] = {nimp, "BATCH_FORGET"},
	[FUSE_READDIRPLUS] = {nfs_fsdev_readdir_plus, "READDIRPLUS"},
	[FUSE_RENAME2] = {nimp, "RENAME2"},
	[FUSE_COPY_FILE_RANGE] = {nimp, "COPY_FILE_RANGE"},
	[FUSE_SETUPMAPPING] = {nimp, "SETUPMAPPING"},
	[FUSE_REMOVEMAPPING] = {nimp, "REMOVEMAPPING"},
	[FUSE_SYNCFS] = {nimp, "SYNCFS"},
	[FUSE_LSEEK] = {nimp, "LSEEK"},
#ifdef FUSE_TMPFILE
	[FUSE_TMPFILE] = {nimp, "TMPFILE"},
#endif
#ifdef FUSE_STATX
	[FUSE_STATX] = {nimp, "STATX"},
#endif
};

static void nimp(struct async_context *context)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)context->fuse_header;

	DOCA_LOG_WARN("UNSUPPORTED FUSE COMMAND: Handler not implemented for opcode=%s", fuse_ll_ops[hdr->opcode].name);
	nfs_fsdev_complete(context, 0, -ENOSYS);
}

void nfs_fsdev_submit(struct nfs_fsdev *fsdev_priv,
		      char *fuse_header,
		      char *cmnd_in_hdr,
		      char *datain,
		      char *fuse_out,
		      char *cmnd_out_hdr,
		      char *dataout,
		      uint16_t src_domain_id,
		      uint64_t req_id,
		      nfs_fsdev_io_cb app_cb,
		      void *app_ctxt)
{
	struct fuse_in_header *hdr = (struct fuse_in_header *)fuse_header;
	int op = hdr->opcode;

	fflush(stdout);
	rpc_set_next_xid(nfs_get_rpc_context(fsdev_priv->nfs), nfs_fsdev_get_xid(src_domain_id, req_id));
	pthread_mutex_lock(&(fsdev_priv->nfs_global->lock));
	if ((op == FUSE_OPEN || op == FUSE_RELEASE) && (src_domain_id == 0)) {
		DOCA_LOG_ERR("Got [%s] request on priority queue", fuse_ll_ops[op].name);
		exit(1);
	}

	fuse_ll_ops[op].func(alloc_init_async_context(fsdev_priv->nfs_global,
						      fsdev_priv,
						      fuse_header,
						      cmnd_in_hdr,
						      datain,
						      fuse_out,
						      cmnd_out_hdr,
						      dataout,
						      app_cb,
						      app_ctxt,
						      src_domain_id,
						      req_id));
}

void nfs_fsdev_progress(struct nfs_fsdev *fsdev_priv)
{
	struct pollfd pfds[2];
	pfds[0].fd = nfs_get_fd(fsdev_priv->nfs);
	pfds[0].events = nfs_which_events(fsdev_priv->nfs);

	if (poll(&(pfds[0]), 1, 0) < 0) {
		DOCA_LOG_ERR("Failed to poll for NFS service");
	}

	if (nfs_service(fsdev_priv->nfs, pfds[0].revents) < 0) {
		DOCA_LOG_ERR("Failed to service NFS");
	}
}

static void nfs_fsdev_complete(struct async_context *context, size_t len, int status)
{
	struct fuse_in_header *hdr_in = (struct fuse_in_header *)(context->fuse_header);
	struct fuse_out_header *hdr_out = (struct fuse_out_header *)(context->fuse_out);

	if (hdr_out) {
		hdr_out->len = len + sizeof(*hdr_out);
		hdr_out->error = status;
		hdr_out->unique = hdr_in->unique;
	}

	pthread_mutex_unlock(&(context->fsdev_global->lock));
	context->app_cb(context->app_ctx, status);
	SLIST_INSERT_HEAD(&context->fsdev_perthread->free_ios, context, entry);
}

struct nfs_fsdev *nfs_fsdev_get(void *fsdev)
{
	return get_private_context_nfs_fsdev(fsdev);
}

void *nfs_fsdev_create(char *server, char *mount_point)
{
	return allocate_and_init_nfs_fsdev_context(server, mount_point);
}

void priv_nfs_fsdev_init(struct nfs_fsdev *fsdev, struct nfs_fsdev_global *global)
{
	struct async_context *ios;

	SLIST_INIT(&fsdev->free_ios);
	ios = (struct async_context *)calloc(NFS_FSDEV_IO_POOL_SIZE, sizeof(*ios));
	if (ios == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for %d async_context objects", NFS_FSDEV_IO_POOL_SIZE);
		exit(1);
	}

	for (int i = 0; i < NFS_FSDEV_IO_POOL_SIZE; i++)
		SLIST_INSERT_HEAD(&fsdev->free_ios, &ios[i], entry);

	pthread_mutex_lock(&(global->lock));

	fsdev->nfs = nfs_init_context();
	// For debug mode use: (NFS_FSDEV_DEBUG_RPC | NFS_FSDEV_DEBUG_NFS | NFS_FSDEV_DEBUG_STATE)
	if (fsdev->nfs == NULL) {
		DOCA_LOG_ERR("Failed to initialize NFS context");
		exit(10);
	}

	nfs_set_debug(fsdev->nfs, NFS_FSDEV_DEBUG_NONE);

	int ret = nfs_mount(fsdev->nfs, global->server, global->mount_point);
	if (ret != 0) {
		DOCA_LOG_ERR("Failed to mount NFS server '%s' export '%s'", global->server, global->mount_point);
		exit(10);
	}

	struct nfsfh *root_fh = NULL;
	ret = nfs_open(fsdev->nfs, "/", O_RDONLY, &root_fh);
	if (ret != 0) {
		DOCA_LOG_ERR("Failed to open root file handle on NFS server");
		exit(10);
	}

	if (!check_if_exist_by_left(global->db, 1)) {
		if (!nfs_fsdev_db_init_new_root(1, nfs_get_fh(root_fh), global->db)) {
			DOCA_LOG_ERR("Failed to insert root file handle into DB");
			exit(-1);
		}
	} else {
		DOCA_LOG_WARN("Root file handle already exists in DB, restoring database");
	}

	pthread_mutex_unlock(&(global->lock));
}
