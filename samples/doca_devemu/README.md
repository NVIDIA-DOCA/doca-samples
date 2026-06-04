#
# This software product is a proprietary product of NVIDIA CORPORATION &
# AFFILIATES (the "Company") and all right, title, and interest in and to the
# software product, including all associated intellectual property rights, are
# and shall remain exclusively with the Company.
#
# This software product is governed by the End User License Agreement
# provided with the software product.
#

# PCI Device Samples

## PCI Device List

This sample illustrates how to list all emulated devices that have the generic type configured in `devemu_pci_type_config.h`.

### Sample Logic

- Initializing the generic PCIe type based on `devemu_pci_type_config.h`.
- Creating a list of all emulated devices belonging to this type.
- Iterating over the emulated devices.
- Dumping their VUID.
- Dumping their PCIe address as seen by the host.
- Releasing the resources.

### References

- `doca_devemu/`
    - `devemu_pci_device_list/`
        - `devemu_pci_device_list_sample.c`
        - `devemu_pci_device_list_main.c`
        - `meson.build`
- `devemu_pci_common.h`
- `devemu_pci_common.c`
- `devemu_pci_type_config.h`

## PCI Device Hot-Plug

This sample illustrates how to create and hot-plug/hot-unplug an emulated device that has the generic type configured in `devemu_pci_type_config.h`.

### Sample Logic

- Initializing the generic PCIe type based on `doca_devemu/devemu_pci_type_config.h`.
- Acquiring the emulated device representor:
  - If the user did not provide VUID as input, then creating and using a new emulated device.
  - If the user provided VUID as an input, then searching for an existing emulated device with a matching VUID and using it.
- Creating a PCIe device context to manage the emulated device and connecting it to a progress engine (PE).
- Registering to the PCIe device's hot-plug state change event.
- Initializing hot-plug/hot-unplug of the device:
  - If the user did not provide VUID as input, then initializing hot-plug flow of the device.
  - If the user provided VUID as input, then initializing hot-unplug flow of the device.
- Using the PE to poll for hot-plug state change event.
- Waiting until hot-plug state transitions to expected state (power on or power off).
- Cleaning up resources.
  - If hot-unplug was requested, then the emulated device is destroyed as well.
  - Otherwise, the emulated device persists.

### References

- `/doca_devemu/`
    - `devemu_pci_device_hotplug/`
        - `devemu_pci_device_hotplug_sample.c`
        - `devemu_pci_device_hotplug_main.c`
        - `meson.build`
- `devemu_pci_common.h`
- `devemu_pci_common.c`
- `devemu_pci_type_config.h`

## PCI Device Stateful Region

This sample illustrates how the host driver can write to a stateful region, and how the BlueField Arm can handle the write operation.

### Sample Logic

#### BlueField Sample Logic

- Initializing the generic PCIe type based on `devemu_pci_type_config.h`.
- Acquiring the emulated device representor that matches the provided VUID.
- Creating a PCIe device context to manage the emulated device and connecting it to a progress engine (PE).
- For each stateful region configured in `devemu_pci_type_config.h`, registering to the PCIe device's stateful region write event.
- Using the PE to poll for driver write to any of the stateful regions.
- Every time the host driver writes to the stateful region, the handler is invoked and performs the following:
  - Queries the values of the stateful region that the host wrote to.
  - Logs the values of the stateful region.
- The sample polls indefinitely until the user presses `[Ctrl+c]` to close the sample.
- Cleaning up resources.

#### Host Sample Logic

- Initializing the VFIO device with a matching PCIe address and VFIO group.
- Mapping the stateful memory region from the BAR to the process address space.
- Writing the values provided as input to the beginning of the stateful region.

### References

- `doca_devemu/`
    - `devemu_pci_device_stateful_region/dpu/`
    - `devemu_pci_device_stateful_region_dpu_sample.c`
    - `devemu_pci_device_stateful_region_dpu_main.c`
    - `meson.build`
- `devemu_pci_device_stateful_region/host/`
    - `devemu_pci_device_stateful_region_host_sample.c`
    - `devemu_pci_device_stateful_region_host_main.c`
    - `meson.build`
- `devemu_pci_common.h`
- `devemu_pci_common.c`
- `devemu_pci_host_common.h`
- `devemu_pci_host_common.c`
- `devemu_pci_type_config.h`

## PCI Device DB

