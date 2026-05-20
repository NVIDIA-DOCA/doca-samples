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

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <infiniband/verbs.h>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_flow.h>
#include <doca_dpa.h>
#include <doca_verbs.h>
#include <doca_verbs_bridge.h>
#include <doca_clock.h>

DOCA_LOG_REGISTER(VERBS_SEND_ETHERNET_FRAMES_ON_DPA);

#define ETH_SQ_LOGICAL_QUEUE_ID 0
#define FLOW_POART_ID 0
#define DST_MAC_ADDR "11:22:33:44:55:66"
#define UNKNOWN_ETHER_TYPE_IPV4 0x88b5
#define PACKETS_NUMBER 32
#define PACKET_SIZE 1500

struct ether_hdr {
	uint8_t dst_addr[DOCA_DEVINFO_MAC_ADDR_SIZE]; /* Destination addr bytes in tx order */
	uint8_t src_addr[DOCA_DEVINFO_MAC_ADDR_SIZE]; /* Source addr bytes in tx order */
	uint16_t ether_type;			      /* Frame type */
} __attribute__((__packed__));

struct verbs_eth_sq_sample_resources {
	struct doca_verbs_context *verbs_context;
	struct doca_verbs_pd *verbs_pd;
	struct doca_dev *dev;
	struct doca_flow_port *df_port;
	struct doca_dpa *dpa_ctx;
	struct doca_dpa_completion *dpa_completion;
	struct doca_verbs_eth_sq *verbs_eth_sq;
	struct ibv_mr *mr;
	void *buf;
	doca_dpa_dev_verbs_eth_sq_t verbs_eth_sq_handle;
	doca_dpa_dev_completion_t dpa_completion_handle;
	uint32_t mkey;
};

/*
 * A struct that includes all needed info on registered kernels and is initialized during linkage by DPACC.
 * Variable name should be the token passed to DPACC with --app-name parameter.
 */
extern struct doca_dpa_app *verbs_sample_app;

/**
 * Send Ethernet frames RPC declaration
 */
doca_dpa_func_t send_ethernet_frames_rpc;

