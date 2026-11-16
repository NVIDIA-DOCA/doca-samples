<!--
This software product is a proprietary product of NVIDIA CORPORATION &
AFFILIATES (the "Company") and all right, title, and interest in and to the
software product, including all associated intellectual property rights, are
and shall remain exclusively with the Company.

This software product is governed by the End User License Agreement
provided with the software product.
-->

# DOCA Memos Target Sample

This sample illustrates how to bring up a DOCA Memos target subsystem that exposes one namespace,
backed by a regular file, over a single NVMe-oF RDMA port. The transport address, service id (port
number) and backing file are taken from the command line. The sample runs until interrupted with
Ctrl-C.

The sample is built entirely on the DOCA Memos target public API together with the DOCA progress
engine, polled from a single thread.

## Usage

The sample requires three mandatory arguments:

- `-a, --addr <ip>` — transport address (IPv4 or IPv6) to listen on
- `-p, --port <port>` — transport service id (port number)
- `-f, --file <path>` — backing file for the namespace

```sh
doca_memos_simple_target -a 192.168.1.1 -p 4420 -f /tmp/memos_ns.img
```

## Sample Logic

1. Registers the buffer allocator and every DOCA Memos target capable device, then initializes the DOCA
   Memos target.
2. Opens and starts the target-level I/O context and management context.
3. Starts the DOCA Memos target, which freezes the set of I/O and management contexts.
4. Creates the target-level listener port.
5. Creates and starts the subsystem, registering its namespace attach/detach, backend add/remove,
   port add/remove, and start/stop completion handlers.
6. Adds the listener port to the subsystem.
7. Adds a file-backed namespace, including creating the namespace, adding a backend for the backing
   file with its write granularity and optimal write size set, and attaching it to the subsystem.
8. Serves READ/WRITE requests against the backing file until interrupted, then tears everything down
   in reverse order.

## References
- `memos_simple_target/memos_simple_target_sample.c`
- `memos_simple_target/memos_simple_target_main.c`
- `memos_simple_target/meson.build`