This sample illustrates how the host driver can ring the doorbell and how the BlueField can retrieve the doorbell value. The sample also demonstrates how to handle FLR.

### Sample Logic

#### BlueField Sample Logic

##### Host (BlueField Arm) Logic

- Initializing the generic PCIe type based on `devemu_pci_type_config.h`.
- Initializing DPA resources:
  - Creating DPA instance and associating it with the DPA application.
  - Creating DPA thread and associating it with the DPA DB handler.
  - Creating DB completion context and associating it with the DPA thread.
- Acquiring the emulated device representor that matches the provided VUID.
- Creating a PCIe device context to manage the emulated device and connecting it to progress engine (PE).
- Registering to the context state changes event.
- Registering to the PCIe device FLR event.
- Using the PE to poll for any of the following:
  - Every time the PCIe device context state transitions to running, the handler performs the following:
    - Creates a DB object.
    - Makes RPC to DPA, to initialize the DB object.
  - Every time the PCIe device context state transitions to stopping, the handler performs the following:
    - Makes RPC to DPA, to un-initialize the DB object.
    - Destroys the DB object.
  - Every time the host driver initializes or destroys the VFIO device, an FLR event is triggered. The FLR handler performs the following:
    - Destroys DB object.
    - Stops the PCIe device context.
    - Starts the PCIe device context again.
- The sample polls indefinitely until the user presses `[Ctrl+c]` to close the sample.

##### Device (BlueField DPA) Logic

- Initializing application RPC:
  - Setting the global context to point to the DB completion context DPA handle.
  - Binding DB to the doorbell completion context.
- Un-initializing application RPC:
  - Unbinding DB from the doorbell completion context.
- DB handler:
  - Getting DB completion element from completion context.
  - Getting DB handle from the DB completion element.
  - Acknowledging the DB completion element.
  - Requesting notification from DB completion context.
  - Requesting notification from DB.
  - Getting DB value from DB.

#### The host sample logic includes

- Initializing the VFIO device with its matching PCIe address and VFIO group.
- Mapping the DB memory region from the BAR to the process address space.
- Writing the value provided as input to the DB region at the given offset.

### References

- `doca_devemu/`
    - `devemu_pci_device_db/dpu/`
        - `host/`
            - `devemu_pci_device_db_dpu_sample.c`
    - `device/`
        - `devemu_pci_device_db_dpu_kernels_dev.c`
    - `devemu_pci_device_db_dpu_main.c`
    - `meson.build`
    - `devemu_pci_device_db/host/`
        - `devemu_pci_device_db_host_sample.c`
        - `devemu_pci_device_db_host_main.c`
        - `meson.build`
    - `devemu_pci_common.h`
    - `devemu_pci_common.c`
    - `devemu_pci_host_common.h`
    - `devemu_pci_host_common.c`
    - `devemu_pci_type_config.h`

## PCI Device MSI-X

This sample illustrates how BlueField can raise an MSI-X vector, either from the DPA or the DPU, sending a signal towards the host, and shows how the host can retrieve this signal.

### Sample Logic

#### BlueField Sample Logic

##### Host (BlueField Arm) Logic

- Initializing the generic PCIe type based on `devemu_pci_type_config.h`.
- According to the selected MSI-X datapath:
  - **DPA path (default)**:
    - Initializing DPA resources:
      - Creating a DPA instance and associating it with the DPA application.
      - Creating a DPA thread and associating it with the DPA MSI-X handler.
    - Setting the PCI device context datapath on DPA.
    - Acquiring the emulated device representor that matches the provided VUID.
    - Creating a PCIe device context to manage the emulated device and connecting it to a progress engine (PE).
    - Creating an MSI-X vector on the DPA and acquiring its DPA handle.
    - Sending an RPC to the DPA to raise the MSI-X vector.
  - **DPU path**:
    - Acquiring the emulated device representor that matches the provided VUID.
    - Creating a PCIe device context to manage the emulated device and connecting it to a progress engine (PE).
    - Creating an MSI-X vector on the DPU.
    - Raising the MSI-X vector directly from the DPU.
- Cleaning up resources.

##### Device (BlueField DPA) Logic

- Raising the MSI-X RPC by using the MSI-X vector handle (DPA path only).

#### Host Sample Logic

