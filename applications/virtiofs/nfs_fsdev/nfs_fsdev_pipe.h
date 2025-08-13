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

#ifndef NFS_FSDEV_PIPE_H
#define NFS_FSDEV_PIPE_H

#include <stdbool.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length for file handle data */
#define NFS_FSDEV_MAX_FH_DATA_LEN 300
#define NFS_FSDEV_MAX_FILE_NAME 300

/**
 * @brief Persistent NFS file handle structure.
 */
struct persistent_nfs_fh3 {
	struct {
		char data_val[NFS_FSDEV_MAX_FH_DATA_LEN]; /* Data value */
		int data_len;				  /* Data length */
	} data;						  /* Data */
};							  /* Persistent NFS file handle structure */

/**
 * @brief Entry for the NFS fsdev persistent map.
 */
struct NfsFsdevEntry {
	unsigned long inode_left_key;		/* Left key for inode */
	unsigned long ref_count;		/* Reference count */
	struct persistent_nfs_fh3 fh_right_key; /* Right key for fh */
	int state;				/* State */
	struct {
		struct persistent_nfs_fh3 parent_fh; /* Parent fh */
		char name[NFS_FSDEV_MAX_FILE_NAME];  /* Name */
	} replay_unlink_params;			     /* Replay unlink params */
};

/* Function declarations for persistent map management. */
/**
 * @brief Allocate and initialize the persistent map.
 * @param [in] filename Path to the persistent map file.
 * @return Pointer to the map, or NULL on failure.
 */
void *allocate_and_init_map(const char *filename);

/**
 * @brief Insert an entry into the persistent map.
 * @param [in] data_base Pointer to the persistent map.
 * @param [in] entry Pointer to the entry to insert.
 * @param [in] left Left key for the entry.
 * @param [in] right Right key for the entry.
 * @return True if the entry was inserted successfully, false otherwise.
 */
bool insert_entry(void *data_base, struct NfsFsdevEntry *entry, unsigned long left, struct persistent_nfs_fh3 *right);

/**
 * @brief Update an entry in the persistent map by left key.
 * @param [in] data_base Pointer to the persistent map.
 * @param [in] entry Pointer to the entry to update.
 * @param [in] left Left key for the entry.
 * @return True if the entry was updated successfully, false otherwise.
 */
bool update_entry_by_left(void *data_base, struct NfsFsdevEntry *entry, unsigned long left);

/**
 * @brief Update an entry in the persistent map by right key.
 * @param [in] data_base Pointer to the persistent map.
 * @param [in] entry Pointer to the entry to update.
 * @param [in] right Right key for the entry.
 * @return True if the entry was updated successfully, false otherwise.
 */
bool update_entry_by_right(void *data_base, struct NfsFsdevEntry *entry, struct persistent_nfs_fh3 *right);

/**
 * @brief Remove an entry from the persistent map by left key.
 * @param [in] data_base Pointer to the persistent map.
 * @param [in] left Left key for the entry.
 * @return True if the entry was removed successfully, false otherwise.
 */
bool remove_entry_by_left(void *data_base, unsigned long left);

/**
 * @brief Remove an entry from the persistent map by right key.
 * @param [in] data_base Pointer to the persistent map.
 * @param [in] right Right key for the entry.
 * @return True if the entry was removed successfully, false otherwise.
 */
bool remove_entry_by_right(void *data_base, struct persistent_nfs_fh3 *right);

/**
 * @brief Get an entry from the persistent map by left key.
 * @param [in] data_base Pointer to the persistent map.
 * @param [in] left Left key for the entry.
 * @return The entry.
 */
struct NfsFsdevEntry get_entry_by_left(void *data_base, unsigned long left);

/**
 * @brief Get an entry from the persistent map by right key.
 * @param [in] data_base Pointer to the persistent map.
 * @param [in] right Right key for the entry.
 * @return The entry.
 */
struct NfsFsdevEntry get_entry_by_right(void *data_base, struct persistent_nfs_fh3 *right);

/**
 * @brief Generate a left key for the persistent map.
 * @param [in] data_base Pointer to the persistent map.
 * @return The left key.
 */
unsigned long generate_left_key(void *data_base);

/**
 * @brief Check if an entry exists in the persistent map by left key.
 * @param [in] data_base Pointer to the persistent map.
 * @param [in] left Left key for the entry.
 * @return True if the entry exists, false otherwise.
 */
bool check_if_exist_by_left(void *data_base, unsigned long left);

/**
 * @brief Check if an entry exists in the persistent map by right key.
 * @param [in] data_base Pointer to the persistent map.
 * @param [in] right Right key for the entry.
 * @return True if the entry exists, false otherwise.
 */
bool check_if_exist_by_right(void *data_base, struct persistent_nfs_fh3 *right);

#ifdef __cplusplus
}
#endif

#endif /* NFS_FSDEV_PIPE_H */
