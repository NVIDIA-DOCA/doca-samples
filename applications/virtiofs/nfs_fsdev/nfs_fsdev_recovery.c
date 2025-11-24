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

#include <nfsc/libnfs-raw-nfs.h>
#include <sys/mman.h>
#include "priv_nfs_fsdev.h"
#include <doca_log.h>

DOCA_LOG_REGISTER(NFS_FSDEV_RECOVERY)

static inline void nfs_fsdev_init_recovery(struct nfs_fsdev_recovery *recovery)
{
	for (int i = 0; i < NFS_FSDEV_NUM_OF_VIRTIO_QUEUES; ++i) {
		recovery->queues[i].header_xid = 1;
		recovery->queues[i].suffix_xid = 0;
		recovery->queues[i].generation = 0;
	}

	recovery->global_generation = 1;
	compiler_barrier();
	recovery->magic_number = NFS_FSDEV_RECOVERY_MAGIC_NUM;
}

static inline void nfs_fsdev_restore_recovery(struct nfs_fsdev_recovery *recovery, int fd, struct database *db)
{
	(void)fd;
	unsigned int max_gen = 0;
	unsigned int index = 0;
	for (int i = 0; i < NFS_FSDEV_NUM_OF_VIRTIO_QUEUES; ++i) {
		if (recovery->queues[i].header_xid == recovery->queues[i].suffix_xid) {
			if (max_gen < recovery->queues[i].generation) {
				index = i;
				max_gen = recovery->queues[i].generation;
			}
		}
	}

	if (max_gen != 0) {
		if (!check_if_exist_by_left(db, recovery->queues[index].file_inode)) {
			DOCA_LOG_WARN("Recovery operation not needed, entry not in data structure.");
			return;
		}

		struct NfsFsdevEntry entry = get_entry_by_left(db, recovery->queues[index].file_inode);
		entry.ref_count = recovery->queues[index].expected_ref_count;
		if (!update_entry_by_left(db, &entry, recovery->queues[index].file_inode)) {
			DOCA_LOG_ERR("Failed at replay of close/open I/O command");
			// Do not exit here; let the caller handle the error
		}
	} else {
		DOCA_LOG_WARN("Nothing to recover");
	}
}

struct nfs_fsdev_recovery *nfs_fsdev_create_recovery(const char *filename, struct database *db)
{
	int fd = open(filename, O_RDWR | O_CREAT, 0666);
	if (fd == -1) {
		DOCA_LOG_ERR("Failed to open recovery file: %s", filename);
		exit(1);
	}

	size_t full_size = sizeof(struct nfs_fsdev_recovery);
	if (ftruncate(fd, full_size) == -1) {
		DOCA_LOG_ERR("Failed to set file size");
		close(fd);
		exit(1);
	}

	struct nfs_fsdev_recovery *recovery =
		(struct nfs_fsdev_recovery *)mmap(NULL, full_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (recovery == MAP_FAILED) {
		DOCA_LOG_ERR("Failed to mmap recovery file: %s", filename);
		close(fd);
		exit(1);
	}

	if (recovery->magic_number != NFS_FSDEV_RECOVERY_MAGIC_NUM) {
		nfs_fsdev_init_recovery(recovery);
	} else {
		nfs_fsdev_restore_recovery(recovery, fd, db);
	}
	compiler_barrier();
	close(fd);
	return recovery;
}

void nfs_fsdev_destroy_recovery(struct nfs_fsdev_recovery *recovery)
{
	if (recovery == NULL)
		return;

	munmap(recovery, sizeof(struct nfs_fsdev_recovery));
}

bool nfs_fsdev_db_init_new_root(unsigned long inode, const struct nfs_fh *fh, struct database *db)
{
	struct NfsFsdevEntry entry = {0};

	entry.inode_left_key = inode;
	entry.ref_count = 0;
	entry.fh_right_key.data.data_len = fh->len;
	memcpy(entry.fh_right_key.data.data_val, fh->val, fh->len);
	entry.state = NFS_FSDEV_REGULAR_STATE;
	return insert_entry(db, &entry, inode, &entry.fh_right_key);
}

bool nfs_fsdev_db_insert(struct database *db, int state, int ref_count, int inode, struct nfs_fh3 *fh)
{
	struct NfsFsdevEntry entry = {0};

	entry.state = state;
	entry.ref_count = ref_count;
	entry.inode_left_key = inode;
	entry.fh_right_key.data.data_len = fh->data.data_len;
	memcpy(entry.fh_right_key.data.data_val, fh->data.data_val, fh->data.data_len);
	return insert_entry(db, &entry, entry.inode_left_key, &entry.fh_right_key);
}