- Initializing the VFIO device with the matching PCIe address and VFIO group.
- Mapping each MSI-X vector to a different FD.
- Reading events from the FDs in a loop.
- Once the DPU raises MSI-X, the FD matching the MSI-X vector returns an event which is then printed to the screen.
- The sample polls the FDs indefinitely until the user presses `[Ctrl+c]` to close the sample.

### References

- `doca_devemu/`
    - `devemu_pci_device_msix/dpu/`
        - `host/`
            - `devemu_pci_device_msix_dpu_sample.c`
        - `device/`
            - `devemu_pci_device_msix_dpu_kernels_dev.c`
        - `devemu_pci_device_msix_dpu_main.c`
        - `meson.build`
    - `devemu_pci_device_msix/host/`
        - `devemu_pci_device_msix_host_sample.c`
        - `devemu_pci_device_msix_host_main.c`
        - `meson.build`
    - `devemu_pci_common.h`
    - `devemu_pci_common.c`
    - `devemu_pci_host_common.h`
    - `devemu_pci_host_common.c`
    - `devemu_pci_type_config.h`

## PCI Device DMA

This sample illustrates how the host driver can set up memory for DMA, then the DPU can use that memory to copy a string from the BlueField to the host and from the host to the BlueField.

### Sample Logic

#### BlueField Sample Logic

- Initializing the generic PCIe type based on `devemu_pci_type_config.h`.
- Acquiring the emulated device representor that matches the provided VUID.
- Creating a PCIe device context to manage the emulated device and connecting it to a progress engine (PE).
- Creating a DMA context to use for copying memory across the host and BlueField.
- Setting up an mmap representing the host driver memory buffer.
- Setting up an mmap representing a local memory buffer.
- Use the DMA context to copy memory from host to BlueField.
- Use the DMA context to copy memory from BlueField to host.
- Cleaning up resources.

#### Host Sample Logic

- Initializing the VFIO device with the matching PCIe address and VFIO group.
- Allocating memory buffer.
- Mapping the memory buffer to I/O memory. The BlueField can now access the memory using the I/O address through DMA.
- Copying the string provided by user to the memory buffer.
- Waiting for the BlueField to write to the memory buffer.
- Un-mapping the memory buffer.
- Cleaning up resources.

### References

- `doca_devemu/`
    - `devemu_pci_device_dma/dpu/`
        - `devemu_pci_device_dma_dpu_sample.c`
        - `devemu_pci_device_dma_dpu_main.c`
        - `meson.build`
    - `devemu_pci_device_dma/host/`
        - `devemu_pci_device_dma_host_sample.c`
        - `devemu_pci_device_dma_host_main.c`
        - `meson.build`

## PCI Device TLP Handler

This sample illustrates how to handle raw Transaction Layer Packets (TLPs) at the BlueField DPU level, providing low-level PCIe protocol processing capabilities for custom device emulation. It also demonstrates how the host driver can interact with transaction regions for testing TLP processing functionality. In addition, the sample supports TLP channel handover (live upgrade): a running source instance can transfer its active TLP channel to a newly started destination instance with zero host-visible disruption, enabling seamless in-service software upgrades.
This sample implements a single PCIe endpoint and is limited to a single TLP channel downstream port, therefore the user must set the number of TLP ports to 1 via mlxconfig before running the sample.

### Sample Logic

#### BlueField Sample Logic

- Initializing a custom PCI TLP type with specific device and vendor IDs.
- Creating and configuring a TLP channel to receive and process raw PCIe transactions.
- Setting up PCI configuration space structure with proper headers, BARs, and device capabilities.
- Registering a TLP request handler callback function to process incoming transactions.
- Creating a device representor to enable host-side device enumeration.
- Initializing a TLP device context for transaction processing.
- Processing various types of TLP requests including:
  - Configuration space read/write operations (Type 0)
  - Memory read/write operations
  - Completion handling with appropriate status codes
- Handling PCIe protocol-specific fields such as:
  - Transaction tags and request IDs
  - Bus/Device/Function (BDF) addressing
  - Byte enable masks and data alignment
  - Completion status codes (Success, Unsupported Request, etc.)
