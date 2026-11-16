/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << 26)
#endif

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <linux/nvme_ioctl.h>
#include <linux/types.h>

#include <doca_dev.h>
#include <doca_kv_dev.h>

#include "memos_fio_utils.h"

DOCA_LOG_REGISTER(MEMOS_FIO_UTILS);

static inline uint64_t memos_fio_utils_offset_to_key_xxh64(uint64_t offset)
{
	return XXH64(&offset, sizeof(offset), 0);
}

static inline uint64_t memos_fio_utils_offset_to_key_splitmix64(uint64_t offset)
{
	uint64_t z = offset + 0x9e3779b97f4a7c15ULL;

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static inline uint64_t memos_fio_utils_offset_to_key_murmur3_64(uint64_t offset)
{
	offset ^= offset >> 33;
	offset *= 0xff51afd7ed558ccdULL;
	offset ^= offset >> 33;
	offset *= 0xc4ceb9fe1a85ec53ULL;
	offset ^= offset >> 33;
	return offset;
}

static inline uint64_t memos_fio_utils_offset_to_key_u64(uint32_t hash_type, uint64_t offset)
{
	switch (hash_type) {
	case MEMOS_FIO_PLUGIN_HASH_TYPE_XXH64:
		return memos_fio_utils_offset_to_key_xxh64(offset);
	case MEMOS_FIO_PLUGIN_HASH_TYPE_SPLITMIX64:
		return memos_fio_utils_offset_to_key_splitmix64(offset);
	case MEMOS_FIO_PLUGIN_HASH_TYPE_MURMUR3_64:
		return memos_fio_utils_offset_to_key_murmur3_64(offset);
	default:
		DOCA_LOG_ERR("Invalid hash_type=%" PRIu32 ", using splitmix64", hash_type);
		return memos_fio_utils_offset_to_key_splitmix64(offset);
	}
}

_Static_assert(MEMOS_FIO_PLUGIN_MEMOS_KEY_SIZE == sizeof(uint64_t), "KV key must hold a 64-bit hash");

void memos_fio_utils_offset_to_key(uint32_t hash_type, uint64_t offset, uint8_t key[MEMOS_FIO_PLUGIN_MEMOS_KEY_SIZE])
{
	const uint64_t hash = memos_fio_utils_offset_to_key_u64(hash_type, offset);

	/* 'key' is a byte buffer with no alignment guarantee, so it must not be stored through a uint64_t *. */
	memcpy(key, &hash, sizeof(hash));
}

#define MEMOS_FIO_NVME_ADMIN_IDENTIFY 0x06
#define MEMOS_FIO_NVME_IDENTIFY_NS 0x00

/*
 * Standard NVMe Identify Namespace data structure (partial).
 * Layout matches the NVMe-kernel backend / NVMe Base spec Figure 97.
 */
struct memos_fio_nvme_idfy_ns {
	__u64 nsze;
	__u64 ncap;
	__u64 nuse;
	__u8 nsfeat;
	__u8 nlbaf;
	__u8 flbas;
	__u8 mc;
	__u8 dpc;
	__u8 dps;
	__u8 nmic;
	__u8 rescap;
	__u8 fpi;
	__u8 dlfeat;
	__u16 nawun;
	__u16 nawupf;
	__u16 nacwu;
	__u16 nabsn;
	__u16 nabo;
	__u16 nabspf;
	__u16 noiob;
	__u8 nvmcap[16];
	__u16 npwg;
	__u16 npwa;
	__u16 npdg;
	__u16 npda;
	__u16 nows;
	__u8 rsvd74[18];
	__u32 anagrpid;
	__u8 rsvd96[3];
	__u8 nsattr;
	__u16 nvmsetid;
	__u16 endgid;
	__u8 nguid[DOCA_KV_DEV_NGUID_LEN];
	__u64 eui64;
	__u8 rsvd128[3968];
};

_Static_assert(sizeof(struct memos_fio_nvme_idfy_ns) == 4096, "Identify Namespace struct size mismatch");
_Static_assert(offsetof(struct memos_fio_nvme_idfy_ns, nguid) == 104, "Identify Namespace NGUID offset mismatch");

static int memos_fio_utils_nvme_identify(int fd, uint32_t nsid, uint32_t cns, void *data)
{
	struct nvme_passthru_cmd cmd = {
		.opcode = MEMOS_FIO_NVME_ADMIN_IDENTIFY,
		.nsid = nsid,
		.addr = (uint64_t)(uintptr_t)data,
		.data_len = 4096,
		.cdw10 = cns,
	};

	if (ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) != 0)
		return -1;

	return 0;
}

