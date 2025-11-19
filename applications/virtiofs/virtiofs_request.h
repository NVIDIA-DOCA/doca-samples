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

#ifndef VIRTIOFS_REQUEST_H
#define VIRTIOFS_REQUEST_H

#include <sys/queue.h>
#include <stdbool.h>
#include <doca_types.h>
#include <doca_devemu_vfs_fuse_kernel.h>
#include <virtiofs_utils.h>

/* DMA task memcpy */
struct doca_dma_task_memcpy;

/* VirtioFS io request data */
struct virtiofs_io_req_data {
	struct doca_buf *host_doca_buf; /* Host doca buffer */
	struct doca_buf *arm_doca_buf;	/* ARM doca buffer */
	uint32_t length;		/* Length */
};

/* VirtioFS request dma operation */
enum virtiofs_request_dma_op {
	VIRTIOFS_REQUEST_DMA_OP_FROM_HOST, /* From host */
	VIRTIOFS_REQUEST_DMA_OP_TO_HOST,   /* To host */
};

/* VirtioFS fsdev io callback */
typedef void (*vfs_doca_fsdev_io_cb)(void *app_ctxt, int status);

/* VirtioFS request */
struct virtiofs_request {
	struct virtiofs_device_io_ctx *dio;		  /* Device io context */
	struct doca_devemu_vfs_fuse_req *devemu_fuse_req; /* VFS fuse request */
	struct virtiofs_io_req_data datain;		  /* Input data */
	struct virtiofs_io_req_data dataout;		  /* Output data */
	struct doca_task *task;				  /* Task */
	size_t dma_to_host_offset;			  /* DMA to host offset */
	int backend_status;				  /* Backend status */
	char *hdr_in;					  /* Input header */
	char *cmnd_hdr_in;				  /* Command input header */
	struct doca_buf *in_datain;			  /* Input data */
	char *hdr_out;					  /* Output header */
	char *cmnd_hdr_out;				  /* Command output header */
	struct doca_buf *out_dataout;			  /* Output data */
	vfs_doca_fsdev_io_cb cb;			  /* Callback */
	enum virtiofs_request_dma_op dma_op;		  /* DMA operation */
	TAILQ_ENTRY(virtiofs_request) entry;		  /* List entry */
	TAILQ_ENTRY(virtiofs_request) order;
};

/*
 * VirtioFS request dma done
 *
 * @param dma_task [in]: DMA task
 * @param task_user_data [in]: Task user data
 * @param ctx_user_data [in]: Context user data
 */
void virtiofs_request_dma_done(struct doca_dma_task_memcpy *dma_task,
			       union doca_data task_user_data,
			       union doca_data ctx_user_data);

/*
 * VirtioFS request dma error
 *
 * @param dma_task [in]: DMA task
 * @param task_user_data [in]: Task user data
 * @param ctx_user_data [in]: Context user data
 */
void virtiofs_request_dma_err(struct doca_dma_task_memcpy *dma_task,
			      union doca_data task_user_data,
			      union doca_data ctx_user_data);

/*
 * VirtioFS request init handler
 *
 * @param req [in]: VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] init_in Init input
 * @param [out] out Output header
 * @param [out] init_out Init output
 */
void virtiofs_request_init_handler(struct doca_devemu_vfs_fuse_req *req,
				   void *req_user_data,
				   struct fuse_in_header *in,
				   struct fuse_init_in *init_in,
				   struct fuse_out_header *out,
				   struct fuse_init_out *init_out);

/*
 * VirtioFS request destroy handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [out] out Output header
 */
void virtiofs_request_destroy_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_out_header *out);

/*
 * VirtioFS request lookup handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] datain Input data
 * @param [out] out Output header
 * @param [out] entry Entry
 */
void virtiofs_request_lookup_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct doca_buf *datain,
				     struct fuse_out_header *out,
				     struct fuse_entry_out *entry);

/*
 * VirtioFS request open handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] open_in Open input
 * @param [out] out Output header
 * @param [out] open_out Open output
 */
void virtiofs_request_open_handler(struct doca_devemu_vfs_fuse_req *req,
				   void *req_user_data,
				   struct fuse_in_header *in,
				   struct fuse_open_in *open_in,
				   struct fuse_out_header *out,
				   struct fuse_open_out *open_out);

/*
 * VirtioFS request create handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] create_in Create input
 * @param [in] datain Input data
 * @param [out] out Output header
 * @param [out] entry_out Entry output
 * @param [out] open_out Open output
 */
void virtiofs_request_create_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct fuse_create_in *create_in,
				     struct doca_buf *datain,
				     struct fuse_out_header *out,
				     struct fuse_entry_out *entry_out,
				     struct fuse_open_out *open_out);

/*
 * VirtioFS request read handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] read_in Read input
 * @param [out] out Output header
 * @param [out] dataout Output data
 */
void virtiofs_request_read_handler(struct doca_devemu_vfs_fuse_req *req,
				   void *req_user_data,
				   struct fuse_in_header *in,
				   struct fuse_read_in *read_in,
				   struct fuse_out_header *out,
				   struct doca_buf *dataout);

