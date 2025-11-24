# DOCA Rivermax Samples

These samples demonstrate various Rivermax functionalities such as listing devices, setting CPU affinity, configuring PTP clocks, and creating streams with different modes.

## List Devices

This sample illustrates how to list all available devices, dump their IPv4
addresses, and check for various device capabilities.

### Sample Logic:
1. Initializing the DOCA Rivermax library.
2. Iterating over the available devices.
3. Dumping their IPv4 addresses.
4. Checking if a PTP clock and various hardware-accelerating ordering modes are
   supported for each device.
5. Releasing the DOCA Rivermax library.

### References:
- `rmax_list_devices/rmax_list_devices_sample.c`
- `rmax_list_devices/rmax_list_devices_main.c`
- `rmax_list_devices/meson.build`
- `rmax_common.h`
- `rmax_common.c`

---

## Set CPU Affinity

This sample illustrates how to set the CPU affinity mask for Rivermax internal threads to achieve better performance. This must be set before initializing the library.

### Sample Logic:
1. Setting CPU affinity using the DOCA Rivermax API.
2. Initializing the DOCA Rivermax library.
3. Releasing the DOCA Rivermax library.

### References:
- `rmax_set_affinity/rmax_set_affinity_sample.c`
- `rmax_set_affinity/rmax_set_affinity_main.c`
- `rmax_set_affinity/meson.build`
- `rmax_common.h`
- `rmax_common.c`

---

## Set Clock

This sample illustrates how to set the PTP clock device to be used internally in DOCA Rivermax.

### Sample Logic:
1. Opening a DOCA device with a given PCIe address.
2. Initializing the DOCA Rivermax library.
3. Setting the device to use for obtaining PTP time.
4. Releasing the DOCA Rivermax library.

### References:
- `rmax_set_clock/rmax_set_clock_sample.c`
- `rmax_set_clock/rmax_set_clock_main.c`
- `rmax_set_clock/meson.build`
- `rmax_common.h`
- `rmax_common.c`

---

## Create Stream

This sample demonstrates how to create a stream, create a flow, attach it to the stream, and start receiving data buffers.

### Sample Logic:
1. Opening a DOCA device with a given PCIe address.
2. Initializing the DOCA Rivermax library.
3. Creating an input stream.
4. Creating the context from the created stream.
5. Initializing DOCA Core related objects.
6. Setting the attributes of the created stream.
7. Creating a flow and attaching it to the stream.
8. Starting to receive data buffers.
9. Clean-up: Detaching the flow, destroying the stream, and DOCA Core related objects.

### References:
- `rmax_create_stream/rmax_create_stream_sample.c`
- `rmax_create_stream/rmax_create_stream_main.c`
- `rmax_create_stream/meson.build`
- `rmax_common.h`
- `rmax_common.c`

---

## Create Stream – Header-data Split Mode

This sample demonstrates how to create a stream in header-data split mode, where packet headers and payloads are split into different RX buffers.

### Sample Logic:
1. Opening a DOCA device with a given PCIe address.
2. Initializing the DOCA Rivermax library.
3. Creating an input stream.
4. Creating the context from the created stream.
5. Initializing DOCA Core related objects.
6. Setting attributes for header-data split mode.
7. Creating a flow and attaching it to the stream.
8. Starting to receive data to split buffers.
9. Clean-up: Detaching the flow, destroying the stream, and DOCA Core related objects.

### References:
- `rmax_create_stream_hds/rmax_create_stream_hds_sample.c`
- `rmax_create_stream_hds/rmax_create_stream_hds_main.c`
- `rmax_create_stream_hds/meson.build`
- `rmax_common.h`
- `rmax_common.c`

---

## Create Stream – Hardware Packet Placement Order Mode

This sample demonstrates how to enable hardware-accelerated packet placement
for the stream. The NIC uses a sequence number from RTP header to determine the
order.

### Sample Logic:
1. Opening a DOCA device with a given PCIe address.
2. Initializing the DOCA Rivermax library.
3. Creating an input stream.
4. Creating the context from the created stream.
5. Initializing DOCA Core related objects.
6. Setting attributes for header-data split mode, enabling hardware-accelerated
   packet placement.
7. Creating a flow and attaching it to the stream.
8. Starting to receive data to split buffers.
9. Clean-up: Detaching the flow, destroying the stream, and DOCA Core related objects.

### References:
- `rmax_create_stream_hw_order_hds/rmax_create_stream_hw_order_hds_sample.c`
- `rmax_create_stream_hw_order_hds/rmax_create_stream_hw_order_hds_main.c`
- `rmax_create_stream_hw_order_hds/meson.build`
- `rmax_common.h`
- `rmax_common.c`
