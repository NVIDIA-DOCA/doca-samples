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

# DOCA Flow Connection Tracking Samples
This section describes DOCA Flow CT samples based on the DOCA Flow CT pipe.

The samples illustrate how to use the library API to manage TCP/UDP connections.

**Info**

    All the DOCA samples described in this section are governed under the BSD-3 software license agreement.

# Running the Samples

Refer to the following documents:

- NVIDIA DOCA Installation Guide for Linux for details on how to install BlueField-related software.
- NVIDIA DOCA Troubleshooting for any issue you may encounter with the installation, compilation, or execution of DOCA samples.

## Building a Sample

To build a given sample:

```sh
cd doca_flow/flow_ct_udp
meson /tmp/build
ninja -C /tmp/build
```

**Info:** The binary `doca_flow_ct_udp` is created under `/tmp/build/samples/`.

## Sample Usage

For example, to use `doca_flow_ct_udp`:

```sh
Usage: doca_<sample_name> [DOCA Flags] [Program Flags]
```

### DOCA Flags:
- `-h, --help`                              Print a help synopsis
- `-v, --version`                           Print program version information
- `-l, --log-level`                         Set the (numeric) log level for the program <10=DISABLE, 20=CRITICAL, 30=ERROR, 40=WARNING, 50=INFO, 60=DEBUG, 70=TRACE>
- `--sdk-log-level`                         Set the SDK (numeric) log level for the program <10=DISABLE, 20=CRITICAL, 30=ERROR, 40=WARNING, 50=INFO, 60=DEBUG, 70=TRACE>
- `-j, --json <path>`                       Parse command line flags from an input json file

### Program Flags:
- `-a, --dev <DEVICE-ADDRESS>`              Device address (e.g., pci/03:00.0)

For additional information per sample, use the `-h` option:

```sh
/tmp/build/samples/<sample_name> -h
```

### Additional Information:

For detailed explanations of each sample’s functionality and configuration, please refer to the corresponding YAML files provided with the samples.