/*
 * VirtioFS request write handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] write_in Write input
 * @param [in] datain Input data
 * @param [out] out Output header
 * @param [out] write_out Write output
 */
void virtiofs_request_write_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_write_in *write_in,
				    struct doca_buf *datain,
				    struct fuse_out_header *out,
				    struct fuse_write_out *write_out);

/*
 * VirtioFS request mknod handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] mknod_in Mknod input
 * @param [in] datain Input data
 * @param [out] out Output header
 * @param [out] entry_out Entry output
 */
void virtiofs_request_mknod_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_mknod_in *mknod_in,
				    struct doca_buf *datain,
				    struct fuse_out_header *out,
				    struct fuse_entry_out *entry_out);

/*
 * VirtioFS request readdir handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] read_in Read input
 * @param [out] out Output header
 * @param [out] dataout Output data
 */
void virtiofs_request_readdir_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_read_in *read_in,
				      struct fuse_out_header *out,
				      struct doca_buf *dataout);

/*
 * VirtioFS request getattr handler
 *
 * @param req [in]: VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] getattr_in Getattr input
 * @param [out] out Output header
 * @param [out] attr_out Attribute output
 */
void virtiofs_request_getattr_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_getattr_in *getattr_in,
				      struct fuse_out_header *out,
				      struct fuse_attr_out *attr_out);

/*
 * VirtioFS request releasedir handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] release_in Release input
 * @param [out] out Output header
 */
void virtiofs_request_releasedir_handler(struct doca_devemu_vfs_fuse_req *req,
					 void *req_user_data,
					 struct fuse_in_header *in,
					 struct fuse_release_in *release_in,
					 struct fuse_out_header *out);

/*
 * VirtioFS request opendir handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] open_in Open input
 * @param [out] out Output header
 * @param [out] open_out Open output
 */
void virtiofs_request_opendir_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_open_in *open_in,
				      struct fuse_out_header *out,
				      struct fuse_open_out *open_out);

/*
 * VirtioFS request setattr handler
 *
 * @param req [in]: VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] setattr_in Setattr input
 * @param [out] out Output header
 * @param [out] attr_out Attribute output
 */
void virtiofs_request_setattr_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_setattr_in *setattr_in,
				      struct fuse_out_header *out,
				      struct fuse_attr_out *attr_out);

/*
 * VirtioFS request release handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] release_in Release input
 * @param [out] out Output header
 */
void virtiofs_request_release_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_release_in *setattr_in,
				      struct fuse_out_header *out);

/*
 * VirtioFS request flush handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] flush_in Flush input
 * @param [out] out Output header
 */
void virtiofs_request_flush_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_flush_in *setattr_in,
				    struct fuse_out_header *out);

/*
 * VirtioFS request forget handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] forget_in Forget input
 */
void virtiofs_request_forget_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct fuse_forget_in *forget_in);

/*
 * VirtioFS request statfs handler
 *
 * @param req [in]: VFS fuse request
 * @param req_user_data [in]: Request user data
 * @param [in] in Input header
 * @param [out] out Output header
 * @param [out] stafs_out Statfs output
 */
void virtiofs_request_statfs_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct fuse_out_header *out,
				     struct fuse_statfs_out *stafs_out);

/*
 * VirtioFS request unlink handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] datain Input data
 * @param [out] out Output header
 */
void virtiofs_request_unlink_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct doca_buf *datain,
				     struct fuse_out_header *out);

/*
 * VirtioFS request getxattr handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] getxattr_in Getxattr input
 * @param [in] datain Input data
 * @param [out] out Output header
 * @param [out] getxattr_out Getxattr output
 * @param [out] dataout Output data
 */
void virtiofs_request_getxattr_handler(struct doca_devemu_vfs_fuse_req *req,
				       void *req_user_data,
				       struct fuse_in_header *in,
				       struct fuse_getxattr_in *getxattr_in,
				       struct doca_buf *datain,
				       struct fuse_out_header *out,
				       struct fuse_getxattr_out *getxattr_out,
				       struct doca_buf *dataout);

/*
 * VirtioFS request mkdir handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] mkdir_in Mkdir input
 * @param [in] datain Input data
 * @param [out] out Output header
 * @param [out] entry Entry
 */
void virtiofs_request_mkdir_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_mkdir_in *mkdir_in,
				    struct doca_buf *datain,
				    struct fuse_out_header *out,
				    struct fuse_entry_out *entry);

/*
 * VirtioFS request rmdir handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] datain Input data
 * @param [out] out Output header
 */
void virtiofs_request_rmdir_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct doca_buf *datain,
				    struct fuse_out_header *out);

/*
 * VirtioFS request rename handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] rename_in Rename input
 * @param [in] datain Input data
 * @param [out] out Output header
 */
void virtiofs_request_rename_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct fuse_rename_in *rename_in,
				     struct doca_buf *datain,
				     struct fuse_out_header *out);