static int memos_fio_utils_query_ns_nguid(int fd, uint8_t nguid[DOCA_KV_DEV_NGUID_LEN])
{
	struct memos_fio_nvme_idfy_ns idfy;
	int nsid;

	nsid = ioctl(fd, NVME_IOCTL_ID);
	if (nsid < 0)
		return -1;

	if (memos_fio_utils_nvme_identify(fd, (uint32_t)nsid, MEMOS_FIO_NVME_IDENTIFY_NS, &idfy) < 0)
		return -1;

	memcpy(nguid, idfy.nguid, DOCA_KV_DEV_NGUID_LEN);
	return 0;
}

doca_error_t memos_fio_utils_wait_state(struct doca_ctx *ctx, struct doca_pe *pe, enum doca_ctx_states desired_state)
{
	doca_error_t result;
	enum doca_ctx_states actual_state;

	for (;;) {
		result = doca_ctx_get_state(ctx, &actual_state);
		if (result != DOCA_SUCCESS)
			return result;

		if (actual_state == desired_state)
			break;

		if (desired_state == DOCA_CTX_STATE_RUNNING && actual_state == DOCA_CTX_STATE_IDLE)
			return DOCA_ERROR_BAD_STATE;

		doca_pe_progress(pe);
	}

	return DOCA_SUCCESS;
}

doca_error_t memos_fio_utils_open_device(const char *device_name, struct doca_dev **doca_device)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0;
	doca_error_t result;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	uint32_t i = 0;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load DOCA devices list: %s", doca_error_get_descr(result));
		return result;
	}

	*doca_device = NULL;

	/* Search device with same dev name*/
	for (i = 0; i < nb_devs; i++) {
		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result == DOCA_SUCCESS) {
			DOCA_LOG_DBG("Found DOCA device (%s)", ibdev_name);

			if (strlen(device_name) != 0 &&
			    strncmp(device_name, ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE) != 0)
				continue;
		}
		result = doca_dev_open(dev_list[i], doca_device);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open DOCA device (%s): %s", device_name, doca_error_get_descr(result));
			goto out;
		}
		break;
	}

out:
	doca_devinfo_destroy_list(dev_list);

	if (*doca_device == NULL) {
		DOCA_LOG_ERR("Couldn't get DOCA device (%s)", device_name);
		return DOCA_ERROR_NOT_FOUND;
	}

	return result;
}

bool memos_fio_utils_check_ctx_state(const struct doca_ctx *dctx,
				     enum doca_ctx_states check_state,
				     const char *error_msg)
{
	enum doca_ctx_states state = memos_fio_utils_get_ctx_state(dctx);

	if (state != check_state) {
		if (error_msg) {
			DOCA_LOG_ERR("%s, current state is %d (should be %d)", error_msg, state, check_state);
		}
		return false;
	}

	return true;
}

/*
 * Match fio mem=mmaphuge / hugepage-size=2M used with this ioengine.
 * SNAP UMR KLMs are one entry per contiguous physical region; a 2MiB value
 * backed by 4KiB pages needs hundreds of KLMs and overflows the QP WQE BB
 * ("WQE BB size 20 is bigger than max QP SQ WQE size 16"). One 2MiB hugetlb
 * page keeps the UMR small.
 */
#define MEMOS_FIO_IO_HUGEPAGE_SIZE (2UL * 1024 * 1024)

static size_t memos_fio_utils_io_map_len(size_t alignment, size_t size)
{
	size_t map_align = alignment > MEMOS_FIO_IO_HUGEPAGE_SIZE ? alignment : MEMOS_FIO_IO_HUGEPAGE_SIZE;

	return (size + map_align - 1) & ~(map_align - 1);
}

void *memos_fio_utils_allocate_aligned_buffer(size_t alignment, size_t size)
{
	size_t map_len;
	void *addr;

	if (alignment == 0 || size == 0)
		return NULL;

	if ((alignment & (alignment - 1)) != 0) {
		DOCA_LOG_ERR("'alignment' (%zu) is not a power of two", alignment);
		return NULL;
	}

	map_len = memos_fio_utils_io_map_len(alignment, size);
	addr = mmap(NULL,
		    map_len,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
		    -1,
		    0);
	if (addr == MAP_FAILED) {
		DOCA_LOG_ERR("Failed to mmap %zu-byte hugepage IO buffer (align=%zu): %s",
			     map_len,
			     alignment,
			     strerror(errno));
		return NULL;
	}

	return addr;
}

void memos_fio_utils_free_aligned_buffer(void *buffer, size_t alignment, size_t size)
{
	size_t map_len;

	if (buffer == NULL || alignment == 0 || size == 0)
		return;

	map_len = memos_fio_utils_io_map_len(alignment, size);
	if (munmap(buffer, map_len) != 0)
		DOCA_LOG_ERR("Failed to munmap %zu-byte hugepage IO buffer: %s", map_len, strerror(errno));
}

