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

#ifndef PRIV_NFS_FSDEV_H
#define PRIV_NFS_FSDEV_H

#include "nfs_fsdev_pipe.h"
#include <sys/time.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef __cplusplus
#include <string>
extern "C" {
#endif

/** @defgroup NFS_FSDEV_Debug Debug flags for NFS operations */
/**@{*/
#define NFS_FSDEV_DEBUG_NONE 0x00  /**< No debug output. */
#define NFS_FSDEV_DEBUG_RPC 0x01   /**< Debug RPC layer. */
#define NFS_FSDEV_DEBUG_NFS 0x02   /**< Debug NFS layer. */
#define NFS_FSDEV_DEBUG_STATE 0x04 /**< Debug state changes. */
/**@}*/

/** @defgroup NFS_FSDEV_States Persistent entry states */
/**@{*/
#define NFS_FSDEV_REGULAR_STATE 1	   /**< Entry is in regular state. */
#define NFS_FSDEV_PENDING_DELETION_STATE 2 /**< Entry is pending deletion. */
/**@}*/

#define NFS_FSDEV_DB_PATH "/tmp/nfs_map/db_log.bin"		/**< Path to persistent DB file. */
#define NFS_FSDEV_RECOVERY_PATH "/tmp/nfs_map/recovery_log.bin" /**< Path to recovery log. */
/**@}*/

#define NFS_FSDEV_RECOVERY_MAGIC_NUM 0x2244387115
#define NFS_FSDEV_NUM_OF_VIRTIO_QUEUES 256
#define NFS_FSDEV_IO_POOL_SIZE 4096

/**
 * @brief NFS file handle structure.
 */
struct nfs_fh {
	int len;   /**< Length of the file handle in bytes. */
	char *val; /**< Pointer to the file handle data (binary). */
};

/**
 * @brief Callback type for NFS fsdev I/O operations.
 * @param context User context pointer.
 * @param status  Status code of the operation.
 */
typedef void (*nfs_fsdev_io_cb)(void *context, int status);

/**
 * @brief Entry for recovery log.
 */
struct nfs_fsdev_recovery_entry {
	unsigned long header_xid;	  /**< Header transaction ID. */
	unsigned long expected_ref_count; /**< Expected reference count. */
	unsigned long file_inode;	  /**< File inode number. */
	unsigned long generation;	  /**< Generation number. */
	unsigned long suffix_xid;	  /**< Suffix transaction ID. */
};

/**
 * @brief Recovery log structure.
 */
struct nfs_fsdev_recovery {
	unsigned long magic_number;						/**< Magic number for validation. */
	unsigned long global_generation;					/**< Global generation counter. */
	struct nfs_fsdev_recovery_entry queues[NFS_FSDEV_NUM_OF_VIRTIO_QUEUES]; /**< recovery entries. */
};

static inline void compiler_barrier(void)
{
	__asm volatile("" ::: "memory");
}

/**
 * @brief Asynchronous context for NFS fsdev operations.
 * @note Not thread-safe; each thread should use its own context.
 */
struct async_context {
	struct nfs_fsdev_global *fsdev_global; /**< Pointer to global context. */
	struct nfs_fsdev *fsdev_perthread;     /**< Pointer to per-thread context. */
	char *fuse_header;		       /**< Pointer to FUSE header. */
	char *cmnd_in_hdr;		       /**< Pointer to FUSE input buffer. */
	char *datain;			       /**< Pointer to FUSE input buffer. */
	char *fuse_out;			       /**< Pointer to FUSE output buffer. */
	char *cmnd_out_hdr;		       /**< Pointer to FUSE input buffer. */
	char *dataout;			       /**< Pointer to FUSE output buffer. */
	nfs_fsdev_io_cb app_cb;		       /**< Application callback. */
	void *app_ctx;			       /**< Application context. */
	SLIST_ENTRY(async_context) entry;      /**< Linked list entry for free_ios. */
	uint16_t src_domain_id;		       /**< Source domain ID. */
	uint32_t req_id;		       /**< Request ID. */
};

struct nfs_fsdev_recovery *nfs_fsdev_create_recovery(const char *filename, struct database *db);
void nfs_fsdev_destroy_recovery(struct nfs_fsdev_recovery *recovery);
void priv_nfs_fsdev_init(struct nfs_fsdev *fsdev, struct nfs_fsdev_global *global);
void priv_nfs_fsdev_deinit(struct nfs_fsdev *fsdev);
bool nfs_fsdev_db_insert(struct database *db, int state, int ref_count, int inode, struct nfs_fh3 *fh);
bool nfs_fsdev_db_init_new_root(unsigned long inode, const struct nfs_fh *fh, struct database *db);

/**
 * @brief Global context for NFS fsdev operations.
 */
struct nfs_fsdev_global {
	const char *server;		     /**< NFS server address. */
	const char *mount_point;	     /**< NFS export path. */
	const char *db_name;		     /**< Path to the persistent DB file. */
	const char *recovery_file_name;	     /**< Path to the open/close replay log. */
	struct database *db;		     /**< Opaque pointer to the DB. */
	pthread_mutex_t lock;		     /**< Mutex for synchronizing access. */
	struct nfs_fsdev_recovery *recovery; /**< Pointer to replay struct. */

#ifdef __cplusplus
	nfs_fsdev_global(const char *server, const char *mount_point)
		: server(server),
		  mount_point(mount_point),
		  db_name(NFS_FSDEV_DB_PATH),
		  recovery_file_name(NFS_FSDEV_RECOVERY_PATH)
	{
		pthread_mutexattr_t attr;

		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&lock, &attr);

		db = allocate_and_init_map(db_name);
		recovery = nfs_fsdev_create_recovery(recovery_file_name, db);
	}
	~nfs_fsdev_global()
	{
		close_map(db);
		nfs_fsdev_destroy_recovery(recovery);
	}
#endif
};

/**
 * @brief Per-thread NFS fsdev context.
 */
struct nfs_fsdev {
	struct nfs_fsdev_global *nfs_global;  /**< Pointer to global context. */
	SLIST_HEAD(, async_context) free_ios; /**< List of free async_context objects. */
	struct nfs_context *nfs;	      /**< Pointer to NFS context. */
	struct async_context *ios_pool;	      /**< Pointer to IO pool. */
#ifdef __cplusplus
	nfs_fsdev(struct nfs_fsdev_global *_global) : nfs_global(_global)
	{
		priv_nfs_fsdev_init(this, nfs_global);
	}
	nfs_fsdev()
	{
	}
#endif
};

#ifdef __cplusplus
}
#endif

#endif /* PRIV_NFS_FSDEV_H */
