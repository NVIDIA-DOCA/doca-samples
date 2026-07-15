# DOCA RDMA

Each sample presents a connection between two peers, transferring data from one to another, using a different RDMA operation in each sample. For more information on the available RDMA operations, refer to section  ["Tasks"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-Tasks).

Each sample is comprised of two executables, each running on a peer.

The samples can run on either DPU or host, as long as the chosen peers have a connection between them.

> **Note:** Prior to running the samples, ensure that the chosen devices, selected by the device name and the GID index, are set correctly and have a connection between one another. In each sample, it is the user's responsibility to copy the descriptors between the peers.

## General Sample Steps

Most of the samples follow the following main basic steps:

### Allocating Resources

1. Locating and opening a device. The chosen device is one that supports the tasks relevant for the sample. If the sample requires no task, any device may be chosen.
2. Creating a local MMAP and configuring it (including setting the MMAP memory range and relevant permissions).
3. Creating a DOCA PE.
4. Creating an RDMA instance and configuring it (including setting the relevant permissions).
5. Connecting the RDMA context to the PE.

### Sample-Specific Configurations

1. Configuring the tasks relevant to the sample, if any. Including:
   - Setting the number of tasks for each task type.
   - Setting callback functions for each task type, with the following logic:
     - **Successful completion callback:**
       - Verifying the data received from the remote, if any, is valid.
       - Printing the transferred data.
       - Freeing the task and task-specific resources (such as source/destination buffers).
       - If an error occurs in steps a. and b., update the error that was encountered.
     - **Failed completion callback:**
       - Update the error that was encountered.
       - Freeing the task and task-specific resources (such as source/destination buffers).
   - Decreasing the number of remaining tasks and stopping the context once it reaches 0.
2. Setting a state change callback function, with the following logic:
   - Once the context moves to Starting state (can only be reached from Idle), export and connect the RDMA and, in some samples, export the local mmap or the sync event.
   - Once the context moves to Running state (can only be reached from Starting state in RDMA samples):
     - In some samples, only print a log and wait for the peer, or synchronize events.
     - In other samples, prepare and submit a task:
       - If needed, create an mmap from the received exported mmap descriptor, passed from the peer.
       - Request the required buffers from the buffer inventory.
       - Allocate and initiate the required task, together with setting the number of remaining tasks parameter as the task's user data.
       - Submit the task.
   - Once the context moves to Stopping state, print a relevant log.
   - Once the context moves to Idle state:
     - Print a relevant log.
     - Send update that the main loop may be stopped.
3. Setting the program's resources as the context user data to be used in callbacks.
4. Creating a buffer inventory and starting it.
5. Starting the context.

> **Info:** After starting the context, the state change callback function is called by the PE which executes the relevant steps.

> **Info:** In a successful run, each section is executed in the order they are presented in section 2.b.

### Progressing the PE

Progressing the PE until the context returns to Idle state and the main loop may be stopped, either because of a run in which all tasks have been completed, or due to a fatal error.

### Cleaning Up the Resources

## RDMA Read

### RDMA Read Requester

This sample illustrates how to read from a remote peer (the responder) using DOCA RDMA.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for the local mmap in this sample are set to local read and write.
- A read task is configured for this sample.
- In this sample, data is read from the peer, verified to be valid, and printed in the successful task completion callback.
- The local mmap is not exported as the peer does not intend to access it.
- To read from the peer, a remote mmap is created from the peer's exported mmap.

**Reference:**

- ` rdma_read_requester/rdma_read_requester_sample.c`
- ` rdma_read_requester/rdma_read_requester_main.c`
- ` rdma_read_requester/meson.build`

### RDMA Read Responder

This sample illustrates how to set up a remote peer for a DOCA RDMA read request.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for both the local mmap and the RDMA instance in this sample allow for RDMA read.
- No tasks are configured for this sample, and thus no tasks are prepared and submitted, nor are there task completion callbacks.
- The local mmap is exported to the remote memory to allow it to be used by the peer for RDMA read.
- No remote mmap is created as there is no intention to access the remote memory in this sample.

**Reference:**

- ` rdma_read_responder/rdma_read_responder_sample.c`
- ` rdma_read_responder/rdma_read_responder_main.c`
- ` rdma_read_responder/meson.build`

## RDMA Write

### RDMA Write Requester

