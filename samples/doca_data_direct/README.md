# DOCA Data Direct Control Sample

The Data Direct Control tool allows Data Direct Interface configuration for
ConnectX-8 devices.

## Background

The ConnectX-8 device may expose a side DMA engine as an additional PCIe PF
called Data Direct device. This additional device allows access to data buffers
through multiple PCIe data path interfaces.

This tool allows to perform configurations related to data direct, such as
enabling/disabling data direct for VFs and SFs (which is disabled by default)
and showing the mapping of a device (PF/VF/SF) to its associated data direct
device.

## Prerequisites

### Hardware

- ConnectX-8 NIC

### Drivers

- fwctl.ko (fwctl device firmware access framework)
- mlx5_fwctl.ko (mlx5 ConnectX fwctl driver)

## Building

To build data direct control, run the following commands:

```
$ meson setup <build_dir>
$ meson compile -C <build_dir>
```

where `<build_dir>` is the directory in which to build the tool.

## Usage

### SYNOPSIS

```
doca_data_direct_ctl set data-direct <device> [ -e <BOOL> | --enable=<BOOL> ]
                                              [ -h | --help ]
                                              [ -v | --verbose ]

doca_data_direct_ctl show data-direct <device> [ -h | --help ]
                                               [ -j | --json ]
                                               [ -v | --verbose ]

doca_data_direct_ctl show data-direct-device [<device>] [ -h | --help ]
                                                        [ -j | --json ]
                                                        [ -v | --verbose ]

doca_data_direct_ctl help
```

### DESCRIPTION

#### doca_data_direct_ctl set data-direct - Enable or disable data direct

Enable or disable data direct for the given device.

The `<device>` argument is mandatory and must be a devlink representor device
(as listed in "devlink port", e.g., pci/0000:08:00.0/1).

##### OPTIONS

`-e <BOOL>, --enable=<BOOL>`
:	Enable or disable data direct.

`-h, --help`
:	Show help menu and exit.

`-v, --verbose`
:	Increase verbosity of the output.

---

#### doca_data_direct_ctl show data-direct - Show data direct state

Show the data direct state of the given device.

The `<device>` argument is mandatory and must be a devlink representor device
(as listed in "devlink port", e.g., pci/0000:08:00.0/1).

##### OPTIONS

`-h, --help`
:	Show help menu and exit.

`-j, --json`
:	Print the output in JSON format.

`-v, --verbose`
:	Increase verbosity of the output.

---

#### doca_data_direct_ctl show data-direct-device - Show data direct device

Show the mapping of devices and their data direct devices.

The `<device>` argument is optional. If `<device>` is provided, it must be a
devlink device (as listed in "devlink dev", e.g., pci/0000:08:00.0) or a devlink
representor device (as listed in "devlink port", e.g., pci/0000:08:00.0/1), and
the data direct device of the given device will be shown. If `<device>` is not
provided, the data direct devices of all applicable devices will be shown.

##### OPTIONS

`-h, --help`
:	Show help menu and exit.

`-j, --json`
:	Print the output in JSON format.

`-v, --verbose`
:	Increase verbosity of the output.

### NOTES

Asterisk (\*) in the data direct device output refers to all the VFs/SFs that
belong to a PF. For example, an output of pci/0000:08:00.0/* means that all
VFs/SFs of the device pci/0000:08:00.0 have the same data direct device.

Enabling or disabling data direct for a device must be done prior to the device
initialization (e.g., before the device is bound to mlx5_core driver).

Data direct will remain enabled for a VF even after it's destroyed and
re-created, so please explicitly disable data direct if it's no longer needed
for a VF.
