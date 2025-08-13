# DOCA Compress Samples

The following samples illustrate how to use the DOCA Compress API to compress and decompress files.

> **Note:**
> DOCA Compress handles payload only unless the `zc` flag is used (available only for deflate samples). In that case, a zlib header and trailer are added in compression, and it is considered part of the input when decompressing.

### Info

All the DOCA samples described in this section are governed under the BSD-3 software license agreement.

## Running the Sample

1. Refer to the following documents:

- **[NVIDIA DOCA Installation Guide for Linux](https://docs.nvidia.com/doca/archive/2-9-0/NVIDIA+DOCA+Installation+Guide+for+Linux)** for details on how to install BlueField-related software.
- **[NVIDIA DOCA Troubleshooting](https://docs.nvidia.com/doca/archive/2-9-0/NVIDIA+DOCA+Troubleshooting)** for any issue you may encounter with the installation, compilation, or execution of DOCA samples.

2. To build a given sample:

```bash
cd <sample_name>
meson /tmp/build
ninja -C /tmp/build
```

> **Note:**
> The binary `doca_<sample_name>` is created under `/tmp/build/`.

3. Sample Usage (e.g., `doca_compress_deflate`):

- **Common Arguments:**

```bash
Usage: doca_<sample_name> [DOCA Flags] [Program Flags]

DOCA Flags:
  -h, --help                        Print a help synopsis
  -v, --version                     Print program version information
  -l, --log-level                   Set the (numeric) log level for the program <10=DISABLE, 20=CRITICAL, 30=ERROR, 40=WARNING, 50=INFO, 60=DEBUG, 70=TRACE>
  --sdk-log-level                   Set the SDK (numeric) log level for the program <10=DISABLE, 20=CRITICAL, 30=ERROR, 40=WARNING, 50=INFO, 60=DEBUG, 70=TRACE>

Program Flags:
  -p, --pci-addr                    DOCA device PCI device address
  -f, --file                        Input file to compress/decompress
  -o, --output                      Output file
  -c, --output-checksum             Output checksum
```

- **Sample-Specific Arguments:**

| **Sample**                  | **Argument**                 | **Description**                                                      |
|-----------------------------|------------------------------|----------------------------------------------------------------------|
| **Compress/Decompress Deflate** | `-wf, --with-frame`           | Write/read a file with a frame, compatible with default zlib settings |
| **Decompress LZ4 Stream**   | `-bc, --has-block-checksum`   | Flag to indicate if blocks have a checksum                           |
|                             | `-bi, --are-blocks-independent` | Flag to indicate if blocks are independent                          |
|                             | `-wf, --with-frame`           | Read a file compatible with an LZ4 frame                             |


4. For additional information per sample, use the `-h` option:
```bash
  /tmp/build/doca_<sample_name> -h
```

## Samples

### Compress/Decompress Deflate

This sample illustrates how to use DOCA Compress library to compress or decompress a file.

#### Sample Logic:

- Locate a DOCA device.
- Initialize the required DOCA Core structures.
- Populate DOCA memory map with two relevant buffers: one for the source data and one for the result.
- Allocate elements in DOCA buffer inventory for each buffer.
- Allocate and initialize a DOCA Compress deflate task or a DOCA Decompress deflate task.
- Submit the task.
- Run the progress engine until the task is completed.
- Write the result into an output file (`out.txt`).
- Destroy all DOCA Compress and DOCA Core structures.

**References:**

- `compress_deflate/compress_deflate_sample.c`
- `compress_deflate/compress_deflate_main.c`
- `compress_deflate/meson.build`
- `decompress_deflate/decompress_deflate_sample.c`
- `decompress_deflate/decompress_deflate_main.c`
- `decompress_deflate/meson.build`
- `compress_common.h`
- `compress_common.c`

### Decompress LZ4 Stream

This sample illustrates how to use DOCA Compress library to decompress a file using the LZ4 stream decompress task.

#### Sample Logic:

- Locate a DOCA device.
- Initialize the required DOCA Core structures.
- Populate DOCA memory map with two relevant buffers: one for the source data and one for the result.
- Allocate elements in DOCA buffer inventory for each buffer.
- Allocate and initialize a DOCA Decompress LZ4 stream task.
- Submit the task.
- Run the progress engine until the task is completed.
- Write the result into an output file (`out.txt`).
- Destroy all DOCA Compress and DOCA Core structures.

**References:**

- `decompress_lz4_stream/decompress_lz4_stream_sample.c`
- `decompress_lz4_stream/decompress_lz4_stream_main.c`
- `decompress_lz4_stream/meson.build`
- `compress_common.h`
- `compress_common.c`

### Backward Compatibility

#### Decompress LZ4 Task

The **decompress LZ4 task** has been removed. To facilitate decompressing memory with the LZ4 algorithm, use the decompress LZ4 stream task or the decompress LZ4 block task instead.
