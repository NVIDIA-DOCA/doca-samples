# DOCA UROM Samples

This section provides DOCA UROM library sample implementations on top of BlueField.

The samples illustrate how to use the DOCA UROM API to do the following:

- Define and create UROM plugin host and DPU versions for offloading HPC/AI tasks.
- Build host applications that use the plugin to execute jobs on BlueField by the DOCA UROM service and workers.

## Info

All the DOCA samples described in this section are governed under the BSD-3 software license agreement.

---

## Sample Prerequisites

| Sample           | Type    | Prerequisite                                    |
|------------------|---------|------------------------------------------------|
| **Sandbox**      | Plugin  | A plugin which offloads the UCX tagged send/receive API |
| **Graph**        | Plugin  | The plugin uses UCX data structures and UCX endpoint |
| **UROM Ping Pong** | Program | The sample uses the Open MPI package as a launcher framework to launch two processes in parallel |

---

## Running the Sample

Refer to the following documents:

- **NVIDIA DOCA Installation Guide for Linux**: Details on how to install BlueField-related software.
- **NVIDIA DOCA Troubleshooting**: Assistance for any issues you may encounter with installation, compilation, or execution of DOCA samples.

### To build a given sample:

```bash
cd /opt/mellanox/doca/samples/doca_urom/<sample_name>
meson /tmp/build
ninja -C /tmp/build
```

### Info

The binary `doca_<sample_name>` is created under `/tmp/build/`.

---

## UROM Sample Arguments

| Sample                | Argument                  | Description            |
|-----------------------|---------------------------|------------------------|
| **UROM multi-workers bootstrap** | `-d, --device <IB device name>` | IB device name |
| **UROM Ping Pong**    | `-d, --device <IB device name>` | IB device name |
|                       | `-m, --message`          | Specify ping pong message |

For additional information per sample, use the `-h` option:

```bash
/tmp/build/doca_<sample_name> -h
```

---

## UROM Plugin Samples

DOCA UROM plugin samples have two components:

1. **Host Component**: Linked with UROM host programs.
2. **DPU Component**: Compiled as an `.so` file and loaded at runtime by the DOCA UROM service (daemon, workers).

### To build a given plugin:

```bash
cd /opt/mellanox/doca/samples/doca_urom/plugins/worker_<plugin_name>
meson /tmp/build
ninja -C /tmp/build
```

### Info

The binary `worker_<sample_name>.so` file is created under `/tmp/build/`.

