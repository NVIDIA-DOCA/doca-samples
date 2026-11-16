#
# This software product is a proprietary product of NVIDIA CORPORATION &
# AFFILIATES (the "Company") and all right, title, and interest in and to the
# software product, including all associated intellectual property rights, are
# and shall remain exclusively with the Company.
#
# This software product is governed by the End User License Agreement
# provided with the software product.
#

# Samples

This section provides DPA sample implementation on top of the BlueField-3 networking platform.

## Info

All the DOCA samples described in this section are governed under the BSD-3 software license agreement.

DPA samples can be run from either Host or DPU. When running from DPU and using RDMA utilities, you need to provide the RDMA DOCA device (SF device) as a parameter (if not provided, then a random RDMA device will be chosen). In this case, the DOCA DPA context is created on the PF device, an Extended DPA context is created on the SF device, and the DOCA RDMA context is created on the SF device.

**Note:** All DPA resources such as DPA thread, DPA completion context, etc., which are associated with the SF RDMA instance must be created using the extended DPA context.

## Running DPA Samples

Refer to the following documents:

- NVIDIA DOCA Installation Guide for Linux for details on how to install BlueField-related software.
- NVIDIA DOCA Troubleshooting for any issue you may encounter with the installation, compilation, or execution of DOCA samples.

To build a given sample:

```sh
cd doca_dpa/<sample_name>
meson /tmp/build
ninja -C /tmp/build
```

**Info:** The binary `doca_<sample_name>` is created under `/tmp/build/`.

## Sample Usage

For example, to use `dpa_initiator_target`:

```sh
Usage: doca_dpa_initiator_target [DOCA Flags] [Program Flags]
```

### DOCA Flags:
- `-h, --help`                              Print a help synopsis
- `-v, --version`                           Print program version information
- `-l, --log-level`                         Set the (numeric) log level for the program <10=DISABLE, 20=CRITICAL, 30=ERROR, 40=WARNING, 50=INFO, 60=DEBUG, 70=TRACE>
- `--sdk-log-level`                         Set the SDK (numeric) log level for the program <10=DISABLE, 20=CRITICAL, 30=ERROR, 40=WARNING, 50=INFO, 60=DEBUG, 70=TRACE>
- `-j, --json <path>`                       Parse command line flags from an input json file

### Program Flags:
- `-pf_dev, --pf-device <PF DOCA device name>` PF device name that supports DPA (optional). If not provided, then a random device will be chosen.
- `-rdma_dev, --rdma-device <RDMA DOCA device name>` device name that supports RDMA (optional). If not provided, then a random device will be chosen.

For additional information per sample, use the `-h` option:

```sh
/tmp/build/doca_<sample_name> -h
```

## Basic Initiator Target

This sample illustrates how to trigger DPA Thread using DPA Completion Context attached to DOCA RDMA. This sample consists of initiator and target endpoints.

In the initiator endpoint, a DOCA RDMA executes RDMA post send operation using DPA RPC. In the target endpoint, a DOCA RDMA, attached to DPA Completion Context, executes RDMA post receive operation using DPA RPC. Completion on the post receive operation triggers DPA Thread which prints completion info and sets DOCA Sync Event to release the host application that waits on that event before destroying all resources and finishing.

### Sample Logic

- Allocating DOCA DPA & DOCA RDMA resources for both initiator and target endpoints.
- Target: Attaching DOCA RDMA to DPA Completion Context which is attached to DPA Thread.
- Run DPA Thread.
- Target: DPA RPC to execute RDMA post receive operation.
- Initiator: DPA RPC to execute RDMA post send operation.
- The completion on the post receive operation triggers DPA Thread.
- Waiting on completion event to be set from DPA Thread.
- Destroying all resources.

![](doca_dpa.jpg)

### Reference:

- `doca_dpa/dpa_basic_initiator_target/dpa_basic_initiator_target_main.c`
- `doca_dpa/dpa_basic_initiator_target/host/dpa_basic_initiator_target_sample.c`
- `doca_dpa/dpa_basic_initiator_target/device/dpa_basic_initiator_target_kernels_dev.c`
- `doca_dpa/dpa_basic_initiator_target/meson.build`
- `doca_dpa/dpa_common.h`
- `doca_dpa/dpa_common.c`
- `doca_dpa/build_dpacc_samples.sh`

# Advanced Initiator Target

This sample illustrates how to trigger DPA threads using both DPA Notification Completion and DPA Completion Context which is attached to a DOCA RDMA context with 2 connections. This sample consists of initiator and target endpoints.

