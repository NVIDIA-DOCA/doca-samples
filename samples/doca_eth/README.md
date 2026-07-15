# Samples

> **Note:** The following samples are for the CPU datapath. For GPU datapath samples, see [DOCA GPUNetIO](https://docs.nvidia.com/doca/archive/2-9-0/DOCA+GPUNetIO).

## ETH TXQ Send Ethernet Frames

This sample illustrates how to send a "regular" packet (smaller than MTU) using DOCA ETH TXQ.

The sample logic includes:

- Locating DOCA device.
- Initializing the required DOCA Core structures.
- Populating DOCA memory map with one buffer to the packet's data.
- Writing the packet's content into the allocated buffer.
- Allocating elements from DOCA buffer inventory for the buffer.
- Initializing and configuring DOCA ETH TXQ context.
- Starting the DOCA ETH TXQ context.
- Allocating DOCA ETH TXQ send task.
- Submitting DOCA ETH TXQ send task into progress engine.
- Retrieving DOCA ETH TXQ send task from the progress engine.
- Handling the completed task using the provided callback.
- Stopping the DOCA ETH TXQ context.
- Destroying DOCA ETH TXQ context.
- Destroying all DOCA Core structures.

**Reference:**

- `eth_txq_send_ethernet_frames/eth_txq_send_ethernet_frames_sample.c`
- `eth_txq_send_ethernet_frames/eth_txq_send_ethernet_frames_main.c`
- `eth_txq_send_ethernet_frames/meson.build`

## ETH TXQ LSO Send Ethernet Frames

This sample illustrates how to send a "large" packet (larger than MTU) using DOCA ETH TXQ.

The sample logic includes:

- Locating DOCA device.
- Initializing the required DOCA Core structures.
- Populating DOCA memory map with one buffer to the packet's payload.
- Writing the packet's payload into the allocated buffer.
- Allocating elements from DOCA Buffer inventory for the buffer.
- Allocating DOCA gather list consisting of one node to the packet's headers.
- Writing the packet's headers into the allocated gather list node.
- Initializing and configuring DOCA ETH TXQ context.
- Starting the DOCA ETH TXQ context.
- Allocating DOCA ETH TXQ LSO send task.
- Submitting DOCA ETH TXQ LSO send task into progress engine.
- Retrieving DOCA ETH TXQ LSO send task from the progress engine.
- Handling the completed task using the provided callback.
- Stopping the DOCA ETH TXQ context.
- Destroying DOCA ETH TXQ context.
- Destroying all DOCA Core structures.

**Reference:**

- `eth_txq_lso_send_ethernet_frames/eth_txq_lso_send_ethernet_frames_sample.c`
- `eth_txq_lso_send_ethernet_frames/eth_txq_lso_send_ethernet_frames_main.c`
- `eth_txq_lso_send_ethernet_frames/meson.build`

## ETH TXQ Batch Send Ethernet Frames

This sample illustrates how to send a batch of "regular" packets (smaller than MTU) using DOCA ETH TXQ.

The sample logic includes:

- Locating DOCA device.
- Initializing the required DOCA Core structures.
- Populating DOCA memory map with multiple buffers, each representing a packet's data.
- Writing the packets' content into the allocated buffers.
- Allocating elements from DOCA Buffer inventory for the buffers.
- Initializing and configuring DOCA ETH TXQ context.
- Starting the DOCA ETH TXQ context.
- Allocating DOCA ETH TXQ send task batch.
- Copying all buffers' pointers to task batch's pkt_arry.
- Submitting DOCA ETH TXQ send task batch into the progress engine.
- Retrieving DOCA ETH TXQ send task batch from the progress engine.
- Handling the completed task batch using the provided callback.
- Stopping the DOCA ETH TXQ context.
- Destroying DOCA ETH TXQ context.
- Destroying all DOCA Core structures.

**Reference:**

- `eth_txq_batch_send_ethernet_frames/eth_txq_batch_send_ethernet_frames_sample.c`
- `eth_txq_batch_send_ethernet_frames/eth_txq_batch_send_ethernet_frames_main.c`
- `eth_txq_batch_send_ethernet_frames/meson.build`

## ETH TXQ Batch LSO Send Ethernet Frames

This sample illustrates how to send a batch of "large" packets (larger than MTU) using DOCA ETH TXQ.

The sample logic includes:

- Locating DOCA device.
- Initializing the required DOCA Core structures.
- Populating DOCA memory map with multiple buffers, each representing a packet's payload.
- Writing the packets' payload into the allocated buffers.
- Allocating elements from DOCA Buffer inventory for the buffers.
- Allocating DOCA gather lists each consisting of one node for the packet's headers.
- Writing the packets' headers into the allocated gather list nodes.
- Initializing and configuring DOCA ETH TXQ context.
- Starting the DOCA ETH TXQ context.
- Allocating DOCA ETH TXQ LSO send task.
- Copying all buffers' pointers to task batch's pkt_payload_arry.
- Copying all gather lists' pointers to task batch's headers_arry.
- Submitting DOCA ETH TXQ LSO send task batch into the progress engine.
- Retrieving DOCA ETH TXQ LSO send task batch from the progress engine.
- Handling the completed task batch using the provided callback.
- Stopping the DOCA ETH TXQ context.
- Destroying DOCA ETH TXQ context.
- Destroying all DOCA Core structures.

**Reference:**

- `eth_txq_batch_lso_send_ethernet_frames/eth_txq_batch_lso_send_ethernet_frames_sample.c`
- `eth_txq_batch_lso_send_ethernet_frames/eth_txq_batch_lso_send_ethernet_frames_main.c`
- `eth_txq_batch_lso_send_ethernet_frames/meson.build`

## ETH RXQ Regular Receive

This sample illustrates how to receive a packet using DOCA ETH RXQ in Regular Receive mode.

The sample logic includes:

- Locating DOCA device.
- Initializing the required DOCA Core structures.
- Populating DOCA memory map with one buffer to the packet's data.
- Allocating element from DOCA Buffer inventory for each buffer.
- Initializing DOCA Flow.
- Initializing and configuring DOCA ETH RXQ context.
- Starting the DOCA ETH RXQ context.
- Starting DOCA Flow.
- Creating a pipe connecting to DOCA ETH RXQ's RX queue.
- Allocating DOCA ETH RXQ receive task.
- Submitting DOCA ETH RXQ receive task into the progress engine.
- Retrieving DOCA ETH RXQ receive task from the progress engine.
- Handling the completed task using the provided callback.
- Stopping DOCA Flow.
- Stopping the DOCA ETH RXQ context.
- Destroying DOCA ETH RXQ context.
- Destroying DOCA Flow.
- Destroying all DOCA Core structures.

**Reference:**

- `eth_rxq_regular_receive/eth_rxq_regular_receive_sample.c`
- `eth_rxq_regular_receive/eth_rxq_regular_receive_main.c`
- `eth_rxq_regular_receive/meson.build`

## ETH RXQ Managed Receive

This sample illustrates how to receive packets using DOCA ETH RXQ in Managed Memory Pool Receive mode.

The sample logic includes:

- Locating DOCA device.
- Initializing the required DOCA Core structures.
- Calculating the required size of the buffer to receive the packets from DOCA ETH RXQ.
- Populating DOCA memory map with a packets buffer.
- Initializing DOCA Flow.
- Initializing and configuring DOCA ETH RXQ context.
- Registering DOCA ETH RXQ managed receive event.
- Starting the DOCA ETH RXQ context.
- Starting DOCA Flow.
- Creating a pipe connecting to DOCA ETH RXQ's RX queue.
- Retrieving DOCA ETH RXQ managed receive events from the progress engine.
- Handling the completed events using the provided callback.
- Stopping DOCA Flow.
- Stopping the DOCA ETH RXQ context.
- Destroying DOCA ETH RXQ context.
- Destroying DOCA Flow.
- Destroying all DOCA Core structures.

**Reference:**

- `eth_rxq_managed_mempool_receive/eth_rxq_managed_mempool_receive_sample.c`
- `eth_rxq_managed_mempool_receive/eth_rxq_managed_mempool_receive_main.c`
- `eth_rxq_managed_mempool_receive/meson.build`

## ETH RXQ Batch Managed Receive

This sample illustrates how to receive batches of packets using DOCA ETH RXQ in Managed Memory Pool Receive mode.

The sample logic includes:

- Locating DOCA device.
- Initializing the required DOCA Core structures.
- Calculating the required size of the buffer to receive the packets from DOCA ETH RXQ.
- Populating DOCA memory map with a packets buffer.
- Initializing DOCA Flow.
- Initializing and configuring DOCA ETH RXQ context.
- Registering DOCA ETH RXQ managed receive event batch.
- Starting the DOCA ETH RXQ context.
- Starting DOCA Flow.
- Creating a pipe connecting to DOCA ETH RXQ's RX queue.
- Retrieving DOCA ETH RXQ managed receive event batches from the progress engine.
- Handling the completed event batches using the provided callback.
- Stopping DOCA Flow.
- Stopping the DOCA ETH RXQ context.
- Destroying DOCA ETH RXQ context.
- Destroying DOCA Flow.
- Destroying all DOCA Core structures.

**Reference:**

- `eth_rxq_batch_managed_mempool_receive/eth_rxq_batch_managed_mempool_receive_sample.c`
- `eth_rxq_batch_managed_mempool_receive/eth_rxq_batch_managed_mempool_receive_main.c`
- `eth_rxq_batch_managed_mempool_receive/meson.build`
```