/*
 * Create verbs context from IB device name
 *
 * @ib_device_name [in]: IB device name
 * @verbs_ctx [out]: verbs context
 * @supported_ts_source_type [out]: supported ts source type
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_verbs_context(const char *ib_device_name,
					 struct doca_verbs_context **verbs_ctx,
					 uint8_t *supported_ts_source_type)
{
	struct doca_devinfo **devinfo_list = NULL;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
	uint32_t nb_devs = 0;
	enum doca_pci_func_type pci_func_type;
	struct doca_verbs_device_attr *device_attr = NULL;
	doca_error_t status;

	status = doca_devinfo_create_list(&devinfo_list, &nb_devs);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create devinfo list: %s", doca_error_get_descr(status));
		return status;
	}

	/* Search for the requested device */
	for (uint32_t i = 0; i < nb_devs; i++) {
		status = doca_devinfo_get_ibdev_name(devinfo_list[i], ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
		if (status == DOCA_SUCCESS && (strcmp(ibdev_name, ib_device_name) == 0)) {
			status = doca_devinfo_get_pci_func_type(devinfo_list[i], &pci_func_type);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to get PCI function type: %s", doca_error_get_descr(status));
				(void)doca_devinfo_destroy_list(devinfo_list);
				return status;
			}
			if (pci_func_type != DOCA_PCI_FUNC_TYPE_PF) {
				DOCA_LOG_ERR("Device is not a PF");
				(void)doca_devinfo_destroy_list(devinfo_list);
				return DOCA_ERROR_INVALID_VALUE;
			}

			status = doca_verbs_context_create(devinfo_list[i],
							   DOCA_VERBS_CONTEXT_CREATE_FLAGS_NONE,
							   verbs_ctx);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to create verbs context: %s", doca_error_get_descr(status));
				(void)doca_devinfo_destroy_list(devinfo_list);
				return status;
			}

			status = doca_verbs_query_device(*verbs_ctx, &device_attr);
			if (status != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to query verbs device attributes: %s",
					     doca_error_get_descr(status));
				(void)doca_devinfo_destroy_list(devinfo_list);
				(void)doca_verbs_context_destroy(*verbs_ctx);
				return status;
			}

			if (doca_clock_cap_nic_real_time_is_supported(devinfo_list[i]) == DOCA_SUCCESS &&
			    doca_verbs_device_attr_get_is_eth_sq_ts_source_type_supported(
				    device_attr,
				    DOCA_VERBS_TS_SOURCE_REAL_TIME) == DOCA_SUCCESS) {
				*supported_ts_source_type = DOCA_VERBS_TS_SOURCE_REAL_TIME;
				DOCA_LOG_INFO("Using Timestamp Source Type: REAL_TIME");
			} else if (doca_clock_cap_nic_free_running_is_supported(devinfo_list[i]) == DOCA_SUCCESS &&
				   doca_verbs_device_attr_get_is_eth_sq_ts_source_type_supported(
					   device_attr,
					   DOCA_VERBS_TS_SOURCE_FREE_RUNNING) == DOCA_SUCCESS) {
				*supported_ts_source_type = DOCA_VERBS_TS_SOURCE_FREE_RUNNING;
				DOCA_LOG_INFO("Using Timestamp Source Type: FREE_RUNNING (with %u[kHz] frequency)",
					      doca_verbs_device_attr_get_ts_free_running_clock_frequency(device_attr));
			} else {
				*supported_ts_source_type = DOCA_VERBS_TS_SOURCE_DEFAULT;
				DOCA_LOG_INFO("Using Timestamp Source Type: DEFAULT");
			}

			(void)doca_verbs_device_attr_free(device_attr);

			break;
		}
	}

	status = doca_devinfo_destroy_list(devinfo_list);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy devinfo list: %s", doca_error_get_descr(status));
		if (*verbs_ctx)
			(void)doca_verbs_context_destroy(*verbs_ctx);
		return status;
	}

	if (*verbs_ctx == NULL) {
		DOCA_LOG_ERR("The requested device was not found");
		return DOCA_ERROR_NOT_FOUND;
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy verbs context
 *
 * @verbs_context [in]: verbs context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_verbs_context(struct doca_verbs_context *verbs_context)
{
	doca_error_t status;
	if (verbs_context != NULL) {
		status = doca_verbs_context_destroy(verbs_context);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy Verbs Context: %s", doca_error_get_descr(status));
			return status;
		}
	}

	verbs_context = NULL;

	return DOCA_SUCCESS;
}