This sample illustrates how to write to a remote peer (the responder) using DOCA RDMA.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for the local mmap in this sample are set to local read and write.
- A write task is configured for this sample.
- In this sample, data is written to the peer and printed in the successful task completion callback.
- The local mmap is not exported as the peer does not intend to access it.
- To write to the peer, a remote mmap is created from the peer's exported mmap.

**Reference:**

- ` rdma_write_requester/rdma_write_requester_sample.c`
- ` rdma_write_requester/rdma_write_requester_main.c`
- ` rdma_write_requester/meson.build`

### RDMA Write Responder

This sample illustrates how to set up a remote peer for a DOCA RDMA write request.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for both the local mmap and the RDMA instance in this sample allow for RDMA write.
- No tasks are configured for this sample, and thus no tasks are prepared and submitted, nor are there task completion callbacks.
- In this sample, the data written to the memory of the responder is printed once the context state is changed to Running, using the state change callback. This is done only after receiving input from the user, indicating that the requester had finished writing.
- The local mmap is exported to the remote memory to allow it to be used by the peer for RDMA write.
- No remote mmap is created as there is no intention to access the remote memory in this sample.

**Reference:**

- ` rdma_write_responder/rdma_write_responder_sample.c`
- ` rdma_write_responder/rdma_write_responder_main.c`
- ` rdma_write_responder/meson.build`

## RDMA Write Immediate

### RDMA Write Immediate Requester

This sample illustrates how to write to a remote peer (the responder) using DOCA RDMA along with a 32-bit immediate value which is sent OOB.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for the local mmap in this sample are set to local read and write.
- A write with immediate task is configured for this sample.
- In this sample, data is written to the peer and printed in the successful task completion callback.
- The local mmap is not exported as the peer does not intend to access it.
- To write to the peer, a remote mmap is created from the peer's exported mmap.

**Reference:**

- ` rdma_write_immediate_requester/rdma_write_immediate_requester_sample.c`
- ` rdma_write_immediate_requester/rdma_write_immediate_requester_main.c`
- ` rdma_write_immediate_requester/meson.build`

### RDMA Write Immediate Responder

This sample illustrates how to set up a remote peer for a DOCA RDMA write request whilst receiving a 32-bit immediate value from the peer's OOB.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for both the local mmap and the RDMA instance in this sample allow for RDMA write.
- A receive task is configured for this sample to retrieve the immediate value. Failing to submit a receive task prior to the write with immediate task results in a fatal failure.
- In this sample, the successful task completion callback also includes:
  - Checking the result opcode, to verify that the receive task has completed after receiving a write with immediate request.
  - Verifying the data written to the memory of the responder is valid and printing it, along with the immediate data received.
- The local mmap is exported to the remote memory, to allow it to be used by the peer for RDMA write.
- No remote mmap is created as there is no intention to access the remote memory in this sample.

**Reference:**

- ` rdma_write_immediate_responder/rdma_write_immediate_responder_sample.c`
- ` rdma_write_immediate_responder/rdma_write_immediate_responder_main.c`
- ` rdma_write_immediate_responder/meson.build`

## RDMA Send and Receive

### RDMA Send

This sample illustrates how to send a message to a remote peer using DOCA RDMA.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for the local mmap in this sample is set to local read and write.
- A send task is configured for this sample.
- In this sample, the data sent is printed during the task preparation, not in the successful task completion callback.
- The local mmap is not exported as the peer does not intend to access it.
- No remote mmap is created as there is no intention to access the remote memory in this sample.

**Reference:**

- ` rdma_send/rdma_send_sample.c`
- ` rdma_send/rdma_send_main.c`
- ` rdma_send/meson.build`

### RDMA Receive

This sample illustrates how the remote peer can receive a message sent by the peer (the sender).

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for the local mmap in this sample is set to local read and write.
- A receive task is configured for this sample to retrieve the sent data. Failing to submit a receive task prior to the send task results in a fatal failure.
- In this sample, data is received from the peer verified to be valid and printed in the successful task completion callback.
- The local mmap is not exported as the peer does not intend to access it.
- No remote mmap is created as there is no intention to access the remote memory in this sample.

**Reference:**

- ` rdma_receive/rdma_receive_sample.c`
- ` rdma_receive/rdma_receive_main.c`
- ` rdma_receive/meson.build`

## RDMA Send and Receive with Immediate

### RDMA Send with Immediate