In the initiator endpoint, two DOCA RDMA connections execute an RDMA post send operation using DPA RPC in the following order:

1. Connection #0 executes the RDMA post send operation on buffer with value 1.
2. Connection #1 executes the RDMA post send operation on buffer with value 2.
3. Connection #0 executes the RDMA post send operation on buffer with value 3.
4. Connection #1 executes the RDMA post send operation on buffer with value 4.

In the target endpoint, DOCA RDMA context with two connections is attached to a single DPA Completion Context which is attached to DPA Thread #1. The target RDMA executes the initial RDMA post receive operation using DPA RPC. Completions on the post receive operations trigger DPA Thread #1 which prints completion info including user data, updates a local database with the receive buffer value, reposts the RDMA receive operation, acknowledges, requests completion, and reschedules.

Once target DPA Thread #1 receives all expected values "1, 2, 3, 4", it notifies DPA Thread #2 and finishes. Once DPA Thread #2 is triggered, it sets DOCA Sync Event to release the host application that waits on that event before destroying all resources and finishing.

## Sample Logic

- Allocating DOCA DPA and DOCA RDMA resources for both initiator and target endpoints.
- Target: Attaching DOCA RDMA to DPA Completion Context which is attached to DPA Thread #1.
- Target: Attaching DPA Notification Completion to DPA Thread #2.
- Run DPA threads.
- Target: DPA RPC to execute the initial RDMA post receive operation.
- Initiator: DPA RPC to execute all RDMA post send operations.
- Completions on the post receive operations (4 completions) trigger DPA Thread #1.
- Once all expected values are received, DPA Thread #1 notifies DPA Thread #2 and finishes.
- Waiting on completion event to be set from DPA Thread #2.
- Destroying all resources.

![](doca_dpa2.jpg)


### Reference:

- `doca_dpa/dpa_initiator_target/dpa_initiator_target_main.c`
- `doca_dpa/dpa_initiator_target/host/dpa_initiator_target_sample.c`
- `doca_dpa/dpa_initiator_target/device/dpa_initiator_target_kernels_dev.c`
- `doca_dpa/dpa_initiator_target/meson.build`
- `doca_dpa/dpa_common.h`
- `doca_dpa/dpa_common.c`
- `doca_dpa/build_dpacc_samples.sh`

# Ping Pong

This sample illustrates the functionality of the following DPA objects:

- DPA Thread
- DPA Completion Context
- DOCA RDMA

This sample consists of ping and pong endpoints which run for 100 iterations. On each iteration, DPA threads (ping and pong) post RDMA receive and send operations for data buffers with values [0-99]. Once all expected values are received on each DPA thread, it sets a DOCA Sync Event to release the host application waiting on that event before destroying all resources and finishes.

To trigger DPA threads, the sample uses a DPA RPC.

## Sample Logic

- Allocating DOCA DPA and DOCA RDMA resources.
- Attaching DOCA RDMA to DPA completion context which is attached to DPA thread.
- Run DPA threads.
- DPA RPC to trigger DPA threads.
- 100 ping pong iterations of RDMA post receive and send operations.
- Waiting on completion events to be set from DPA threads.
- Destroying all resources.

![](doca_dpa3.jpg)

### Reference:

- `doca_dpa/dpa_ping_pong/dpa_ping_pong_main.c`
- `doca_dpa/dpa_ping_pong/host/dpa_ping_pong_sample.c`
- `doca_dpa/dpa_ping_pong/device/dpa_ping_pong_kernels_dev.c`
- `doca_dpa/dpa_ping_pong/meson.build`
- `doca_dpa/dpa_common.h`
- `doca_dpa/dpa_common.c`
- `doca_dpa/build_dpacc_samples.sh`

# Kernel Launch

This sample illustrates how to launch a DOCA DPA kernel with wait and completion DOCA sync events.

## Sample Logic

- Allocating DOCA DPA resources.
- Initializing wait and completion DOCA sync events for the DOCA DPA kernel.
- Running `hello_world` DOCA DPA kernel that waits on the wait event.
- Running a separate thread that triggers the wait event.
- `hello_world` DOCA DPA kernel prints "Hello from kernel".
- Waiting for the completion event of the kernel.
- Destroying the events and resources.

## Reference