/*
 * Create DPA context (along with verbs pd and doca dev)
 *
 * @verbs_context [in]: verbs context
 * @verbs_pd [out]: verbs pd
 * @dev [out]: doca dev
 * @dpa_ctx [out]: DPA context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_dpa_context(struct doca_verbs_context *verbs_context,
				       struct doca_verbs_pd **verbs_pd,
				       struct doca_dev **dev,
				       struct doca_dpa **dpa_ctx)
{
	doca_error_t status, tmp_status;
	status = doca_verbs_pd_create(verbs_context, verbs_pd);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs PD: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_verbs_pd_as_doca_dev(*verbs_pd, dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA dev: %s", doca_error_get_descr(status));
		goto destroy_verbs_pd;
	}

	status = doca_dpa_create(*dev, dpa_ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DPA context: %s", doca_error_get_descr(status));
		goto close_doca_dev;
	}

	status = doca_dpa_set_app(*dpa_ctx, verbs_sample_app);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA app: %s", doca_error_get_descr(status));
		goto destroy_dpa_context;
	}

	status = doca_dpa_start(*dpa_ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DPA context: %s", doca_error_get_descr(status));
		goto destroy_dpa_context;
	}

	return DOCA_SUCCESS;

destroy_dpa_context:
	tmp_status = doca_dpa_destroy(*dpa_ctx);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DPA context: %s", doca_error_get_descr(tmp_status));
	}
	return status;

close_doca_dev:
	tmp_status = doca_dev_close(*dev);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close DOCA dev: %s", doca_error_get_descr(tmp_status));
	}
destroy_verbs_pd:
	tmp_status = doca_verbs_pd_destroy(*verbs_pd);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close DOCA dev: %s", doca_error_get_descr(tmp_status));
	}
	return status;
}

/*
 * Destroy DPA context (along with verbs pd and doca dev)
 *
 * @verbs_pd [in]: verbs pd
 * @dev [in]: doca dev
 * @dpa_ctx [in]: DPA context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_dpa_context(struct doca_verbs_pd *verbs_pd, struct doca_dev *dev, struct doca_dpa *dpa_ctx)
{
	doca_error_t status;
	if (dpa_ctx != NULL) {
		status = doca_dpa_stop(dpa_ctx);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DPA context: %s", doca_error_get_descr(status));
			return status;
		}

		status = doca_dpa_destroy(dpa_ctx);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DPA context: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (dev != NULL) {
		status = doca_dev_close(dev);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close DOCA dev: %s", doca_error_get_descr(status));
			return status;
		}
	}

	if (verbs_pd != NULL) {
		status = doca_verbs_pd_destroy(verbs_pd);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy Verbs PD: %s", doca_error_get_descr(status));
			return status;
		}
	}

	dpa_ctx = NULL;
	dev = NULL;
	verbs_pd = NULL;

	return DOCA_SUCCESS;
}

/*
 * Create DPA completion
 *
 * @dpa_ctx [in]: DPA context
 * @queue_size [in]: DPA completion queue size
 * @dpa_completion [out]: DPA completion
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_dpa_completion(struct doca_dpa *dpa_ctx,
					  unsigned int queue_size,
					  struct doca_dpa_completion **dpa_completion)
{
	doca_error_t status, tmp_status;

	status = doca_dpa_completion_create(dpa_ctx, queue_size, dpa_completion);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DPA completion: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_dpa_completion_start(*dpa_completion);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DPA completion: %s", doca_error_get_descr(status));
		goto destroy_dpa_completion;
	}

	return DOCA_SUCCESS;

destroy_dpa_completion:
	tmp_status = doca_dpa_completion_destroy(*dpa_completion);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DPA completion: %s", doca_error_get_descr(tmp_status));
	}
	return status;
}

/*
 * Destroy DPA completion
 *
 * @dpa_completion [in]: DPA completion
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_dpa_completion(struct doca_dpa_completion *dpa_completion)
{
	doca_error_t status;
	if (dpa_completion != NULL) {
		status = doca_dpa_completion_destroy(dpa_completion);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DPA completion: %s", doca_error_get_descr(status));
			return status;
		}
	}

	dpa_completion = NULL;

	return DOCA_SUCCESS;
}

/*
 * Create verbs QP
 *
 * @verbs_context [in]: verbs context
 * @verbs_pd [in]: verbs pd
 * @dpa_ctx [in]: DPA context
 * @dpa_completion [in]: DPA completion
 * @ts_source_type [in]: Timestamp Source Type
 * @wr_num [in]: WR number
 * @queue_id [in]: queue id
 * @verbs_eth_sq [out]: verbs ETH SQ
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_verbs_eth_sq(struct doca_verbs_context *verbs_context,
					struct doca_verbs_pd *verbs_pd,
					struct doca_dpa *dpa_ctx,
					struct doca_dpa_completion *dpa_completion,
					uint8_t ts_source_type,
					uint32_t wr_num,
					uint16_t queue_id,
					struct doca_verbs_eth_sq **verbs_eth_sq)
{
	doca_error_t status, tmp_status;
	struct doca_verbs_eth_sq_init_attr *verbs_eth_sq_init_attr = NULL;
	struct doca_verbs_eth_sq *new_eth_sq = NULL;

	status = doca_verbs_eth_sq_init_attr_create(&verbs_eth_sq_init_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs ETH SQ attributes: %s", doca_error_get_descr(status));
		return status;
	}

	status = doca_verbs_eth_sq_init_attr_set_pd(verbs_eth_sq_init_attr, verbs_pd);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Verbs PD: %s", doca_error_get_descr(status));
		goto destroy_verbs_eth_sq_init_attr;
	}

	status = doca_verbs_eth_sq_init_attr_set_wr_num(verbs_eth_sq_init_attr, wr_num);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set WR number: %s", doca_error_get_descr(status));
		goto destroy_verbs_eth_sq_init_attr;
	}

	status = doca_verbs_eth_sq_init_attr_set_max_sges(verbs_eth_sq_init_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set max_sges: %s", doca_error_get_descr(status));
		goto destroy_verbs_eth_sq_init_attr;
	}

	status = doca_verbs_eth_sq_init_attr_set_queue_id(verbs_eth_sq_init_attr, queue_id);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set queue_id: %s", doca_error_get_descr(status));
		goto destroy_verbs_eth_sq_init_attr;
	}

	status = doca_verbs_eth_sq_init_attr_set_dpa(verbs_eth_sq_init_attr, dpa_ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA context: %s", doca_error_get_descr(status));
		goto destroy_verbs_eth_sq_init_attr;
	}

	status = doca_verbs_eth_sq_init_attr_set_dpa_completion(verbs_eth_sq_init_attr, dpa_completion);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA completion: %s", doca_error_get_descr(status));
		goto destroy_verbs_eth_sq_init_attr;
	}

	status = doca_verbs_eth_sq_init_attr_set_ts_source_type(verbs_eth_sq_init_attr, ts_source_type);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set Timestamp Source Type: %s", doca_error_get_descr(status));
		goto destroy_verbs_eth_sq_init_attr;
	}

	status = doca_verbs_eth_sq_init_attr_set_external_datapath_en(verbs_eth_sq_init_attr, 1);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set external datapath: %s", doca_error_get_descr(status));
		goto destroy_verbs_eth_sq_init_attr;
	}

	status = doca_verbs_eth_sq_create(verbs_context, verbs_eth_sq_init_attr, &new_eth_sq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs ETH SQ: %s", doca_error_get_descr(status));
		goto destroy_verbs_eth_sq_init_attr;
	}

	status = doca_verbs_eth_sq_init_attr_destroy(verbs_eth_sq_init_attr);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy Verbs ETH SQ attributes: %s", doca_error_get_descr(status));
		goto destroy_verbs_eth_sq;
	}

	*verbs_eth_sq = new_eth_sq;

	return DOCA_SUCCESS;

destroy_verbs_eth_sq:
	tmp_status = doca_verbs_eth_sq_destroy(new_eth_sq);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy Verbs ETH SQ: %s", doca_error_get_descr(tmp_status));

destroy_verbs_eth_sq_init_attr:
	tmp_status = doca_verbs_eth_sq_init_attr_destroy(verbs_eth_sq_init_attr);
	if (tmp_status != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy Verbs ETH SQ attributes: %s", doca_error_get_descr(tmp_status));

	return status;
}

/*
 * Destroy Verbs ETH SQ
 *
 * @verbs_eth_sq [in]: Verbs ETH SQ
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_verbs_eth_sq(struct doca_verbs_eth_sq *verbs_eth_sq)
{
	doca_error_t status;
	if (verbs_eth_sq != NULL) {
		status = doca_verbs_eth_sq_destroy(verbs_eth_sq);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy Verbs ETH SQ: %s", doca_error_get_descr(status));
			return status;
		}
	}

	verbs_eth_sq = NULL;

	return DOCA_SUCCESS;
}

/*
 * Create local memory objects
 *
 * @buf_size [in]: buffer size
 * @pd [in]: ib verbs pd
 * @buf [out]: allocated buffer
 * @mr [out]: ib verbs mr
 * @mkey [out]: mkey
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_local_memory_objects(size_t buf_size,
						struct ibv_pd *pd,
						void **buf,
						struct ibv_mr **mr,
						uint32_t *mkey)
{
	*buf = (void *)calloc(1, buf_size);
	if (*buf == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory buffer of size = %zu", buf_size);
		return DOCA_ERROR_NO_MEMORY;
	}

	*mr = ibv_reg_mr(pd,
			 (void *)*buf,
			 buf_size,
			 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
	if (*mr == NULL) {
		DOCA_LOG_ERR("Failed to register local buffer");
		free((void *)*buf);
		return DOCA_ERROR_NO_MEMORY;
	}

	*mkey = (*mr)->rkey;

	return DOCA_SUCCESS;
}

/*
 * Destroy local memory objects
 *
 * @mr [in]: ib verbs mr
 * @buf [in]: allocated buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_local_memory_objects(struct ibv_mr *mr, void *buf)
{
	int ret = 0;

	if (mr != NULL) {
		ret = ibv_dereg_mr(mr);
		if (ret != 0) {
			DOCA_LOG_ERR("ibv_dereg_mr failed with error=%d", ret);
			return DOCA_ERROR_DRIVER;
		}
	}

	if (buf != NULL)
		free((void *)buf);

	mr = NULL;
	buf = NULL;

	return DOCA_SUCCESS;
}

/*
 * Fill Ethernet packets
 *
 * @devinfo [in]: doca devinfo
 * @buf [in]: allocated buffer
 * @packet_size [in]: packet size
 * @packet_count [in]: packet count
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t fill_ethernet_packets(struct doca_devinfo *devinfo,
					  void *buf,
					  uint32_t packet_size,
					  uint32_t packet_count)
{
	doca_error_t status;
	uint8_t src_mac_addr[DOCA_DEVINFO_MAC_ADDR_SIZE];
	uint8_t dst_mac_addr[DOCA_DEVINFO_MAC_ADDR_SIZE];
	struct ether_hdr *eth_hdr;

	status = doca_devinfo_get_mac_addr(devinfo, src_mac_addr, DOCA_DEVINFO_MAC_ADDR_SIZE);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get source MAC address: %s", doca_error_get_descr(status));
		return status;
	}

	sscanf(DST_MAC_ADDR,
	       "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
	       &dst_mac_addr[0],
	       &dst_mac_addr[1],
	       &dst_mac_addr[2],
	       &dst_mac_addr[3],
	       &dst_mac_addr[4],
	       &dst_mac_addr[5]);

	for (uint32_t i = 0; i < packet_count; i++) {
		eth_hdr = (struct ether_hdr *)buf;
		memcpy(&(eth_hdr->src_addr), src_mac_addr, DOCA_DEVINFO_MAC_ADDR_SIZE);
		memcpy(&(eth_hdr->dst_addr), dst_mac_addr, DOCA_DEVINFO_MAC_ADDR_SIZE);
		eth_hdr->ether_type = htobe16(UNKNOWN_ETHER_TYPE_IPV4);
		memset((uint8_t *)buf + sizeof(struct ether_hdr), i, packet_size - sizeof(struct ether_hdr));
		buf += packet_size;
	}

	return DOCA_SUCCESS;
}

/*
 * Initialize flow
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t init_flow(void)
{
	doca_error_t result, tmp_result;
	struct doca_flow_cfg *flow_cfg;

	result = doca_flow_cfg_create(&flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_cfg, err: %s", doca_error_get_name(result));
		return result;
	}
	result = doca_flow_cfg_set_pipe_queues(flow_cfg, 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set pipe_queues, err: %s", doca_error_get_name(result));
		goto destroy_cfg;
	}
	result = doca_flow_cfg_set_mode_args(flow_cfg, "vnf");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mode_args, err: %s", doca_error_get_name(result));
		goto destroy_cfg;
	}
	result = doca_flow_cfg_set_nr_counters(flow_cfg, (1 << 19));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set nr_counters, err: %s", doca_error_get_name(result));
		goto destroy_cfg;
	}

	result = doca_flow_init(flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init doca flow, err: %s", doca_error_get_name(result));
		goto destroy_cfg;
	}

destroy_cfg:
	tmp_result = doca_flow_cfg_destroy(flow_cfg);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca_flow_cfg, err: %s", doca_error_get_name(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

/*
 * Destroy flow
 */
