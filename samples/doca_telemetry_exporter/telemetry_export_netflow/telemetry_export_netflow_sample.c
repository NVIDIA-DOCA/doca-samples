/*
 * Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <linux/types.h>

#include <doca_log.h>
#include <doca_telemetry_exporter_netflow.h>

DOCA_LOG_REGISTER(TELEMETRY_EXPORTER::NETFLOW);

#define DOCA_TELEMETRY_EXPORTER_NETFLOW_EXAMPLE_SOURCE_ID 111	      /* Source ID for DOCA Telemetry Netflow example */
#define DOCA_TELEMETRY_EXPORTER_NETFLOW_EXAMPLE_EVENTS_BATCH_SIZE 100 /* Number of records in a single batch */
#define DOCA_TELEMETRY_EXPORTER_NETFLOW_EXAMPLE_EVENTS_NOF_BATCHES 10 /* Number of batches to send */

/* DOCA Telemetry Netflow example struct */
struct doca_telemetry_exporter_netflow_example_record {
	__be32 src_addr_v4;	     /* Source IPV4 Address */
	__be32 dst_addr_v4;	     /* Destination IPV4 Address */
	struct in6_addr src_addr_v6; /* Source IPV6 Address */
	struct in6_addr dst_addr_v6; /* Destination IPV6 Address */
	__be32 next_hop_v4;	     /* Next hop router's IPV4 Address */
	struct in6_addr next_hop_v6; /* Next hop router's IPV6 Address */
	__be16 input;		     /* Input interface index */
	__be16 output;		     /* Output interface index */
	__be16 src_port;	     /* TCP/UDP source port number or equivalent */
	__be16 dst_port;	     /* TCP/UDP destination port number or equivalent */
	uint8_t tcp_flags;	     /* Cumulative OR of tcp flags */
	uint8_t protocol;	     /* IP protocol type (for example, TCP = 6;UDP = 17) */
	uint8_t tos;		     /* IP Type-of-Service */
	__be16 src_as;		     /* Originating AS of source address */
	__be16 dst_as;		     /* Originating AS of destination address */
	uint8_t src_mask;	     /* Source address prefix mask bits */
	uint8_t dst_mask;	     /* Destination address prefix mask bits */
	__be32 d_pkts;		     /* Packets sent in Duration */
	__be32 d_octets;	     /* Octets sent in Duration */
	__be32 first;		     /* SysUptime at start of flow */
	__be32 last;		     /* And of last packet of flow */
	__be64 flow_id;		     /* This identifies a transaction within a connection */
	char application_name[DOCA_TELEMETRY_EXPORTER_NETFLOW_APPLICATION_NAME_DEFAULT_LENGTH];
	/* Name associated with a classification*/
} __attribute__((packed));

/* Template for DOCA Telemetry Netflow Example */
static struct doca_telemetry_exporter_netflow_template *example_template;

/*
 * Adds a new field - of type and length given as input - to the global example_template struct.
 *
 * @type [in]: Type of field
 * @length [in]: Length of field
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_netflow_field(uint16_t type, uint16_t length)
{
	struct doca_telemetry_exporter_netflow_flowset_field *field;
	doca_error_t result;

	result = doca_telemetry_exporter_netflow_field_create(&field);
	if (result != DOCA_SUCCESS)
		return result;
	doca_telemetry_exporter_netflow_field_set_type(field, type);
	doca_telemetry_exporter_netflow_field_set_len(field, length);

	result = doca_telemetry_exporter_netflow_template_add_field(example_template, field);
	if (result != DOCA_SUCCESS)
		doca_telemetry_exporter_netflow_field_destroy(field);

	return result;
}

/*
 * Adds all template fields to the global example_template struct.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_template_fields(void)
{
	doca_error_t result = DOCA_SUCCESS;

	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV4_SRC_ADDR,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV4_SRC_ADDR_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV4_DST_ADDR,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV4_DST_ADDR_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV6_SRC_ADDR,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV6_SRC_ADDR_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV6_DST_ADDR,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV6_DST_ADDR_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV4_NEXT_HOP,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV4_NEXT_HOP_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV6_NEXT_HOP,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_IPV6_NEXT_HOP_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_INPUT_SNMP,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_INPUT_SNMP_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_OUTPUT_SNMP,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_OUTPUT_SNMP_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_L4_SRC_PORT,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_L4_SRC_PORT_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_L4_DST_PORT,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_L4_DST_PORT_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_TCP_FLAGS,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_TCP_FLAGS_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_PROTOCOL,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_PROTOCOL_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_SRC_TOS,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_SRC_TOS_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_SRC_AS,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_SRC_AS_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_DST_AS,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_DST_AS_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_SRC_MASK,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_SRC_MASK_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_DST_MASK,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_DST_MASK_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_IN_PKTS,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_IN_PKTS_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_IN_BYTES,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_IN_BYTES_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_FIRST_SWITCHED,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_FIRST_SWITCHED_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_LAST_SWITCHED,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_LAST_SWITCHED_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_CONNECTION_TRANSACTION_ID,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_CONNECTION_TRANSACTION_ID_DEFAULT_LENGTH);
	result |= add_netflow_field(DOCA_TELEMETRY_EXPORTER_NETFLOW_APPLICATION_NAME,
				    DOCA_TELEMETRY_EXPORTER_NETFLOW_APPLICATION_NAME_DEFAULT_LENGTH);
	if (result != DOCA_SUCCESS)
		return DOCA_ERROR_NO_MEMORY;

	return DOCA_SUCCESS;
}

/*
 * Fills a DOCA Telemetry Netflow example record with arbitrary info.
 *
 * @record [out]: The record getting filled
 */
