#
# Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are permitted
# provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright notice, this list of
#       conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright notice, this list of
#       conditions and the following disclaimer in the documentation and/or other materials
#       provided with the distribution.
#     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
#       to endorse or promote products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
# FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
# STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

# Management Data Direct

The ConnectX-8 device may expose a side DMA engine as an additional PCIe PF called Data Direct device. This additional device allows access to data buffers through multiple PCIe data path interfaces.

By default, data direct capability is disabled for VFs and SFs, and it must be explicitly enabled to allow data direct usage for a certain VF or SF.

This sample allows getting and setting (enabling/disabling) the data direct capability for a given VF or SF, and demonstrates how to do it using DOCA management library API.

## Prerequisites

### Hardware

- ConnectX-8 NIC

### Drivers

- fwctl.ko (fwctl device firmware access framework)
- mlx5_fwctl.ko (mlx5 ConnectX fwctl driver)

### DOCA

- DOCA Management library

## Building

To build the sample, run the following commands:

```
$ cd <doca_samples_dir>/doca_mgmt/mgmt_data_direct
$ meson setup build
$ meson compile -C build
```

The sample binary `doca_mgmt_data_direct` will be built under `build` directory.

## Usage

The sample has two commands:
* `get` which shows the current data direct capability state.
* `set` which enables or disables the data direct capability.

For both commands, the device (VF/SF) to operate on must be specified.

Specifying a VF can be done in two ways:
1. Specifying the VF PCI address, e.g., 0000:08:00.2, using the `-v/--vf-pci-addr` parameter.
2. Specifying the VF representor, using the `-r/--rep` parameter with the following format: `pci/<parent_pf_pci_address>,pf<pfnum>vf<vfnum>`.  
   For example, for VF 0000:08:00.2 whose VF number is 0 and parent PF is 0000:08:00.0 with PF number 0, the identifier would be `pci/0000:08:00.0,pf0vf0`.

Specifying a SF is done by its representor, using the `-r/--rep` parameter with the following format: `pci/<parent_pf_pci_address>,pf<pfnum>sf<sfnum>`.  
For example, for SF whose SF number is 88 and parent PF is 0000:08:00.0 with PF number 0, the identifier would be `pci/0000:08:00.0,pf0sf88`.

For `set` command, `-e/--enabled` parameter must be specified as well with true/false values, which indicates whether data direct capability should be enabled or disabled, respectively.

Important notes:
1. In order get or set data direct, the VF or SF must have a representor, i.e., their parent PF must be in switchdev mode.
2. In order to set data direct, the VF or SF must be uninitialized, meaning, for example, that they must not be bound to mlx5_core driver.
3. Data direct will remain enabled for a VF even after it's destroyed and re-created, so data direct must be explicitly disabled if it's no longer needed for a VF.

## Examples

- Get data direct state for a VF whose PCI address is 0000:08:00.2:
```
$ doca_mgmt_data_direct get --vf-pci-addr 0000:08:00.2
```

- Enable data direct for a VF whose PCI address is 0000:08:00.2:
```
$ doca_mgmt_data_direct set --vf-pci-addr 0000:08:00.2 --enabled true
```

- Disable data direct for a VF whose PCI address is 0000:08:00.2:
```
$ doca_mgmt_data_direct set --vf-pci-addr 0000:08:00.2 --enabled false
```

- Enable data direct for a VF whose VF number is 0 and parent PF is 0000:08:00.0 with PF number 0 (Note: the VF must be uninitialized, i.e., not bound to mlx5_core driver):
```
$ doca_mgmt_data_direct set --rep pci/0000:08:00.0,pf0vf0 --enabled true
```

- Enable data direct for a SF whose SF number is 88 and parent PF is 0000:08:00.0 with PF number 0 (Note: the SF must be uninitialized, i.e., not bound to mlx5_core driver):
```
$ doca_mgmt_data_direct set --rep pci/0000:08:00.0,pf0sf88 --enabled true
```

## Sample Logic

The sample logic includes:

### Get Data Direct Operation:
1. Opening the DOCA device and a DOCA device representor that were specified in the command line arguments.
2. Creating a DOCA management device context for the DOCA device and a DOCA management device representor context for the DOCA device representor.
3. Creating a device caps general handle.
4. Executing get operation through the DOCA management device caps general API.
5. Parsing the command response to extract error code and retrieving the data direct capability for the specified representor interface.
6. Cleaning up all DOCA management and device structures.

### Set Data Direct Operation:
1. Opening the DOCA device that was specified in the command line arguments.
2. Creating a DOCA management device context for the DOCA device and a DOCA management device representor context for the DOCA device representor.
3. Creating a device caps general handle and setting data direct attribute.
4. Executing set operation through the DOCA management device caps general API.
5. Parsing the command response to extract error code.
6. Cleaning up all DOCA management and device structures.

## References

- `mgmt_data_direct_sample.c`
- `mgmt_data_direct_main.c`
- `meson.build`

# Management Congestion Control Global Status

This sample illustrates how to manage congestion control global status settings on a DOCA device using the DOCA Management library.

The sample demonstrates how to get and set congestion control global status for different priorities and protocols (NP/RP) on a DOCA device.

## Sample Logic

The sample logic includes:

### Get Congestion Control Global Status Operation:
1. Opening the DOCA device that was specified in the command line arguments.
2. Creating a DOCA management device context for the DOCA device.
3. Creating a congestion control global status handle and setting priority and protocol attributes.
4. Executing get operation through the DOCA management congestion control global status API.
5. Parsing the command response to extract error code and retrieving the congestion control global status enabled attribute for the specified priority and rotocol.
6. Cleaning up all DOCA management and device structures.