void destroy_flow(void)
{
	doca_flow_destroy();
}

/*
 * Create DOCA Flow port with desired port ID
 *
 * @dev [in]: The doca device
 * @port_id [in]: The port ID
 * @df_port [out]: DOCA Flow port to create
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_flow_port(struct doca_dev *dev, uint16_t port_id, struct doca_flow_port **df_port)
{
	doca_error_t status, tmp_result;
	struct doca_flow_port_cfg *port_cfg;

	status = doca_flow_port_cfg_create(&port_cfg);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_port_cfg, err: %s", doca_error_get_name(status));
		return status;
	}

	status = doca_flow_port_cfg_set_port_id(port_cfg, port_id);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg port ID, err: %s", doca_error_get_name(status));
		goto destroy_port_cfg;
	}

	status = doca_flow_port_cfg_set_dev(port_cfg, dev);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_port_cfg dev, err: %s", doca_error_get_name(status));
		goto destroy_port_cfg;
	}

	status = doca_flow_port_start(port_cfg, df_port);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca flow, err: %s", doca_error_get_name(status));
		goto destroy_port_cfg;
	}

destroy_port_cfg:
	tmp_result = doca_flow_port_cfg_destroy(port_cfg);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set start doca_flow port: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(status, tmp_result);
	}
	return status;
}

/*
 * Destroy DOCA Flow port
 *
 * @df_port [in]: The DOCA Flow port to destroy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_flow_port(struct doca_flow_port *df_port)
{
	doca_error_t status = DOCA_SUCCESS;

	if (df_port != NULL) {
		status = doca_flow_port_stop(df_port);
		if (status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop DOCA flow port, err: %s", doca_error_get_name(status));
			return status;
		}
	}

	return status;
}

/*
 * Get DPA handles (verbs eth sq and completion)
 *
 * @dpa_ctx [in]: DPA context
 * @verbs_eth_sq [in]: Verbs ETH SQ
 * @dpa_completion [in]: DPA completion
 * @verbs_eth_sq_handle [out]: Verbs ETH SQ handle
 * @dpa_completion_handle [out]: DPA completion handle
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_dpa_handles(struct doca_dpa *dpa_ctx,
				    struct doca_verbs_eth_sq *verbs_eth_sq,
				    struct doca_dpa_completion *dpa_completion,
				    doca_dpa_dev_verbs_eth_sq_t *verbs_eth_sq_handle,
				    doca_dpa_dev_completion_t *dpa_completion_handle)
{
	doca_error_t status;
	status = doca_verbs_eth_sq_get_dpa_handle(verbs_eth_sq, dpa_ctx, verbs_eth_sq_handle);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs ETH SQ handle: %s", doca_error_get_descr(status));
		return status;
	}
	status = doca_dpa_completion_get_dpa_handle(dpa_completion, dpa_completion_handle);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DPA completion handle: %s", doca_error_get_descr(status));
		return status;
	}

	return DOCA_SUCCESS;
}

/*
 * Create sample resources
 *
 * @resources [out]: sample resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_sample_resources(const char *ib_device_name, struct verbs_eth_sq_sample_resources *resources)
{
	doca_error_t status, tmp_status;
	uint8_t supported_ts_source_type = 0;

	/* Initialize flow */
	status = init_flow();
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize flow: %s", doca_error_get_descr(status));
		return status;
	}

	/* Create Verbs Context */
	status = create_verbs_context(ib_device_name, &resources->verbs_context, &supported_ts_source_type);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs Context: %s", doca_error_get_descr(status));
		goto _destroy_flow;
	}

	/* Create DPA context with DOCA dev and Verbs PD */
	status = create_dpa_context(resources->verbs_context,
				    &resources->verbs_pd,
				    &resources->dev,
				    &resources->dpa_ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DPA context: %s", doca_error_get_descr(status));
		goto _destroy_verbs_context;
	}

	/* Create DPA completion */
	status = create_dpa_completion(resources->dpa_ctx, PACKETS_NUMBER, &resources->dpa_completion);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DPA completion: %s", doca_error_get_descr(status));
		goto _destroy_dpa_context;
	}

	/* Create Verbs ETH SQ */
	status = create_verbs_eth_sq(resources->verbs_context,
				     resources->verbs_pd,
				     resources->dpa_ctx,
				     resources->dpa_completion,
				     supported_ts_source_type,
				     PACKETS_NUMBER,
				     ETH_SQ_LOGICAL_QUEUE_ID,
				     &resources->verbs_eth_sq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Verbs ETH SQ: %s", doca_error_get_descr(status));
		goto _destroy_dpa_completion;
	}

	/* Create DOCA Flow port */
	status = create_flow_port(resources->dev, FLOW_POART_ID, &resources->df_port);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA flow port: %s", doca_error_get_descr(status));
		goto _destroy_verbs_eth_sq;
	}

	/* Create local memory objects */
	status = create_local_memory_objects(PACKETS_NUMBER * PACKET_SIZE,
					     doca_verbs_bridge_verbs_pd_get_ibv_pd(resources->verbs_pd),
					     &resources->buf,
					     &resources->mr,
					     &resources->mkey);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create local memory objects: %s", doca_error_get_descr(status));
		goto _destroy_flow_port;
	}

	/* Fill Ethernet packets */
	status =
		fill_ethernet_packets(doca_dev_as_devinfo(resources->dev), resources->buf, PACKET_SIZE, PACKETS_NUMBER);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to fill ethernet packet: %s", doca_error_get_descr(status));
		goto _destroy_local_memory_objects;
	}

	/* Get DPA handles */
	status = get_dpa_handles(resources->dpa_ctx,
				 resources->verbs_eth_sq,
				 resources->dpa_completion,
				 &resources->verbs_eth_sq_handle,
				 &resources->dpa_completion_handle);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get DPA handles: %s", doca_error_get_descr(status));
		goto _destroy_local_memory_objects;
	}

	return DOCA_SUCCESS;

