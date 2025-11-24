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

#include <filesystem>
#include <string>
#include "nfs_fsdev_pipe.h"
#include "nfs_fsdev_gpm.hpp"
#include <doca_log.h>

DOCA_LOG_REGISTER(NFS_FSDEV_PIPE)

struct database {
public:
	PersistentMap<struct NfsFsdevEntry> *map;
	database(int fd_, void *shmem_, size_t full_sz)
		: map(new PersistentMap<struct NfsFsdevEntry>(shmem_)),
		  fd(fd_),
		  shmem(shmem_),
		  full_size(full_sz)
	{
	}

	~database()
	{
		delete map;
		if (shmem && shmem != MAP_FAILED) {
			munmap(shmem, full_size);
		}
		if (fd != -1) {
			close(fd);
		}
	}

private:
	int fd;
	void *shmem;
	size_t full_size;
};

extern "C" {

struct database *allocate_and_init_map(const char *filename)
{
	std::filesystem::create_directories(std::filesystem::path(filename).parent_path());

	int fd = open(filename, O_RDWR | O_CREAT, 0666);
	if (fd == -1) {
		DOCA_LOG_ERR("Error: in opening file");
		return NULL;
	}

	size_t full_size = sizeof(RawPersistentDataBase<struct NfsFsdevEntry>);
	if (ftruncate(fd, full_size) == -1) {
		DOCA_LOG_ERR("Error: setting file size");
		close(fd);
		return NULL;
	}

	void *shmem = mmap(NULL, full_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shmem == MAP_FAILED) {
		DOCA_LOG_ERR("Error: error mapping file");
		close(fd);
		return NULL;
	}

	return new database(fd, shmem, full_size);
}
void close_map(struct database *db)
{
	delete db;
}

bool insert_entry(struct database *db, struct NfsFsdevEntry *entry, unsigned long left, struct persistent_nfs_fh3 *right)
{
	std::string temp(right->data.data_val, right->data.data_len);
	return db->map->InsertEntry(*entry, left, temp);
}

bool update_entry_by_left(struct database *db, struct NfsFsdevEntry *entry, unsigned long left)
{
	return db->map->UpdateEntryByLeft(*entry, left);
}

bool update_entry_by_right(struct database *db, struct NfsFsdevEntry *entry, struct persistent_nfs_fh3 *right)
{
	std::string temp((char *)right->data.data_val, right->data.data_len);
	return db->map->UpdateEntryByRight(*entry, temp);
}

bool remove_entry_by_left(struct database *db, unsigned long left)
{
	return db->map->RemoveEntryByLeft(left);
}

bool remove_entry_by_right(struct database *db, struct persistent_nfs_fh3 *right)
{
	std::string temp((char *)right->data.data_val, right->data.data_len);
	return db->map->RemoveEntryByRight(temp);
}

struct NfsFsdevEntry get_entry_by_left(struct database *db, unsigned long left)
{
	return db->map->GetEntryByLeft(left);
}

struct NfsFsdevEntry get_entry_by_right(struct database *db, struct persistent_nfs_fh3 *right)
{
	std::string temp((char *)right->data.data_val, right->data.data_len);
	return db->map->GetEntryByRight(temp);
}

unsigned long generate_left_key(struct database *db)
{
	return db->map->GenerateLeftKey();
}

bool check_if_exist_by_left(struct database *db, unsigned long left)
{
	return db->map->CheckEntryExistByLeftKey(left);
}

bool check_if_exist_by_right(struct database *db, struct persistent_nfs_fh3 *right)
{
	std::string temp((char *)right->data.data_val, right->data.data_len);
	return db->map->CheckEntryExistByRightKey(temp);
}
}