static void prepare_netflow_example_record(struct doca_telemetry_exporter_netflow_example_record *record)
{
	int ret;

	record->src_addr_v4 = inet_addr("192.168.120.1"); /* Source IPV4 Address */
	record->dst_addr_v4 = inet_addr("192.168.120.2"); /* Destination IPV4 Address */

	ret = inet_pton(AF_INET6, "0:0:0:0:0:FFFF:C0A8:7801", &record->src_addr_v6); /* Source IPV6 Address */
	if (ret != 1)
		DOCA_LOG_ERR("Failed to convert source IPv6 address, ret=%d", ret);

	ret = inet_pton(AF_INET6, "0:0:0:0:0:FFFF:C0A8:7802", &record->dst_addr_v6); /* Destination IPV6 Address */
	if (ret != 1)
		DOCA_LOG_ERR("Failed to convert destination IPv6 address, ret=%d", ret);

	record->next_hop_v4 = inet_addr("192.168.133.7"); /* Next hop router's IPV4 Address */

	ret = inet_pton(AF_INET6, "0:0:0:0:0:FFFF:C0A8:8507", &record->next_hop_v6); /* Next hop router's IPV6 Address
										      */
	if (ret != 1)
		DOCA_LOG_ERR("Failed to convert next hop IPv6 address, ret=%d", ret);

	record->input = htobe16(1);	  /* Input interface index */
	record->output = htobe16(65535);  /* Output interface index */
	record->src_port = htobe16(5353); /* TCP/UDP source port number or equivalent */
	record->dst_port = htobe16(8000); /* TCP/UDP destination port number or equivalent */
	record->tcp_flags = 0;		  /* Cumulative OR of tcp flags */
	record->protocol = 17;		  /* IP protocol type (for example, TCP  = 6 = , UDP  = 17) */
	record->tos = 0;		  /* IP Type-of-Service */
	record->src_as = htobe16(0);	  /* originating AS of source address */
	record->dst_as = htobe16(0);	  /* originating AS of destination address */
	record->src_mask = 0;		  /* source address prefix mask bits */
	record->dst_mask = 0;		  /* destination address prefix mask bits */
	record->d_pkts = htobe32(9);	  /* Packets sent in Duration */
	record->d_octets = htobe32(1909); /* Octets sent in Duration */
	record->first = htobe32(800294);  /* SysUptime at start of flow */
	record->last = htobe32(804839);	  /* and of last packet of flow */
	record->flow_id = htobe64(1337);  /* This identifies a transaction within a connection */
	strcpy(record->application_name, "DOCA TELEMETRY NETFLOW EXAMPLE"); /* Name associated with a classification */
}