_destroy_local_memory_objects:
	tmp_status = destroy_local_memory_objects(resources->mr, resources->buf);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local memory objects: %s", doca_error_get_descr(tmp_status));
	}
_destroy_flow_port:
	tmp_status = destroy_flow_port(resources->df_port);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA flow port: %s", doca_error_get_descr(tmp_status));
	}
_destroy_verbs_eth_sq:
	tmp_status = destroy_verbs_eth_sq(resources->verbs_eth_sq);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy Verbs ETH SQ: %s", doca_error_get_descr(tmp_status));
	}
_destroy_dpa_completion:
	tmp_status = destroy_dpa_completion(resources->dpa_completion);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DPA completion: %s", doca_error_get_descr(tmp_status));
	}
_destroy_dpa_context:
	tmp_status = destroy_dpa_context(resources->verbs_pd, resources->dev, resources->dpa_ctx);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DPA context: %s", doca_error_get_descr(tmp_status));
	}
_destroy_verbs_context:
	tmp_status = destroy_verbs_context(resources->verbs_context);
	if (tmp_status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy Verbs Context: %s", doca_error_get_descr(tmp_status));
	}
_destroy_flow:
	destroy_flow();

	return status;
}

/*
 * Destroy sample resources
 *
 * @resources [in]: sample resources
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static doca_error_t destroy_sample_resources(struct verbs_eth_sq_sample_resources *resources)
{
	doca_error_t status;

	status = destroy_local_memory_objects(resources->mr, resources->buf);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy local memory objects: %s", doca_error_get_descr(status));
		return status;
	}

	status = destroy_flow_port(resources->df_port);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA flow port: %s", doca_error_get_descr(status));
		return status;
	}

	status = destroy_verbs_eth_sq(resources->verbs_eth_sq);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy Verbs ETH SQ: %s", doca_error_get_descr(status));
		return status;
	}

	status = destroy_dpa_completion(resources->dpa_completion);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DPA completion: %s", doca_error_get_descr(status));
		return status;
	}

	status = destroy_dpa_context(resources->verbs_pd, resources->dev, resources->dpa_ctx);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DPA context: %s", doca_error_get_descr(status));
		return status;
	}

	status = destroy_verbs_context(resources->verbs_context);
	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy Verbs Context: %s", doca_error_get_descr(status));
		return status;
	}

	destroy_flow();

	return DOCA_SUCCESS;
}

/*
 * Run verbs send ethernet frames on DPA sample
 *
 * @ib_device_name [in]: IB device name
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t verbs_send_ethernet_frames_on_dpa(const char *ib_device_name)
{
	doca_error_t result, tmp_result;
	struct verbs_eth_sq_sample_resources resources = {0};

	/* Create Sample Resources */
	result = create_sample_resources(ib_device_name, &resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create sample resources: %s", doca_error_get_descr(result));
		return result;
	}

	uint64_t rpc_ret;
	result = doca_dpa_rpc(resources.dpa_ctx,
			      &send_ethernet_frames_rpc,
			      &rpc_ret,
			      resources.verbs_eth_sq_handle,
			      resources.dpa_completion_handle,
			      (doca_dpa_dev_uintptr_t)resources.buf,
			      resources.mkey,
			      PACKETS_NUMBER,
			      PACKET_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send Ethernet frames RPC: %s", doca_error_get_descr(result));
		goto _destroy_sample_resources;
	}

_destroy_sample_resources:
	tmp_result = destroy_sample_resources(&resources);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy sample resources: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}