bool memos_fio_utils_parse_nsid_file_name(char *file_name,
					  const char **nsids,
					  uint32_t nsid_arr_size,
					  uint32_t *nsids_count)
{
	char *token;
	char *saveptr = NULL;
	uint32_t count = 0;

	if (file_name == NULL || nsids == NULL || nsids_count == NULL || nsid_arr_size == 0)
		return false;

	*nsids_count = 0;

	token = strtok_r(file_name, ":", &saveptr);
	while (token != NULL) {
		if (count >= nsid_arr_size)
			return false;

		nsids[count++] = token;
		token = strtok_r(NULL, ":", &saveptr);
	}

	*nsids_count = count;
	return (count > 0) ? true : false;
}

/** Accept 0 (use default in init_configuration) or any value from enum doca_log_level in doca_log.h */
bool memos_fio_utils_validate_doca_log_level(uint32_t level)
{
	if (level == 0)
		return true;
	switch (level) {
	case DOCA_LOG_LEVEL_DISABLE:
	case DOCA_LOG_LEVEL_CRIT:
	case DOCA_LOG_LEVEL_ERROR:
	case DOCA_LOG_LEVEL_WARNING:
	case DOCA_LOG_LEVEL_INFO:
	case DOCA_LOG_LEVEL_DEBUG:
	case DOCA_LOG_LEVEL_TRACE:
		return true;
	default:
		return false;
	}
}

bool memos_fio_utils_validate_num_io_threads(uint32_t num_io_threads)
{
	long nproc = sysconf(_SC_NPROCESSORS_ONLN);
	unsigned long long nproc_u;

	if (nproc < 1) {
		fprintf(stderr, "BAD configuration: online CPU count is less than 1\n");
		return false;
	}

	nproc_u = (unsigned long long)nproc;

	if ((unsigned long long)num_io_threads > nproc_u) {
		fprintf(stderr,
			"BAD configuration: num_io_threads=%" PRIu32 " exceeds online CPU count (%llu)\n",
			num_io_threads,
			nproc_u);
		return false;
	}

	if (num_io_threads > MEMOS_FIO_PLUGIN_IO_COUNT_MAX) {
		fprintf(stderr,
			"BAD configuration: num_io_threads=%" PRIu32 " exceeds plugin maximum (%u)\n",
			num_io_threads,
			MEMOS_FIO_PLUGIN_IO_COUNT_MAX);
		return false;
	}

	return true;
}

bool memos_fio_utils_init_log(uint32_t fio_log_level, uint32_t doca_log_level, struct doca_log_backend **sdk_log)
{
	doca_error_t dres;

	if (!memos_fio_utils_validate_doca_log_level(fio_log_level)) {
		fprintf(stderr,
			"BAD configuration: invalid fio-log-level=%" PRIu32
			" (use 0 for default or a doca_log_level value: 10,20,30,40,50,60,70)",
			fio_log_level);
		return false;
	}

	if (!memos_fio_utils_validate_doca_log_level(doca_log_level)) {
		fprintf(stderr,
			"BAD configuration: invalid doca-log-level=%" PRIu32
			" (use 0 for default or a doca_log_level value: 10,20,30,40,50,60,70)",
			doca_log_level);
		return false;
	}

	/* Setup doca app/lib logger */
	dres = doca_log_backend_create_standard();
	if (dres != DOCA_SUCCESS)
		return false;

	dres = doca_log_backend_create_with_file_sdk(stderr, sdk_log);
	if (dres != DOCA_SUCCESS)
		return false;

	dres = doca_log_level_set_global_lower_limit(fio_log_level); /* DOCA_LOG_* */
	if (dres != DOCA_SUCCESS) {
		return false;
	}

	dres = doca_log_level_set_global_sdk_limit(doca_log_level); /* internal DOCA library logs */
	if (dres != DOCA_SUCCESS) {
		return false;
	}

	return true;
}

bool memos_fio_utils_get_nguid(const char *kvdev_path, uint8_t *nguid, uint16_t nguid_len)
{
	int fd;

	if (kvdev_path == NULL || nguid == NULL) {
		DOCA_LOG_ERR("'kvdev_path' and 'nguid' must not be NULL");
		return false;
	}

	if (nguid_len < DOCA_KV_DEV_NGUID_LEN) {
		DOCA_LOG_ERR("'nguid_len' (%u) is smaller than DOCA_KV_DEV_NGUID_LEN (%u)",
			     nguid_len,
			     DOCA_KV_DEV_NGUID_LEN);
		return false;
	}

	if (kvdev_path[0] == '\0') {
		DOCA_LOG_ERR("'kvdev_path' is empty");
		return false;
	}

	fd = open(kvdev_path, O_RDWR);
	if (fd < 0) {
		DOCA_LOG_ERR("Failed to open %s, errno=%d", kvdev_path, errno);
		return false;
	}

	if (memos_fio_utils_query_ns_nguid(fd, nguid) < 0) {
		DOCA_LOG_ERR("Failed to query namespace NGUID on %s", kvdev_path);
		close(fd);
		return false;
	}

	close(fd);
	return true;
}
