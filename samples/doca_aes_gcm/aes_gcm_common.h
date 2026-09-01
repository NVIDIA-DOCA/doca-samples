/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef AES_GCM_COMMON_H_
#define AES_GCM_COMMON_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <doca_dev.h>
#include <doca_aes_gcm.h>
#include <doca_mmap.h>
#include <doca_error.h>

#define USER_MAX_FILE_NAME 255		       /* Max file name length */
#define MAX_FILE_NAME (USER_MAX_FILE_NAME + 1) /* Max file name string length */

#define AES_GCM_KEY_128_SIZE_IN_BYTES 16		   /* AES-GCM 128 bits key size */
#define AES_GCM_KEY_256_SIZE_IN_BYTES 32		   /* AES-GCM 256 bits key size */
#define MAX_AES_GCM_KEY_SIZE AES_GCM_KEY_256_SIZE_IN_BYTES /* Max AES-GCM key size in bytes */

#define AES_GCM_KEY_128_STR_SIZE (AES_GCM_KEY_128_SIZE_IN_BYTES * 2) /* AES-GCM 128 bits key string size */
#define AES_GCM_KEY_256_STR_SIZE (AES_GCM_KEY_256_SIZE_IN_BYTES * 2) /* AES-GCM 256 bits key string size */
#define MAX_AES_GCM_KEY_STR_SIZE (AES_GCM_KEY_256_STR_SIZE + 1)	     /* Max AES-GCM key string size */

#define AES_GCM_AUTH_TAG_96_SIZE_IN_BYTES 12  /* AES-GCM 96 bits authentication tag size */
#define AES_GCM_AUTH_TAG_128_SIZE_IN_BYTES 16 /* AES-GCM 128 bits authentication tag size */

#define MAX_AES_GCM_IV_LENGTH 12				    /* Max IV length in bytes */
#define MAX_AES_GCM_IV_STR_LENGTH ((MAX_AES_GCM_IV_LENGTH * 2) + 1) /* Max IV string length */

#define SLEEP_IN_NANOS (10 * 1000) /* Sample the task every 10 microseconds */
#define NUM_AES_GCM_TASKS (1)	   /* Number of AES-GCM tasks */

/* AES-GCM modes */
enum aes_gcm_mode {
	AES_GCM_MODE_ENCRYPT, /* Encrypt mode */
	AES_GCM_MODE_DECRYPT, /* Decrypt mode */
};