/*
 * Main sample function.
 * Initializes and starts DOCA Telemetry Netflow, and sends batches of records.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t telemetry_netflow_export(void)
{
	bool file_write_enable = true; /* Enables writing to local machine */
	bool ipc_enabled = true;       /* Enables sending to DTS through ipc sockets */
	doca_error_t result;
	int i;
	size_t nb_of_records_sent;
	struct doca_telemetry_exporter_netflow_example_record
		*records[DOCA_TELEMETRY_EXPORTER_NETFLOW_EXAMPLE_EVENTS_BATCH_SIZE];
	struct doca_telemetry_exporter_netflow_example_record record;

	/* Address and port for DOCA Telemetry Netflow exportation */
	char *netflow_addr = "127.0.0.1";
	uint16_t netflow_port = 9996;

	/* Set attributes and initialize DOCA Telemetry Netflow */

	/* Create example template */
	result = doca_telemetry_exporter_netflow_template_create(&example_template);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Error creating DOCA Telemetry Netflow template %d", result);
		return result;
	}

	result = init_template_fields();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Error initializing DOCA Telemetry Netflow template fields %d", result);
		goto fields_init_err;
	}

	/* Init DOCA Telemetry Netflow */
	result = doca_telemetry_exporter_netflow_init(DOCA_TELEMETRY_EXPORTER_NETFLOW_EXAMPLE_SOURCE_ID);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("DOCA Netflow init failed with error %d", result);
		goto netflow_init_err;
	}

	/*
	 * Enable file write during the app development.
	 * Check written files under data root to make sure that data format is correct.
	 * Default max_file_size is 1 Mb, default max_file_age is 1 hour.
	 */
	if (file_write_enable)
		doca_telemetry_exporter_netflow_set_file_write_enabled();

	/*
	 * If IPC is enabled, DOCA Telemetry Netflow will try to find DOCA Telemetry Service (DTS)
	 * socket under ipc_sockets_dir. IPC is disabled by default.
	 */
	if (ipc_enabled)
		doca_telemetry_exporter_netflow_set_ipc_enabled();

	doca_telemetry_exporter_netflow_set_collector_addr(netflow_addr);
	doca_telemetry_exporter_netflow_set_collector_port(netflow_port);

	doca_telemetry_exporter_netflow_source_set_id("source_1");
	doca_telemetry_exporter_netflow_source_set_tag("source_1_tag");

	/* Start Netflow */
	result = doca_telemetry_exporter_netflow_start();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("DOCA Telemetry Netflow start failed with error %d", result);
		doca_telemetry_exporter_netflow_destroy();
		goto netflow_start_err;
	}

	/* Report batches of netflow records */
	prepare_netflow_example_record(&record);

	for (i = 0; i < DOCA_TELEMETRY_EXPORTER_NETFLOW_EXAMPLE_EVENTS_BATCH_SIZE; i++)
		records[i] = &record;

	for (i = 0; i < DOCA_TELEMETRY_EXPORTER_NETFLOW_EXAMPLE_EVENTS_NOF_BATCHES; i++) {
		nb_of_records_sent = 0;

		result = doca_telemetry_exporter_netflow_send(example_template,
							      (const void **)&records,
							      DOCA_TELEMETRY_EXPORTER_NETFLOW_EXAMPLE_EVENTS_BATCH_SIZE,
							      &nb_of_records_sent);
		if ((result != DOCA_SUCCESS) ||
		    (nb_of_records_sent != DOCA_TELEMETRY_EXPORTER_NETFLOW_EXAMPLE_EVENTS_BATCH_SIZE)) {
			DOCA_LOG_ERR("Batch#%d: %zu out of %d records sent (err=%d)",
				     i,
				     nb_of_records_sent,
				     DOCA_TELEMETRY_EXPORTER_NETFLOW_EXAMPLE_EVENTS_BATCH_SIZE,
				     result);
			doca_telemetry_exporter_netflow_destroy();
			result = result != DOCA_SUCCESS ? result : DOCA_ERROR_DRIVER;
			goto netflow_send_err;
		}
		DOCA_LOG_INFO("Batch#%d: %zu records sent", i, nb_of_records_sent);
	}

	/* Destroy Netflow to clean up */
	doca_telemetry_exporter_netflow_destroy();
	doca_telemetry_exporter_netflow_template_destroy(example_template);
	return DOCA_SUCCESS;

netflow_send_err:
netflow_start_err:
	doca_telemetry_exporter_netflow_destroy();
netflow_init_err:
fields_init_err:
	doca_telemetry_exporter_netflow_template_destroy(example_template);
	return result;
}
