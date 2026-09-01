#
# Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

# DOCA Management Samples

The DOCA Management library provides an interface for querying and configuring NVIDIA networking devices and their representors (PF/VF/SF) over the firmware control (fwctl) interface. This directory contains a set of samples, each is a standalone command-line tool, demonstrating the various device management capabilities.

This section provides general information common to all the samples; detailed documentation for each sample — its purpose, prerequisites, commands, and examples — is provided in its dedicated section below.

## Building

To build a sample, run the following commands (replace `<sample>` with the sample's directory name, e.g., `mgmt_icm_quota`):

```
$ cd <doca_samples_dir>/doca_mgmt/<sample>
$ meson setup build
$ meson compile -C build
```

The resulting binary `doca_mgmt_<sample>` is produced under the `build` directory.

## Usage

Each sample is invoked as `doca_mgmt_<sample> <command> [options]`. The device or representor to operate on is specified as a target string via the `-d/--device` or `-r/--rep` parameter (see [Listing Management Targets](#listing-management-targets) below to discover them).

Beyond the shared `list-targets` command, most samples follow a common command pattern:

* `list-targets` — enumerate the management targets available on the system (supported by all samples).
* `caps` — check whether the capability is supported by the target (where applicable).
* `get` — read the current configuration.
* `set` — modify the configuration.

The exact set of commands and parameters varies per sample (for example, `mgmt_raw` groups its operations under the `register` and `hca-cap` sub-commands). See the per-sample sections below for each sample's specific commands, parameters, and examples.

### Listing Management Targets

All the samples share a common `list-targets` command that enumerates the management targets available on the system and prints their target strings. A target string printed by `list-targets` can be passed directly to a sample's `-d/--device` parameter (device target) or `-r/--rep` parameter (representor target).

```
$ doca_mgmt_<sample> list-targets [-v/--verbose]
```

By default only the target strings are printed. Pass `-v/--verbose` to additionally print each target's attributes. In samples that manage representors, each device's representor targets are printed nested beneath it.

#### Target String Formats

A device target string identifies a device (e.g., a PF):
* Device: `pci/<BDF>`, e.g., `pci/0000:08:00.0`.

A representor target string identifies a representor, and its form depends on the representor type:
* Uplink: `pci/<BDF>`
* VF:     `pci/<BDF>,c<controller>pf<pf>vf<vf>`, e.g., `pci/0000:08:00.0,c0pf0vf2`
* SF:     `pci/<BDF>,c<controller>pf<pf>sf<sf>`, e.g., `pci/0000:08:00.0,c0pf0sf88`
* PF:     `pci/<BDF>,c<controller>pf<pf>vf-1`

#### Examples

- List the device targets (target strings only):
```
$ doca_mgmt_diagnostics_data list-targets
pci/0000:08:00.0
pci/0000:08:00.1
```

- List the device targets and their representors, with all attributes:
```
$ doca_mgmt_data_direct list-targets --verbose
pci/0000:08:00.0
    pci_func_type       PF
    native_port_num     1
    iface_name          eth2
    ibdev_name          mlx5_0
    pci/0000:08:00.0,c0pf0vf0
        pci_func_type       VF
        controller_num      0
        pf_index            0
        vf_index            0
        vhca_id             2
        iface_name          eth4
```

The shared `list-targets` command and target-string helpers are implemented in `mgmt_common.c` / `mgmt_common.h`.

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

The sample has three commands:
* `list-targets` which lists the available management targets (see [Listing Management Targets](#listing-management-targets)).
* `get` which shows the current data direct capability state.
* `set` which enables or disables the data direct capability.

For `get` and `set`, the device (VF/SF) to operate on must be specified.

Specifying a VF can be done in two ways:
1. Specifying the VF representor, using the `-r/--rep` parameter with a representor target string, e.g., `pci/0000:08:00.0,c0pf0vf0` (see [Target String Formats](#target-string-formats)). Use `list-targets` to discover the available representor targets.
2. Specifying the VF PCI address, e.g., 0000:08:00.2, using the `-v/--vf-pci-addr` parameter.

Specifying a SF is done by its representor, using the `-r/--rep` parameter with a representor target string, e.g., `pci/0000:08:00.0,c0pf0sf88`.

For `set` command, `-e/--enabled` parameter must be specified as well with true/false values, which indicates whether data direct capability should be enabled or disabled, respectively.

Important notes:
1. In order get or set data direct, the VF or SF must have a representor, i.e., their parent PF must be in switchdev mode.
2. If the device port type is Infiniband, which doesn't support representors, it's still possible to configure data direct for a VF by the `-v/--vf-pci-addr` parameter.
3. In order to set data direct, the VF or SF must be uninitialized, meaning, for example, that they must not be bound to mlx5_core driver.
4. Data direct will remain enabled for a VF even after it's destroyed and re-created, so data direct must be explicitly disabled if it's no longer needed for a VF.

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

- Enable data direct for a VF whose VF number is 0 and parent PF is 0000:08:00.0 with PF number 0 on controller 0 (Note: the VF must be uninitialized, i.e., not bound to mlx5_core driver):
```
$ doca_mgmt_data_direct set --rep pci/0000:08:00.0,c0pf0vf0 --enabled true
```

- Enable data direct for a SF whose SF number is 88 and parent PF is 0000:08:00.0 with PF number 0 on controller 0 (Note: the SF must be uninitialized, i.e., not bound to mlx5_core driver):
```
$ doca_mgmt_data_direct set --rep pci/0000:08:00.0,c0pf0sf88 --enabled true
```

- List the available targets:
```
$ doca_mgmt_data_direct list-targets
```

## Sample Logic

The sample logic includes:

### Get Data Direct Operation:
1. Creating a DOCA management device representor context for the target VF/SF: either from the representor target string or from the VF PCI address.
2. Creating a device caps general handle.
3. Executing get operation through the DOCA management device caps general API.
4. Parsing the command response to extract error code and retrieving the data direct capability for the specified representor interface.
5. Cleaning up all DOCA management and device structures.

### Set Data Direct Operation:
1. Creating a DOCA management device representor context for the target VF/SF: either from the representor target string or from the VF PCI address.
2. Creating a device caps general handle and setting data direct attribute.
3. Executing set operation through the DOCA management device caps general API.
4. Parsing the command response to extract error code.
5. Cleaning up all DOCA management and device structures.

## References

- `mgmt_data_direct_sample.c`
- `mgmt_data_direct_main.c`
- `meson.build`

# Management Congestion Control Global Status

This sample illustrates how to manage congestion control global status settings on a DOCA device using the DOCA Management library.

The sample demonstrates how to get and set congestion control global status for different priorities and protocols (NP/RP) on a DOCA device.

## Usage

The sample has three commands:
* `list-targets` which lists the available management targets (see [Listing Management Targets](#listing-management-targets)).
* `get` which shows the current congestion control global status.
* `set` which enables or disables the congestion control global status.

For `get` and `set`, the device to operate on must be specified using the `-d/--device` parameter with a device target string, e.g., `pci/0000:08:00.0` (see [Target String Formats](#target-string-formats)). Use `list-targets` to discover the available device targets.

The following parameters are also supported:
- `-p/--priority` - The congestion control priority (0-7). Default: 0.
- `-t/--protocol` - The congestion control protocol, `rp` or `np`. Default: `rp`.

For `set`, the `-e/--enabled` parameter must be specified with `true` or `false`, indicating whether the congestion control should be enabled or disabled, respectively.

## Examples

- List the available targets:
```
$ doca_mgmt_cc_global_status list-targets
```

- Get the congestion control global status for a device, priority 3, protocol np:
```
$ doca_mgmt_cc_global_status get --device pci/0000:08:00.0 --priority 3 --protocol np
```

- Enable the congestion control global status for a device, priority 0, protocol rp:
```
$ doca_mgmt_cc_global_status set --device pci/0000:08:00.0 --priority 0 --protocol rp --enabled true
```

## Sample Logic

The sample logic includes:

### Get Congestion Control Global Status Operation:
1. Creating a DOCA management device context from the target string that was specified in the command line arguments.
2. Creating a congestion control global status handle and setting priority and protocol attributes.
3. Executing get operation through the DOCA management congestion control global status API.
4. Parsing the command response to extract error code and retrieving the congestion control global status enabled attribute for the specified priority and protocol.
5. Cleaning up all DOCA management structures.

### Set Congestion Control Global Status Operation:
1. Creating a DOCA management device context from the target string that was specified in the command line arguments.
2. Creating a congestion control global status handle and setting priority, protocol and enabled attributes.
3. Executing set operation through the DOCA management congestion control global status API.
4. Parsing the command response to extract error code.
5. Cleaning up all DOCA management structures.

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

The sample has four commands:
* `list-targets` which lists the available management targets (see [Listing Management Targets](#listing-management-targets)).
* `caps` which shows the ICM quota capabilities for the device.
* `get` which shows the current ICM quota configuration (limit, current allocation, max reached allocation).
* `set` which sets the ICM quota limit or resets the max reached allocation counter.

For `caps`, `get` and `set`, either a device or a device representor must be specified (see [Target String Formats](#target-string-formats)):
* A device is specified using the `-d/--device` parameter with a device target string, e.g., `pci/0000:08:00.0`.
* A VF or SF representor is specified using the `-r/--rep` parameter with a representor target string, e.g., `pci/0000:08:00.0,c0pf0vf0` (VF) or `pci/0000:08:00.0,c0pf0sf88` (SF).

The `-d/--device` and `-r/--rep` parameters are mutually exclusive. Use `list-targets` to discover the available targets.

The `caps` command operates on a device target only, showing whether the device supports ICM quota and, if so, the maximum ICM quota limit that can be set by the `set` command. The capabilities reported for a device also apply to its representors.

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
$ doca_mgmt_icm_quota set --rep pci/0000:08:00.0,c0pf0vf0 --limit 1G
```

- Set ICM quota limit to unlimited for a device:
```
$ doca_mgmt_icm_quota set --device pci/0000:08:00.0 --limit unlimited
```

- Reset the max reached counter for a SF representor:
```
$ doca_mgmt_icm_quota set --rep pci/0000:08:00.0,c0pf0sf88 --reset-max-reached
```

- List the available targets:
```
$ doca_mgmt_icm_quota list-targets
```

## Sample Logic

The sample logic includes:

### Get ICM Quota Operation:
1. Creating a DOCA management device context from the device target string, or a DOCA management device representor context from the representor target string.
2. Creating an ICM quota handle.
3. Executing get operation through the DOCA management ICM quota API.
4. Retrieving and displaying the requested attributes (limit, current allocation, max reached).
5. Cleaning up all DOCA management structures.

### Set ICM Quota Operation:
1. Creating a DOCA management device context from the device target string, or a DOCA management device representor context from the representor target string.
2. Creating an ICM quota handle and setting the requested attributes (limit, reset max reached).
3. Executing set operation through the DOCA management ICM quota API.
4. Cleaning up all DOCA management structures.

### Get ICM Quota Capabilities Operation:
1. Creating a DOCA management device context from the device target string.
2. Checking if ICM quota is supported on the device.
3. Retrieving and displaying the maximum ICM quota limit supported by the device.
4. Cleaning up all DOCA management structures.

## References

- `mgmt_icm_quota_sample.c`
- `mgmt_icm_quota_main.c`
- `meson.build`

# Management Diagnostics Data

This sample demonstrates how to use the DOCA Management diagnostics data API on a device that supports it: check capability, read the current multi-domain setting, and apply a new multi-domain setting.

## Building

To build the sample, run the following commands:

```
$ cd <doca_samples_dir>/doca_mgmt/mgmt_diagnostics_data
$ meson setup build
$ meson compile -C build
```

The sample binary `doca_mgmt_diagnostics_data` is produced under the `build` directory.

## Usage

The sample provides four commands:

* `list-targets` — lists the available management targets (see [Listing Management Targets](#listing-management-targets)).
* `caps` — reports whether diagnostics data is supported on the given device.
* `get` — queries the device and prints the current `multi_domain` value (`true` / `false`).
* `set` — sets the desired `multi_domain` value on the device.

For `caps`, `get` and `set`, the target device is mandatory and is passed with `-d` / `--device` using a device target string, e.g., `pci/0000:08:00.0` (see [Target String Formats](#target-string-formats)). Use `list-targets` to discover the available device targets.

For `set`, you must also pass `--multi-domain` with `true` or `false` (enable or disable multi-domain mode).

**Note:** Changing multi_domain may be rejected by the device if any of the relevant diagnostics data domains of the device has ownership; in that case an appropriate error is returned.

## Examples

- Check whether diagnostics data is supported:

```
$ doca_mgmt_diagnostics_data caps --device pci/0000:08:00.0
```

- Read the current multi-domain setting:

```
$ doca_mgmt_diagnostics_data get --device pci/0000:08:00.0
```

- Enable multi-domain:

```
$ doca_mgmt_diagnostics_data set --device pci/0000:08:00.0 --multi-domain true
```

- Disable multi-domain:

```
$ doca_mgmt_diagnostics_data set --device pci/0000:08:00.0 --multi-domain false
```

- List the available targets:

```
$ doca_mgmt_diagnostics_data list-targets
```

## Sample Logic

The sample logic includes:

### Caps (support check)

1. Create a DOCA management device context from the target string.
2. Call `doca_mgmt_cap_diagnostics_data_multi_domain_is_supported` and print supported / unsupported (or an error).
3. Destroy the management device context.

### Get (query)

1. Create a DOCA management device context from the target string.
2. Create a diagnostics data handle, `doca_mgmt_diagnostics_data_query_for_dev`, then `doca_mgmt_diagnostics_data_get_multi_domain` and print the value.
3. Destroy the handle and management context.

### Set (modify)

1. Create a DOCA management device context from the target string.
2. Create a diagnostics data handle, `doca_mgmt_diagnostics_data_set_multi_domain`, then `doca_mgmt_diagnostics_data_modify_for_dev`.
3. Destroy the handle and management context.

## References

- `mgmt_diagnostics_data_sample.c`
- `mgmt_diagnostics_data_main.c`
- `meson.build`

# Management Raw

This sample demonstrates how to use the DOCA Management raw API which provides a low level interface for device configuration for advanced users.

The sample illustrates how to use the raw register API to list supported registers of a device, describe the fields of a register, query a register and modify a register.

The sample also illustrates how to use the raw HCA cap API to list supported HCA capability types of a device, describe the fields of a HCA capability type, query HCA capabilities and modify HCA capabilities.

Detailed explanation of each register and its fields, and each HCA capability type and its fields is outside the scope of the API and may be obtained separately.

## Building

To build the sample, run the following commands:

```
$ cd <doca_samples_dir>/doca_mgmt/mgmt_raw
$ meson setup build
$ meson compile -C build
```

The sample binary `doca_mgmt_raw` will be built under `build` directory.

## Listing Targets

In addition to the `register` and `hca-cap` commands, the sample provides a `list-targets` command that lists the available management targets (see [Listing Management Targets](#listing-management-targets)). The device target strings it prints can be passed to the `-d/--device` parameter, and (with `--verbose`) the representor VHCA IDs it prints are useful for the `-v/--vhca-id` parameter of the `hca-cap` commands, as well as for `vhca_id` input fields of certain registers.

Example:
```
$ doca_mgmt_raw list-targets --verbose
```

## Raw Register

This section describes the `register` command of the sample.

### Usage

The sample has a `register` command with four sub-commands:
* `list` – List available registers for the device.
* `describe` – List available fields for a given register.
* `get` – Query a register. Returns all fields if output fields are not specified, or only the requested fields.
* `set` – Modify a register with the given input fields.

For all sub-commands, the device must be specified using the `-d/--device` parameter with the format `pci/<pci_address>`. For example, for device 0000:08:00.0, use `pci/0000:08:00.0`.

For `describe`, `get`, and `set`, the register name must be specified using `-r/--register-name`.

For `get`, the following optional parameters can be specified:
- `-i/--input-fields` – Input fields in format `field_name1=value1,field_name2=value2,...`.
- `-o/--output-fields` – Output fields in format `field_name1,field_name2,...`. If omitted, all fields are returned.

For `set`, the following parameter is required:
- `-i/--input-fields` – Input fields in format `field_name1=value1,field_name2=value2,...`.

Detailed information about the available commands and their parameters is available through the `-h/--help` parameter of each command.

In case of errors, the SDK error logs may hold detailed information that can help reveal the underlying issue. The SDK error logs are enabled by default and can be controlled by the `--sdk-log-level` parameter.

### Examples

- List available registers of a device:
```
$ doca_mgmt_raw register list --device pci/0000:08:00.0
```

- Describe fields of VHCA_ICM_CTRL register:
```
$ doca_mgmt_raw register describe --device pci/0000:08:00.0 --register-name VHCA_ICM_CTRL
```

- Get all fields of VHCA_ICM_CTRL register:
```
$ doca_mgmt_raw register get --device pci/0000:08:00.0 --register-name VHCA_ICM_CTRL
```

- Get specific fields of VHCA_ICM_CTRL register:
```
$ doca_mgmt_raw register get --device pci/0000:08:00.0 --register-name VHCA_ICM_CTRL --output-fields cur_alloc_icm,max_reached_alloc_icm
```

- Get specific fields of VHCA_ICM_CTRL register with input parameters:
```
$ doca_mgmt_raw register get --device pci/0000:08:00.0 --register-name VHCA_ICM_CTRL --input-fields vhca_id=2,vhca_id_valid=1 --output-fields cur_alloc_icm
```

- Set VHCA_ICM_CTRL register fields:
```
$ doca_mgmt_raw register set --device pci/0000:08:00.0 --register-name VHCA_ICM_CTRL --input-fields max_alloc_icm_th_mask=1,max_alloc_icm_th=0xffffffff
```

## Raw HCA Cap

This section describes the `hca-cap` command of the sample.

### Usage

The sample has a `hca-cap` command with four sub-commands:
* `list` – List available HCA capability types for the device.
* `describe` – List available fields for a given HCA capability type.
* `get` – Query HCA capabilities. Returns all fields if output fields are not specified, or only the requested fields.
* `set` – Modify HCA capabilities with the given input fields.

For all sub-commands, the device must be specified using the `-d/--device` parameter with the format `pci/<pci_address>`. For example, for device 0000:08:00.0, use `pci/0000:08:00.0`.

For `describe`, `get`, and `set`, the HCA capability type must be specified using `-c/--cap-type` parameter.

For `get`, the following parameters can be specified:
- `-m/--cap-mode` - The capability mode to query. Can be either `max` or `current`. This is a required parameter.
- `-f/--function-id` - The function ID of the target function to query its capabilities. This is an optional parameter. Mutually exclusive with `-v/--vhca-id`. If both `-f/--function-id` and `-v/--vhca-id` are not set, the target function is the one specified in `-d/--device`.
- `-v/--vhca-id` - The VHCA ID of the target function to query its capabilities. This is an optional parameter. Mutually exclusive with `-f/--function-id`. If both `-f/--function-id` and `-v/--vhca-id` are not set, the target function is the one specified in `-d/--device`.
- `-o/--output-fields` – Output fields in format `field_name1,field_name2,...`. If omitted, all fields are returned.

For `set`, the following parameters are required (`-f/--function-id` and `-v/--vhca-id` are mutually exclusive):
- `-f/--function-id` - The function ID of the target function to query its capabilities. Mutually exclusive with `-v/--vhca-id`.
- `-v/--vhca-id` - The VHCA ID of the target function to query its capabilities. Mutually exclusive with `-f/--function-id`.
- `-i/--input-fields` – Input fields in format `field_name1=value1,field_name2=value2,...`.

Detailed information about the available commands and their parameters is available through the `-h/--help` parameter of each command.

HCA capabilities can be set only to a function which has not been initialized yet. For example, the function must not be bound to mlx5_core driver when setting its HCA capabilities.

In case of errors, the SDK error logs may hold detailed information that can help reveal the underlying issue. The SDK error logs are enabled by default and can be controlled by the `--sdk-log-level` parameter.

### Examples

- List available HCA capability types of a device:
```
$ doca_mgmt_raw hca-cap list --device pci/0000:08:00.0
```

- Describe fields of cmd_hca_cap HCA capability type:
```
$ doca_mgmt_raw hca-cap describe --device pci/0000:08:00.0 --cap-type cmd_hca_cap
```

- Get all fields of cmd_hca_cap HCA capability type in max capability mode:
```
$ doca_mgmt_raw hca-cap get --device pci/0000:08:00.0 --cap-type cmd_hca_cap --cap-mode max
```

- Get specific fields of cmd_hca_cap HCA capability type in current capability mode:
```
$ doca_mgmt_raw hca-cap get --device pci/0000:08:00.0 --cap-type cmd_hca_cap --cap-mode current --output-fields vhca_id,log_max_qp
```

- Get specific fields of cmd_hca_cap HCA capability type in max capability mode of another function specified by its VHCA ID:
```
$ doca_mgmt_raw hca-cap get --device pci/0000:08:00.0 --cap-type cmd_hca_cap --cap-mode max --vhca-id 2 --output-fields vhca_id,log_max_qp
```

- Set fields of cmd_hca_cap HCA capability type of another function specified by its VHCA ID:
```
$ doca_mgmt_raw hca-cap set --device pci/0000:08:00.0 --cap-type cmd_hca_cap --vhca-id 2 --input-fields selective_repeat=0x0
```

## Sample Logic

The sample logic includes:

### List Registers Operation:
1. Creating a DOCA management device context from the target string.
2. Retrieving the list of supported register names via the DOCA management raw register API.
3. Displaying the register names and cleaning up.

### Describe Register Operation:
1. Creating a DOCA management device context from the target string.
2. Retrieving the list of field descriptors for the specified register via the DOCA management raw register API.
3. Displaying the fields and cleaning up.

### Get Register Operation:
1. Creating a DOCA management device context from the target string.
2. Parsing optional input fields and optional output fields from the command line.
3. If output fields were specified, querying only those fields; otherwise querying all fields via the DOCA management raw register API.
4. Displaying the results and cleaning up.

### Set Register Operation:
1. Creating a DOCA management device context from the target string.
2. Parsing input fields from the command line.
3. Modifying the register with the given fields via the DOCA management raw register API.
4. Cleaning up.

### List HCA Capability Types Operation:
1. Creating a DOCA management device context from the target string.
2. Retrieving the list of supported HCA capability types via the DOCA management raw HCA cap API.
3. Displaying the HCA capability types and cleaning up.

### Describe HCA Capability Operation:
1. Creating a DOCA management device context from the target string.
2. Retrieving the list of field descriptors for the specified HCA capability type via the DOCA management raw HCA cap API.
3. Displaying the fields and cleaning up.

### Get HCA Capability Operation:
1. Creating a DOCA management device context from the target string.
2. Parsing optional output fields from the command line.
3. If output fields were specified, querying only those fields; otherwise querying all fields via the DOCA management raw HCA cap API.
4. Displaying the results and cleaning up.

### Set HCA Capability Operation:
1. Creating a DOCA management device context from the target string.
2. Parsing input fields from the command line.
3. Modifying the HCA capabilities with the given fields via the DOCA management raw HCA cap API.
4. Cleaning up.

## References

- `mgmt_raw_sample.c`
- `mgmt_raw_main.c`
- `meson.build`