/* Configuration struct */
struct aes_gcm_cfg {
	char file_path[MAX_FILE_NAME];		      /* File to encrypt/decrypt */
	char output_path[MAX_FILE_NAME];	      /* Output file */
	char pci_address[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* Device PCI address */
	uint8_t raw_key[MAX_AES_GCM_KEY_SIZE];	      /* Raw key */
	enum doca_aes_gcm_key_type raw_key_type;      /* Raw key type */
	uint8_t iv[MAX_AES_GCM_IV_LENGTH];	      /* Initialization vector */
	uint32_t iv_length;			      /* Initialization vector length */
	uint32_t tag_size;			      /* Authentication tag size */
	uint32_t aad_size;			      /* Additional authenticated data size */
	enum aes_gcm_mode mode;			      /* AES-GCM task type */
	int num_src_buf;			      /* Number of linked_list doca_buf element for the source buffer */
	int num_dst_buf; /* Number of linked_list doca_buf element for the destination buffer */
};

/* DOCA AES-GCM resources */
struct aes_gcm_resources {
	struct program_core_objects *state; /* DOCA program core objects */
	struct doca_aes_gcm *aes_gcm;	    /* DOCA AES-GCM context */
	size_t num_remaining_tasks;	    /* Number of remaining AES-GCM tasks */
	enum aes_gcm_mode mode;		    /* AES-GCM mode - encrypt/decrypt */
	bool run_pe_progress;		    /* Controls whether progress loop should run */
};

/*
 * Initialize AES-GCM parameters for the sample.
 *
 * @aes_gcm_cfg [in]: AES-GCM configuration struct
 */
void init_aes_gcm_params(struct aes_gcm_cfg *aes_gcm_cfg);

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_aes_gcm_params(void);

/*
 * Allocate DOCA AES-GCM resources
 *
 * @pci_addr [in]: Device PCI address
 * @max_bufs [in]: Maximum number of buffers for DOCA Inventory
 * @resources [out]: DOCA AES-GCM resources to allocate
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t allocate_aes_gcm_resources(const char *pci_addr, uint32_t max_bufs, struct aes_gcm_resources *resources);

/*
 * Destroy DOCA AES-GCM resources
 *
 * @resources [in]: DOCA AES-GCM resources to destroy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_aes_gcm_resources(struct aes_gcm_resources *resources);

/*
 * Submit AES-GCM encrypt task and wait for completion
 *
 * @resources [in]: DOCA AES-GCM resources
 * @src_buf [in]: Source buffer
 * @dst_buf [in]: Destination buffer
 * @key [in]: DOCA AES-GCM key
 * @iv [in]: Initialization vector
 * @iv_length [in]: Initialization vector length in bytes
 * @tag_size [in]: Authentication tag size in bytes
 * @aad_size [in]: Additional authenticated data size in bytes
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t submit_aes_gcm_encrypt_task(struct aes_gcm_resources *resources,
					 struct doca_buf *src_buf,
					 struct doca_buf *dst_buf,
					 struct doca_aes_gcm_key *key,
					 const uint8_t *iv,
					 uint32_t iv_length,
					 uint32_t tag_size,
					 uint32_t aad_size);

/*
 * Submit AES-GCM decrypt task and wait for completion
 *
 * @resources [in]: DOCA AES-GCM resources
 * @src_buf [in]: Source buffer
 * @dst_buf [in]: Destination buffer
 * @key [in]: DOCA AES-GCM key
 * @iv [in]: Initialization vector
 * @iv_length [in]: Initialization vector length in bytes
 * @tag_size [in]: Authentication tag size in bytes
 * @aad_size [in]: Additional authenticated data size in bytes
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t submit_aes_gcm_decrypt_task(struct aes_gcm_resources *resources,
					 struct doca_buf *src_buf,
					 struct doca_buf *dst_buf,
					 struct doca_aes_gcm_key *key,
					 const uint8_t *iv,
					 uint32_t iv_length,
					 uint32_t tag_size,
					 uint32_t aad_size);

/*
 * Check if given device is capable of executing a DOCA AES-GCM encrypt task.
 *
 * @devinfo [in]: The DOCA device information
 * @return: DOCA_SUCCESS if the device supports DOCA AES-GCM encrypt task and DOCA_ERROR otherwise
 */
doca_error_t aes_gcm_task_encrypt_is_supported(struct doca_devinfo *devinfo);

/*
 * Check if given device is capable of executing a DOCA AES-GCM decrypt task.
 *
 * @devinfo [in]: The DOCA device information
 * @return: DOCA_SUCCESS if the device supports DOCA AES-GCM decrypt task and DOCA_ERROR otherwise
 */
doca_error_t aes_gcm_task_decrypt_is_supported(struct doca_devinfo *devinfo);

/*
 * Encrypt task completed callback
 *
 * @encrypt_task [in]: Completed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
void encrypt_completed_callback(struct doca_aes_gcm_task_encrypt *encrypt_task,
				union doca_data task_user_data,
				union doca_data ctx_user_data);

/*
 * Encrypt task error callback
 *
 * @encrypt_task [in]: failed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
void encrypt_error_callback(struct doca_aes_gcm_task_encrypt *encrypt_task,
			    union doca_data task_user_data,
			    union doca_data ctx_user_data);

/*
 * Decrypt task completed callback
 *
 * @decrypt_task [in]: Completed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
void decrypt_completed_callback(struct doca_aes_gcm_task_decrypt *decrypt_task,
				union doca_data task_user_data,
				union doca_data ctx_user_data);

/*
 * Decrypt task error callback
 *
 * @decrypt_task [in]: failed task
 * @task_user_data [in]: doca_data from the task
 * @ctx_user_data [in]: doca_data from the context
 */
void decrypt_error_callback(struct doca_aes_gcm_task_decrypt *decrypt_task,
			    union doca_data task_user_data,
			    union doca_data ctx_user_data);

#endif /* AES_GCM_COMMON_H_ */
