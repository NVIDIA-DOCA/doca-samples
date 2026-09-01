/*
 * Copyright (c) 2021-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <sys/queue.h>
#include <unistd.h>
#include <linux/types.h>

#include <rte_string_fns.h>

#include <doca_log.h>

#include "telemetry_exporter.h"
#include "utils.h"

DOCA_LOG_REGISTER(NETFLOW_TELEMETRY);

/* Queues to hold the telemetry data until sent */
static struct rte_ring *netflow_pending_ring, *netflow_freelist_ring;

/* Netflow records queues */
static struct doca_telemetry_exporter_netflow_record data_to_send[NETFLOW_QUEUE_SIZE];
static struct doca_telemetry_exporter_netflow_record *data_to_send_ptr[NETFLOW_QUEUE_SIZE];

static struct doca_telemetry_exporter_netflow_template *netflow_template; /* Netflow template */

/*
 * Add new Netflow field to the Netflow template
 *
 * @type [in]: Netflow field type
 * @length [in]: Netflow field length
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

	result = doca_telemetry_exporter_netflow_template_add_field(netflow_template, field);
	if (result != DOCA_SUCCESS)
		doca_telemetry_exporter_netflow_field_destroy(field);
	return result;
}

/*
 * Initialize Netflow template by adding all fields
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
 * Get host name to be tagged with the telemetry data
 *
 * @host_name [out]: host name
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_hostname_ip(char *host_name)
{
	doca_error_t result = DOCA_SUCCESS;
	char host[256];
	struct hostent *host_entry = NULL;

	/* Find the host name */
	if (gethostname(host, sizeof(host)) < 0) {
		switch (errno) {
		case EPERM:
			result = DOCA_ERROR_NOT_PERMITTED;
			break;
		default:
			result = DOCA_ERROR_INVALID_VALUE;
			break;
		}
		DOCA_LOG_ERR("Gethostname failed");
		return result;
	}
	/* Find host information */
	host_entry = gethostbyname(host);
	if (host_entry == NULL)
		strlcpy(host_name, host, 64);
	else
		strlcpy(host_name, host_entry->h_name, 64);

	DOCA_LOG_TRC("Host name: %s", host_name);
	return result;
}

doca_error_t send_netflow_record(void)
{
	doca_error_t result;
	size_t records_to_send = 0;
	size_t records_sent = 0;
	size_t records_successfully_sent;
	int ring_count = rte_ring_count(netflow_pending_ring);
	static struct doca_telemetry_exporter_netflow_record *records[NETFLOW_QUEUE_SIZE];
	/*
	 * Sending the record array
	 * The while loop ensure that all records have been sent, in case just some are sent.
	 * This section should happen periodically with updated the flows.
	 */
	if (ring_count == 0)
		return DOCA_SUCCESS;
	/* We need to dequeue only the records that were enqueued with the allocated memory */
	records_to_send = rte_ring_dequeue_bulk(netflow_pending_ring, (void **)records, ring_count, NULL);
	while (records_sent < records_to_send) {
		result = doca_telemetry_exporter_netflow_send(netflow_template,
							      (const void **)(records + records_sent),
							      records_to_send - records_sent,
							      &records_successfully_sent);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to send Netflow: %s", doca_error_get_name(result));
			return result;
		}
		records_sent += records_successfully_sent;
	}
	/* Flushing the buffer sends it to the collector */
	doca_telemetry_exporter_netflow_flush();
	DOCA_LOG_TRC("Successfully sent %lu netflow records with default template", records_sent);
	if ((size_t)rte_ring_enqueue_bulk(netflow_freelist_ring, (void **)records, records_sent, NULL) !=
	    records_sent) {
		DOCA_LOG_ERR("Placeholder queue mismatch");
		return DOCA_ERROR_DRIVER;
	}
	return DOCA_SUCCESS;
}

void enqueue_netflow_record_to_ring(const struct doca_telemetry_exporter_netflow_record *record)
{
	struct doca_telemetry_exporter_netflow_record *tmp_record;
	/* To avoid memory corruption when flows are destroyed, we copy the pointers to a
	 *	preallocated pointer inside freelist ring and enqueue it so the main thread
	 *	can send them.
	 */
	if (rte_ring_mc_dequeue(netflow_freelist_ring, (void **)&tmp_record) != 0) {
		DOCA_LOG_DBG("Placeholder queue is empty");
		return;
	}
	*tmp_record = *record;
	if (rte_ring_mp_enqueue(netflow_pending_ring, tmp_record) != 0) {
		DOCA_LOG_DBG("Netflow queue is full");
		return;
	}
}

void destroy_netflow_schema_and_source(void)
{
	rte_ring_free(netflow_pending_ring);
	rte_ring_free(netflow_freelist_ring);

	doca_telemetry_exporter_netflow_destroy();
	doca_telemetry_exporter_netflow_template_destroy(netflow_template);
}

doca_error_t init_netflow_schema_and_source(uint8_t id, char *source_tag)
{
	doca_error_t result;
	int i;
	char hostname[64];
	char *bluefield_rshim = "192.168.100.1";

	result = doca_telemetry_exporter_netflow_template_create(&netflow_template);
	if (result != DOCA_SUCCESS)
		return result;

	result = init_template_fields();
	if (result != DOCA_SUCCESS)
		goto destroy_template;

	result = doca_telemetry_exporter_netflow_init(id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot init DOCA netflow");
		goto destroy_template;
	}

	doca_telemetry_exporter_netflow_set_ipc_enabled();

	/* Setting the Netflow collector is recommended for debugging only - use DTS otherwise,
	 * Default PORT is 2055
	 */
	doca_telemetry_exporter_netflow_set_collector_addr(bluefield_rshim);

	result = get_hostname_ip(hostname);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Getting hostname failed");
		goto destroy_netflow;
	}

	doca_telemetry_exporter_netflow_source_set_id(hostname);
	doca_telemetry_exporter_netflow_source_set_tag(source_tag);

	result = doca_telemetry_exporter_netflow_start();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot start DOCA netflow");
		goto destroy_netflow;
	}
	/* In the Netflow ring scenario, a producer-consumer solution is given where the dpi_worker threads produce
	 * records and enqueues them to a rings struct. The records are consumed by the main thread that dequeues
	 * the records and sends them. This allows avoiding collisions between thread and memory corruption issues.
	 */
	netflow_pending_ring = rte_ring_create("netflow_queue", NETFLOW_QUEUE_SIZE, SOCKET_ID_ANY, RING_F_SC_DEQ);
	netflow_freelist_ring =
		rte_ring_create("placeholder_netflow_queue", NETFLOW_QUEUE_SIZE, SOCKET_ID_ANY, RING_F_SP_ENQ);
	if (netflow_pending_ring == NULL || netflow_freelist_ring == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_netflow;
	}
	for (i = 0; i < NETFLOW_QUEUE_SIZE; i++)
		data_to_send_ptr[i] = &data_to_send[i];
	if (rte_ring_enqueue_bulk(netflow_freelist_ring, (void **)data_to_send_ptr, NETFLOW_QUEUE_SIZE - 1, NULL) !=
	    NETFLOW_QUEUE_SIZE - 1) {
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_netflow;
	}

	return DOCA_SUCCESS;

destroy_netflow:
	doca_telemetry_exporter_netflow_destroy();
destroy_template:
	doca_telemetry_exporter_netflow_template_destroy(netflow_template);
	return result;
}