- `doca_dpa/dpa_wait_kernel_launch/dpa_wait_kernel_launch_main.c`
- `doca_dpa/dpa_wait_kernel_launch/host/dpa_wait_kernel_launch_sample.c`
- `doca_dpa/dpa_wait_kernel_launch/device/dpa_wait_kernel_launch_kernels_dev.c`
- `doca_dpa/dpa_wait_kernel_launch/meson.build`
- `doca_dpa/dpa_common.h`
- `doca_dpa/dpa_common.c`
- `doca_dpa/build_dpacc_samples.sh`

# Verbs Initiator Target
This sample illustrates how to perform RDMA operations using DOCA Verbs API.

## Sample Logic
- Allocating DOCA DPA resources, including:
        - Initializing and configuring DPA threads and kernels.
	- Initializing buffer on DPA heap for Verbs operations and attaching mmap.
	- Initializing DPA completion context, that will be used instead of CQ.
	- Initializing DOCA sync event to synchronize host and DPA when data path is finished.
- Allocating Verbs resources, including – verbs context, verbs QP attached to DPA completion context, verbs PD, verbs AH.
- Connection Establishment between initiator and target using TCP socket
- RDMA Parameters Exchange - initiator and target exchange local buffer addresses, MKEYs, QP numbers and GID addresses
- QP Connection - set QP attributes, Modify QP states from Reset to Ready to Send.
- Trigger RPC for first iteration, and then data path starting for number of iterations:
        Initiator
	- Wait for completion
	- Poll local buffer value to sync with target's Write operation
	- Increase buffer’s value by 1
	- Post Send work request
	- Acknowledgment completion
	Target
	- Wait for completion
	- Post Receive work request
	- Increase buffer’s value from what it received by 1
	- Post Write work request
	- Acknowledgment 2 completions
- In the last operation, initiator verify that the end value in the buffer is as expected. Both initiator and target updating sync event to threshold value to sync host.
- Printing sample data summary.
- Destroying all resources.

### Reference
- `doca_dpa/dpa_verbs_initiator_target/dpa_verbs_initiator_target_main.c`
- `doca_dpa/dpa_verbs_initiator_target/host/dpa_verbs_initiator_target_sample.c`
- `doca_dpa/dpa_verbs_initiator_target/device/dpa_verbs_initiator_target_kernels_dev.c`
- `doca_dpa/dpa_verbs_initiator_target/meson.build`
- `doca_dpa/dpa_common.h`
- `doca_dpa/dpa_common.c`
- `doca_dpa/build_dpacc_samples.sh`

# Event Channel

This sample illustrates how a single DPA thread can service multiple DPA Completion Contexts through a thread event channel, waking on any completion and identifying which one raised it by the completion id the channel reports.

A DPA thread is created with an event channel enabled and two DPA Completion Contexts attached to it. Each completion is raised from a DPA RPC (using `doca_dpa_dev_thread_notify()`), which wakes the thread. On each wake the thread reads the id of the completion that woke it with `doca_dpa_dev_thread_get_completion_id_from_event_channel()`, without polling each of the completions attached to it, and writes that id into the DPA memory it shares with the host. It then sets a DOCA Sync Event to tell the host it is done, and the host reads the reported id back. In a real application the completions would instead be produced by data-path activity (e.g. RDMA/DMA operations).

## Program Flags

- `-pf_dev, --pf-device <PF DOCA device name>` PF device name that supports DPA (optional). If not provided, then a random device will be chosen.

## Sample Logic

- Allocating DOCA DPA resources.
- Querying event channel support and the maximum completions per event channel.
- Creating a DPA Thread with event channel enabled and a maximum number of completions.
- Creating two DPA Completion Contexts attached to the thread and retrieving their ids.
- Creating a DOCA Sync Event for the thread to signal the host with once it is done.
- Running the DPA Thread.
- For each DPA Completion Context: a DPA RPC raises a completion; the thread wakes, reads the completion id from its event channel and reports it, and the host verifies that the reported id is the one of the raised completion.
- Destroying all resources.

## Reference

- `doca_dpa/dpa_event_channel/dpa_event_channel_main.c`
- `doca_dpa/dpa_event_channel/host/dpa_event_channel_sample.c`
- `doca_dpa/dpa_event_channel/device/dpa_event_channel_kernels_dev.c`
- `doca_dpa/dpa_event_channel/common/dpa_event_channel_common_defs.h`
- `doca_dpa/dpa_event_channel/meson.build`
- `doca_dpa/dpa_common.h`
- `doca_dpa/dpa_common.c`
- `doca_dpa/build_dpacc_samples.sh`
