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

This sample illustrates how BlueField can raise an MSI-X vector, sending a signal towards the host, and shows how the host can retrieve this signal.

### Sample Logic

#### BlueField Sample Logic

##### Host (BlueField Arm) Logic

- Initializing the generic PCIe type based on `devemu_pci_type_config.h`.
- Initializing DPA resources:
  - Creating a DPA instance and associating it with the DPA application.
  - Creating a DPA thread and associating it with the DPA DB handler.
- Acquiring the emulated device representor that matches the provided VUID.
- Creating a PCIe device context to manage the emulated device and connecting it to a progress engine (PE).
- Creating an MSI-X vector and acquiring its DPA handle.
- Sending an RPC to the DPA to raise the MSI-X vector.
- Cleaning up resources.

##### Device (BlueField DPA) Logic

- Raising the MSI-X RPC by using the MSI-X vector handle.

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