### Set Congestion Control Global Status Operation:
1. Opening the DOCA device that was specified in the command line arguments.
2. Creating a DOCA management device context for the DOCA device.
3. Creating a congestion control global status handle and setting priority, protocol and enabled attributes.
4. Executing set operation through the DOCA management congestion control global status API.
5. Parsing the command response to extract error code.
6. Cleaning up all DOCA management and device structures.

## References

- `mgmt_cc_global_status_sample.c`
- `mgmt_cc_global_status_main.c`
- `meson.build`

# Management ICM Quota

ICM (Interconnect Context Memory) is host memory that is allocated exclusively for the HCA, and is used to maintain and manage its control objects.
The amount of ICM that is needed by the HCA may vary and depends on the capabilities and the amount of resources that the HCA is required to support.

Host memory is a finite resource, and thus it may be useful to limit the HCA's usage of it. On devices that support it, it is possible to limit the amount of ICM that a function is allowed to use.

This sample allows getting, setting, and querying capabilities for ICM quota on a given device or device representor (PF/VF/SF), and demonstrates how to do it using DOCA management library API.

## Building

To build the sample, run the following commands:

```
$ cd <doca_samples_dir>/doca_mgmt/mgmt_icm_quota
$ meson setup build
$ meson compile -C build
```

The sample binary `doca_mgmt_icm_quota` will be built under `build` directory.

## Usage

The sample has three commands:
* `caps` which shows the ICM quota capabilities for the device.
* `get` which shows the current ICM quota configuration (limit, current allocation, max reached allocation).
* `set` which sets the ICM quota limit or resets the max reached allocation counter.

For all commands, either a device or a device representor must be specified.

Specifying a device is done using the `-d/--device` parameter with the following format: `pci/<pci_address>`.  
For example, for device 0000:08:00.0, the identifier would be `pci/0000:08:00.0`.

Specifying a VF representor is done using the `-r/--rep` parameter with the following format: `pci/<parent_pf_pci_address>,pf<pfnum>vf<vfnum>`.  
For example, for VF whose VF number is 0 and parent PF is 0000:08:00.0 with PF number 0, the identifier would be `pci/0000:08:00.0,pf0vf0`.

Specifying a SF representor is done using the `-r/--rep` parameter with the following format: `pci/<parent_pf_pci_address>,pf<pfnum>sf<sfnum>`.  
For example, for SF whose SF number is 88 and parent PF is 0000:08:00.0 with PF number 0, the identifier would be `pci/0000:08:00.0,pf0sf88`.

For a full documentation of device and representor identifiers patterns, please refer to the DOCA Arg Parser documentation.

The `caps` command shows whether the device supports ICM quota and if so, it shows the maximum ICM qouta limit that can be set by `set` command.

For `get` command, the following optional parameters can be specified to retrieve specific attributes:
- `--limit` - Get the ICM quota limit that is configured for the device
- `--cur-alloc` - Get the currently allocated ICM quota of the device
- `--max-reached` - Get the maximum reached ICM quota that the device has reached so far

If no parameters are specified, all attributes are retrieved.

For `set` command, at least one of the following parameters must be specified:
- `-L/--limit` - Set the ICM quota limit for the device (e.g., 4096, 4K, 1M, 1G, 1T, unlimited). The value must be aligned to 4K. The value must be less than or equal to the maximum ICM quota limit reported by `caps` command and less than or equal to 16TB-8KB. Value of 'unlimited' indicates no limit.
- `--reset-max-reached` - Reset the maximum reached ICM quota counter

## Examples

- Get ICM quota capabilities for a device:
```
$ doca_mgmt_icm_quota caps --device pci/0000:08:00.0
```

- Get all ICM quota attributes for a device:
```
$ doca_mgmt_icm_quota get --device pci/0000:08:00.0
```

- Get only the ICM quota limit for a device:
```
$ doca_mgmt_icm_quota get --device pci/0000:08:00.0 --limit
```

- Set ICM quota limit to 1GB for a VF representor:
```
$ doca_mgmt_icm_quota set --rep pci/0000:08:00.0,pf0vf0 --limit 1G
```

- Set ICM quota limit to unlimited for a device:
```
$ doca_mgmt_icm_quota set --device pci/0000:08:00.0 --limit unlimited
```

- Reset the max reached counter for a SF representor:
```
$ doca_mgmt_icm_quota set --rep pci/0000:08:00.0,pf0sf88 --reset-max-reached
```

## Sample Logic

The sample logic includes:

### Get ICM Quota Operation:
1. Opening the DOCA device or device representor that was specified in the command line arguments.
2. Creating a DOCA management device context for the DOCA device, and a DOCA management device representor context for the device representor.
3. Creating an ICM quota handle.
4. Executing get operation through the DOCA management ICM quota API.
5. Retrieving and displaying the requested attributes (limit, current allocation, max reached).
6. Cleaning up all DOCA management and device structures.

### Set ICM Quota Operation:
1. Opening the DOCA device or device representor that was specified in the command line arguments.
2. Creating a DOCA management device context for the DOCA device, and a DOCA management device representor context for the device representor.
3. Creating an ICM quota handle and setting the requested attributes (limit, reset max reached).
4. Executing set operation through the DOCA management ICM quota API.
5. Cleaning up all DOCA management and device structures.

### Get ICM Quota Capabilities Operation:
1. Opening the DOCA device or device representor that was specified in the command line arguments.
2. Creating a DOCA management device context for the DOCA device or for the DOCA device representor.
3. Checking if ICM quota is supported on the device.
4. Retrieving and displaying the maximum ICM quota limit supported by the device.
5. Cleaning up all DOCA management and device structures.

## References

- `mgmt_icm_quota_sample.c`
- `mgmt_icm_quota_main.c`
- `meson.build`