This sample illustrates how to send a message to a remote peer using DOCA RDMA along with a 32-bit immediate value which is sent OOB.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for the local mmap in this sample is set to local read and write.
- A send with immediate task is configured for this sample.
- In this sample, the data sent is printed during the task preparation, not in the successful task completion callback.
- The local mmap is not exported as the peer does not intend to access it.
- No remote mmap is created as there is no intention to access the remote memory in this sample.

**Reference:**

- ` rdma_send_immediate/rdma_send_immediate_sample.c`
- ` rdma_send_immediate/rdma_send_immediate_main.c`
- ` rdma_send_immediate/meson.build`

### RDMA Receive with Immediate

This sample illustrates how the remote peer can receive a message sent by the peer (the sender) while also receiving a 32-bit immediate value from the peer's OOB.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for the local mmap in this sample is set to local read and write.
- A receive task is configured for this sample to retrieve the sent data and the immediate value. Failing to submit a receive task prior to the send with immediate task results in a fatal failure.
- In this sample, the successful task completion callback also includes:
  - Checking the result opcode, to verify that the receive task has completed after receiving a sent message with an immediate.
  - Verifying the data received from the peer is valid and printing it along with the immediate data received.
- In this sample, data is received from the peer verified to be valid and printed in the successful task completion callback.
- The local mmap is not exported as the peer does not intend to access it.
- No remote mmap is created as there is no intention to access the remote memory in this sample.

**Reference:**

- ` rdma_receive_immediate/rdma_receive_immediate_sample.c`
- ` rdma_receive_immediate/rdma_receive_immediate_main.c`
- ` rdma_receive_immediate/meson.build`

## RDMA Remote Sync Event

### RDMA Remote Sync Event Requester

This sample illustrates how to synchronize between local sync event and a remote sync event using DOCA RDMA.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for the local mmap in this sample is set to local read and write.
- A "remote net sync event notify set" task is configured for this sample.
  - For this task, the successful task completion callback has the following logic:
    - Printing an info log saying the task was successfully completed and a specific successful completion log for the task.
    - Decreasing the number of remaining tasks. Once 0 is reached:
      - Freeing the task and task-specific resources.
      - Stopping the context.
  - For this task, the failed task completion callback stops the context even when the number of remaining tasks is different than 0 (since the synchronization between the peers would fail).
- A "remote net sync event get" task is configured for this sample.
  - For this task, the successful task completion callback also includes:
    - Resubmitting the task, until a value greater than or equal to the expected value is retrieved.
    - Once such value is retrieved, submitting a "remote net sync event notify set" task to signal sample completion, including:
      - Updating the successful completion message accordingly.
      - Increasing the number of submitted tasks.
    - If an error was encountered, and the "remote net sync event notify set" task was not submitted, the task and task resources are freed.
  - For this task, the failed task completion callback also includes freeing the "remote net sync event notify set" task and task resources.
- The local mmap is not exported as the peer does not intend to access it.
- No remote mmap is created as there is no intention to access the remote memory in this sample.
- To synchronize events with the peer, a sync event remote net is created from the peer's exported sync event.
- Both tasks are prepared and submitted in the state change callback, once the context moves from starting to running.
- The user data of the "remote net sync event get" task points to the "remote net sync event notify set" task.

**Reference:**

- ` rdma_sync_event_requester/rdma_sync_event_requester_sample.c`
- ` rdma_sync_event_requester/rdma_sync_event_requester_main.c`
- ` rdma_sync_event_requester/meson.build`


### RDMA Remote Sync Event Responder

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for the local mmap in this sample is set to local read and write.
- This sample includes creating a local sync event and exporting it to the remote memory to allow the peer to create a remote handle.
- No tasks are configured for this sample, and thus no tasks are prepared and submitted, nor are there task completion callbacks. In this sample, the following steps are executed once the context moves from starting to running, using the state change callback:
  - Waiting for the sync event to be signaled from the remote side.
  - Notifying the sync event from the local side.
  - Waiting for completion notification from the remote side.

**Reference:**

- ` rdma_sync_event_responder/rdma_sync_event_responder_sample.c`
- ` rdma_sync_event_responder/rdma_sync_event_responder_main.c`
- ` rdma_sync_event_responder/meson.build`

# RDMA Multi-connections Send and Receive

The following samples illustrate how multiple connections can be performed to demonstrate an exchange of messages between peers.

Please note, using DOCA RDMA CM flow (see section ["Connecting Using RDMA CM Connection Flow"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-ConnectingUsingRDMACMConnectionFlow)):