/*
 * VirtioFS request rename2 handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] rename_in Rename input
 * @param [in] datain Input data
 * @param [out] out Output header
 */
void virtiofs_request_rename2_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct fuse_rename2_in *rename_in,
				      struct doca_buf *datain,
				      struct fuse_out_header *out);

/*
 * VirtioFS request readlink handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [out] out Output header
 * @param [out] dataout Output data
 */
void virtiofs_request_readlink_handler(struct doca_devemu_vfs_fuse_req *req,
				       void *req_user_data,
				       struct fuse_in_header *in,
				       struct fuse_out_header *out,
				       struct doca_buf *dataout);

/*
 * VirtioFS request symlink handler
 *
 * @param req [in]: VFS fuse request
 * @param req_user_data [in]: Request user data
 * @param in [in]: Input header
 * @param [in] datain Input data
 * @param [out] out Output header
 * @param [out] entry Entry
 */
void virtiofs_request_symlink_handler(struct doca_devemu_vfs_fuse_req *req,
				      void *req_user_data,
				      struct fuse_in_header *in,
				      struct doca_buf *datain,
				      struct fuse_out_header *out,
				      struct fuse_entry_out *entry);

/*
 * VirtioFS request link handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] link_in Link input
 * @param [in] datain Input data
 * @param [out] out Output header
 * @param [out] entry Entry
 */
void virtiofs_request_link_handler(struct doca_devemu_vfs_fuse_req *req,
				   void *req_user_data,
				   struct fuse_in_header *in,
				   struct fuse_link_in *link_in,
				   struct doca_buf *datain,
				   struct fuse_out_header *out,
				   struct fuse_entry_out *entry);

/*
 * VirtioFS request fsync handler
 *
 * @param req [in]: VFS fuse request
 * @param req_user_data [in]: Request user data
 * @param [in] in Input header
 * @param [in] fsync_in Fsync input
 * @param [out] out Output header
 */
void virtiofs_request_fsync_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_fsync_in *fsync_in,
				    struct fuse_out_header *out);

/*
 * VirtioFS request fallocate handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] fallocate_in Fallocate input
 * @param [out] out Output header
 */
void virtiofs_request_fallocate_handler(struct doca_devemu_vfs_fuse_req *req,
					void *req_user_data,
					struct fuse_in_header *in,
					struct fuse_fallocate_in *fallocate_in,
					struct fuse_out_header *out);

/*
 * VirtioFS request setxattr handler
 *
 * @param req [in]: VFS fuse request
 * @param req_user_data [in]: Request user data
 * @param in [in]: Input header
 * @param [in] setxattr_in Setxattr input
 * @param [in] datain Input data
 * @param [out] out Output header
 */
void virtiofs_request_setxattr_handler(struct doca_devemu_vfs_fuse_req *req,
				       void *req_user_data,
				       struct fuse_in_header *in,
				       struct fuse_setxattr_in *setxattr_in,
				       struct doca_buf *datain,
				       struct fuse_out_header *out);

/*
 * VirtioFS request listxattr handler
 *
 * @param [in] req VFS fuse request
 * @param [in] req_user_data Request user data
 * @param [in] in Input header
 * @param [in] listxattr_in Listxattr input
 * @param [out] out Output header
 * @param [out] listxattr_out Listxattr output
 * @param [out] dataout Output data
 */
void virtiofs_request_listxattr_handler(struct doca_devemu_vfs_fuse_req *req,
					void *req_user_data,
					struct fuse_in_header *in,
					struct fuse_getxattr_in *listxattr_in,
					struct fuse_out_header *out,
					struct fuse_getxattr_out *listxattr_out,
					struct doca_buf *dataout);

/*
 * VirtioFS request removexattr handler
 *
 * @param req [in]: VFS fuse request
 * @param req_user_data [in]: Request user data
 * @param [in] in Input header
 * @param [in] datain Input data
 * @param [out] out Output header
 */
void virtiofs_request_removexattr_handler(struct doca_devemu_vfs_fuse_req *req,
					  void *req_user_data,
					  struct fuse_in_header *in,
					  struct doca_buf *datain,
					  struct fuse_out_header *out);

/*
 * VirtioFS request syncfs handler
 *
 * @param req [in]: VFS fuse request
 * @param req_user_data [in]: Request user data
 * @param [in] in Input header
 * @param [in] syncfs_in Syncfs input
 * @param [out] out Output header
 */
void virtiofs_request_syncfs_handler(struct doca_devemu_vfs_fuse_req *req,
				     void *req_user_data,
				     struct fuse_in_header *in,
				     struct fuse_syncfs_in *syncfs_in,
				     struct fuse_out_header *out);

void virtiofs_request_ioctl_handler(struct doca_devemu_vfs_fuse_req *req,
				    void *req_user_data,
				    struct fuse_in_header *in,
				    struct fuse_ioctl_in *ioctl_in,
				    struct doca_buf *datain,
				    struct fuse_out_header *out,
				    struct fuse_ioctl_out *ioctl_out,
				    struct doca_buf *dataout);

#endif /* VIRTIOFS_REQUEST_H */