- Parsing TLP headers to extract transaction information and routing details.
- Generating appropriate TLP completion responses for posted and non-posted transactions.
- Managing device state and configuration space updates.
- Supporting TLP channel handover for live upgrade between two instances:
  - Both the **source** and **destination** must be started with the same `--shm-dir-path` (shared memory directory); handover is not available without this path on either side.
  - The **source** instance runs normally and listens for an incoming handover request on a Unix domain socket.
  - The **destination** instance is started with the `--handover-destination` flag. It connects to the source and performs a multi-step handover protocol:
    1. **SETUP** — receives the ibverbs `cmd_fd` via `SCM_RIGHTS` to import the same DOCA device context.
    2. **EXPORT** — receives the serialized TLP channel export descriptor to reconstruct the channel.
    3. **BEGIN** — receives the PCI configuration space, TLP handler context, transaction region contents, and Expansion ROM contents; the source stops the channel.
    4. **END** — notifies the source of success or failure; on success the source destroys its channel and exits, and the destination starts handling incoming TLP channel requests.
  - If the destination fails during a handover, the source attempts rollback to continue serving the host.
- The sample polls indefinitely for TLP requests until the user presses `[Ctrl+c]` to close the sample.

#### Host Sample Logic

- Initializing the VFIO device with a matching PCIe address and VFIO group.
- Mapping the transaction memory region from the BAR to the process address space.
- Writing the values provided as input to the beginning of the transaction region, or reading existing values if no input is provided.
- The sample demonstrates interaction with transaction regions that can be processed by the TLP handler on the BlueField DPU.

### References

- `doca_devemu/`
    - `devemu_pci_device_tlp_handler/dpu/`
        - `devemu_pci_device_tlp_handler_dpu_sample.c`
        - `devemu_pci_device_tlp_handler_dpu_main.c`
        - `meson.build`
    - `devemu_pci_device_tlp_handler/host/`
        - `devemu_pci_device_tlp_handler_host_sample.c`
        - `devemu_pci_device_tlp_handler_host_main.c`
        - `meson.build`
    - `devemu_pci_common.h`
    - `devemu_pci_common.c`
    - `devemu_pci_host_common.h`
    - `devemu_pci_host_common.c`
    - `devemu_pci_type_config.h`

## PCI Device TLP Bridge Handler

This sample illustrates how to emulate a complete PCIe switch topology with multiple bridges and endpoints using raw Transaction Layer Packets (TLPs). It demonstrates advanced PCIe fabric emulation capabilities by implementing a hierarchical bus architecture with upstream/downstream ports and multiple endpoint devices.

### Sample Logic

- Initializing a custom PCI TLP type for bridge and multi-endpoint topology.
- Creating and configuring a TLP channel to receive and process PCIe transactions across the entire topology.
- Initializing a complex PCIe device topology with one sub-topology per TLP channel DSP, each consisting of:
  - 1 Upstream Switch Port (USP)
  - N Downstream Switch Ports (DSPs) connected to the USP, where N is the number of endpoints assigned to this channel DSP
  - N Single-PF endpoint devices, each connected to a DSP
- Setting up PCI configuration space structures for both Type 0 (endpoint) and Type 1 (bridge) headers.
- Implementing PCIe switch routing logic:
  - Type 0 configuration requests routing to USPs
  - Type 1 configuration requests routing to DSPs and endpoints
  - Bus number management and secondary bus assignment
  - Device number mapping across the switch fabric
- Creating device representors for all endpoint devices to enable host enumeration.
- Initializing TLP device contexts for hardware-accelerated endpoint processing.
- Allocating independent transaction memory regions for each endpoint device.
- Processing various types of TLP requests with topology-aware routing:
  - Configuration space read/write operations (Type 0 and Type 1)
  - Memory read/write operations with BDF-based device lookup
  - Bridge-specific configuration (bus numbers, memory windows)
  - Endpoint-specific configuration (BARs, capabilities)
- Handling PCIe capabilities for endpoints:
  - PCIe Express capability
  - MSI-X capability
  - VPD (Vital Product Data) capability
  - Power Management capability
- Managing bridge and endpoint device states independently.
- The sample polls indefinitely for TLP requests until the user presses `[Ctrl+c]` to close the sample.

### References

- `doca_devemu/`
    - `devemu_pci_device_tlp_bridge_handler/`
        - `devemu_pci_device_tlp_bridge_handler_config.h`
        - `devemu_pci_device_tlp_bridge_handler_sample.c`
        - `devemu_pci_device_tlp_bridge_handler_main.c`
        - `meson.build`
    - `devemu_pci_common.h`
    - `devemu_pci_common.c`
    - `devemu_pci_type_config.h`