- One sample would act as Server while the other would act as a client.
- Multiple instances of the sample acting as client need to be independently executed to simulate each client peer to reach the amount of desired RDMA connections.

## RDMA Multi-conn Send

This sample shows how multiple connections can be established and demonstrates:

- How multiple peers (clients) can send a message to a remote single peer (server) using DOCA RDMA CM flow (see section ["Connecting Using RDMA CM Connection Flow"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-ConnectingUsingRDMACMConnectionFlow)).
- How a single peer (server) can send a message to multiple remote peers (clients) using DOCA RDMA CM flow (see section ["Connecting Using RDMA CM Connection Flow"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-ConnectingUsingRDMACMConnectionFlow)).
- How multiple remote peers can send a message to their connected peers using the DOCA RDMA exporting and connection flow (see section ["Exporting and Connecting RDMA"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-ExportingandConnectingRDMA)).

> **Note:** In DOCA RDMA CM flow multiple instances of this sample need to be independently executed to simulate each client peer to reach the amount of desired RDMA connections.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for the local mmap in this sample is set to local read and write.
- A send task is configured for this sample.
- In this sample, the data sent is printed during the task preparation, not in the successful task completion callback.
- The local mmap is not exported as the peer does not intend to access it.
- No remote mmap is created as there is no intention to access the remote memory in this sample.
- The number of connections option can be set to:
  - The number of remote peers expected by the single peer (server sender) when using DOCA RDMA CM flow (see section ["Connecting Using RDMA CM Connection Flow"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-ConnectingUsingRDMACMConnectionFlow)).
  - The number of peers to connect to its remote peers using the DOCA RDMA exporting and connection flow (see section ["Exporting and Connecting RDMA"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-ExportingandConnectingRDMA)).

> **Note:** In DOCA RDMA CM flow, the number of connections cannot be used to simulate each client peer.

**Reference:**

- ` rdma_multi_conn_send/rdma_multi_conn_send_sample.c`
- ` rdma_multi_conn_send/rdma_multi_conn_send_main.c`
- ` rdma_multi_conn_send/meson.build`

## RDMA Multi-conn Receive

This sample shows how multiple connections can be performed and demonstrates:

- How multiple remote peers (clients) can receive a message sent by one single peer (the sender server) using DOCA RDMA CM flow (see section ["Connecting Using RDMA CM Connection Flow"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-ConnectingUsingRDMACMConnectionFlow)).
- How a single remote peer (server) can receive a message from multiple peers (the sender clients) using DOCA RDMA CM flow (see section ["Connecting Using RDMA CM Connection Flow"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-ConnectingUsingRDMACMConnectionFlow)).
- How multiple remote peers can receive a message sent by their connected peers using the DOCA RDMA exporting and connection flow (see section ["Exporting and Connecting RDMA"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-ExportingandConnectingRDMA)).

> **Note:** In DOCA RDMA CM flow multiple instances of this sample need to be independently executed to simulate each client peer to reach the amount of desired RDMA connections.

The sample logic is as presented in the General Sample Steps, with attention to the following:

- The permissions for the local mmap in this sample is set to local read and write.
- A receive task is configured for this sample to retrieve the sent data.

> **Note:** Failing to submit a receive task prior to the send task results in a fatal failure.

- In this sample, data is received from the peer verified to be valid and printed in the successful task completion callback.
- The local mmap is not exported as the peer does not intend to access it.
- No remote mmap is created as there is no intention to access the remote memory in this sample.
- The number of connections can be set to:
  - The number of peers expected by the single remote peer (server) when using DOCA RDMA CM flow (see section ["Connecting Using RDMA CM Connection Flow"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-ConnectingUsingRDMACMConnectionFlow)).
  - The number of remote peers connected to their sender peers using the DOCA RDMA exporting and connection flow (see section ["Exporting and Connecting RDMA"](https://docs.nvidia.com/doca/archive/2-9-0/doca+rdma/index.html#src-3113769286_id-.DOCARDMAv2.9.0LTS-ExportingandConnectingRDMA)).

> **Note:** In DOCA RDMA CM flow the number of connections cannot be used to simulate each client peer.

**Reference:**

- ` rdma_multi_conn_receive/rdma_multi_conn_receive_sample.c`
- ` rdma_multi_conn_receive/rdma_multi_conn_receive_main.c`
- ` rdma_multi_conn_receive/meson.build`

