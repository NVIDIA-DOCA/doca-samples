#
# This software product is a proprietary product of NVIDIA CORPORATION &
# AFFILIATES (the "Company") and all right, title, and interest in and to the
# software product, including all associated intellectual property rights, are
# and shall remain exclusively with the Company.
#
# This software product is governed by the End User License Agreement
# provided with the software product.
#

# GPUNetIO Samples

These samples show how CUDA kernels use DOCA GPUNetIO to drive NVIDIA network devices directly from the GPU: Ethernet send and receive queues, RDMA and DOCA Verbs queue pairs, and DMA copies.

| Sample | What it shows | Setup |
|--------|---------------|-------|
| gpunetio_send_wait_time | Ethernet send at times scheduled in the NIC (Accurate Send Scheduling) | One host, plus a capture point on the link |
| gpunetio_simple_send | Ethernet send from a CUDA kernel, with an optional shared send queue or CPU proxy | One host, plus a capture point on the link |
| gpunetio_simple_receive | Ethernet receive into GPU memory, with DOCA Flow steering UDP traffic to the GPU | One host, plus a UDP traffic generator |
| gpunetio_rdma_client_server_write | DOCA RDMA Write and Send with Immediate from CUDA kernels | Server and client |
| gpunetio_dma_memcpy | DOCA DMA copies between host and GPU memory, from the CPU and from a CUDA kernel | One host with a BlueField device |
| gpunetio_verbs_write_bw | RDMA Write bandwidth, posting WQEs with the low-level DOCA Verbs device functions | Server and client |
| gpunetio_verbs_write_lat | RDMA Write ping-pong latency | Server and client |
| gpunetio_verbs_put_bw | RDMA Write bandwidth with doca_gpu_dev_verbs_put() on a shared QP | Server and client |
| gpunetio_verbs_put_signal_bw | Bandwidth of puts with a signal: RDMA Write plus an atomic add on a remote flag | Server and client |
| gpunetio_verbs_put_signal_lat | Ping-pong latency of puts with a signal | Server and client |
| gpunetio_verbs_put_counter_bw | Bandwidth of puts with a counter: RDMA Write, then an atomic add once the write completed | Server and client |
| gpunetio_verbs_put_counter_lat | Ping-pong latency of puts with a counter | Server and client |
| gpunetio_verbs_get_bw | RDMA Read bandwidth with doca_gpu_dev_verbs_get() | Server and client |
| gpunetio_verbs_twosided_bw | RDMA Send and Receive bandwidth | Server and client |

## Info

    All the DOCA samples described in this section are governed under the BSD-3 software license agreement.

## Build the Samples

Each sample directory contains the sample sources and a meson.build. The directories under /opt/mellanox/doca belong to root, so build each sample in a directory of your own. Set the environment, then build the sample:

```plaintext
export PATH=${PATH}:/usr/local/cuda/bin
export CPATH="$(echo /usr/local/cuda/targets/{x86_64,sbsa}-linux/include | sed 's/ /:/'):${CPATH}"
export PKG_CONFIG_PATH=${PKG_CONFIG_PATH}:/opt/mellanox/doca/lib/$(uname -m)-linux-gnu/pkgconfig
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/usr/local/cuda/lib64:/opt/mellanox/doca/lib/$(uname -m)-linux-gnu

mkdir -p ~/<sample> && cd ~/<sample>
meson setup build /opt/mellanox/doca/samples/doca_gpunetio/<sample>
ninja -C build
```

The binary is `build/doca_<sample>`, for example `build/doca_gpunetio_simple_send`, and the commands below run it from that directory. To modify a sample, for example its meson.build or its CUDA kernel, first copy /opt/mellanox/doca/VERSION, /opt/mellanox/doca/samples and /opt/mellanox/doca/applications/common to a directory of your own, keeping the same layout: each meson.build refers to them with relative paths.

The build needs CUDA 12.1 or newer and meson 0.61.2 or newer, and uses the release build type by default. gpunetio_dma_memcpy and gpunetio_rdma_client_server_write also link the DOCA GPUNetIO device library, libdoca_gpunetio_device.a, so they need at least the CUDA version that DOCA was built with. `cuobjdump -elf /opt/mellanox/doca/lib/$(uname -m)-linux-gnu/libdoca_gpunetio_device.a | grep -m1 'CUDA Tool Kit Version'` prints that version. With an older CUDA, their link fails with "newer than toolkit". The DOCA Verbs samples also need the libibverbs and libmlx5 pkg-config files, from the rdma-core development package.

> **Note:** Each meson.build compiles the CUDA kernels for sm_75, sm_80 and sm_90, plus sm_70 with CUDA older than 13 and sm_100 with CUDA 12.8 or newer. gpunetio_send_wait_time also compiles sm_103, sm_110, sm_120 and sm_121 with CUDA 13 or newer. The sm_80 code also runs on other 8.x GPUs, such as A10 or L40, and the sm_100 code on 10.3 GPUs. For any other GPU, add its -gencode entries to both nvcc_flags and nvcc_flags_link in meson.build. Otherwise the kernel launch fails with "no kernel image is available for execution on the device".

## Common Requirements and Options

- The GPU and the NIC must support GPUDirect RDMA. The samples register GPU memory with dmabuf, and fall back to the nvidia-peermem kernel module when dmabuf is not available.
- Unless a CPU proxy mode is used, the GPU writes the NIC doorbell registers directly. See the DOCA GPUNetIO Programming Guide for the system configuration this needs.
- GDRCopy is optional. Without it, DOCA GPUNetIO prints the error "Failed to find GDRCopy library libgdrapi.so" and the warning "GDRCopy and/or gdrdrv kernel module not found", and allocates CPU-visible buffers in host memory instead of GPU memory. If GDRCopy is installed outside the library path, set GDRCOPY_PATH_L to the directory of libgdrapi.so, or add that directory to LD_LIBRARY_PATH. sudo drops both variables, so for the Ethernet samples pass them on the command line, for example `sudo GDRCOPY_PATH_L=<directory> ./build/doca_<sample> ...`.
- The Ethernet samples create raw Ethernet queues and start a DOCA Flow port, and gpunetio_simple_receive also creates DOCA Flow rules, so they run as root (sudo).
- Every sample also accepts the DOCA ARGP options: -h/--help, -v/--version, -l/--log-level, --sdk-log-level, --log-filter and -j/--json. The samples print DOCA SDK messages of WARNING level and above. Add `--sdk-log-level 50` (INFO) or `--sdk-log-level 60` (DEBUG) to see why a DOCA call failed.
- The example outputs below show only the function name and the message of each log line: the timestamp, thread ID, [DOCA] tag, level, logger name and source file:line are removed. Values in angle brackets vary from system to system. The DOCA SDK may also print warnings on stderr that are not shown, such as "adapting queue depth to 128." when DOCA Flow starts in the Ethernet samples.

---
## Ethernet Send Wait Time

The sample shows how to enable Accurate Send Scheduling (or wait-on-time) in the context of a GPUNetIO application. Accurate Send Scheduling is the ability of an NVIDIA NIC to send packets in the future according to application-provided timestamps.

> **Note:** This feature is supported on ConnectX-7 and later, with the REAL_TIME_CLOCK_ENABLE and ACCURATE_TX_SCHEDULER firmware attributes enabled.

Accurate Send Scheduling is used, for example, in 5G networks, where packets must leave at precise times.

This DOCA GPUNetIO sample provides a simple application to send packets with Accurate Send Scheduling from the GPU.

### Enabling the Feature

The real-time clock and the accurate Tx scheduler of the NIC must be enabled in its firmware. Then reset the NIC firmware or reboot the system:

```plaintext
sudo mlxconfig -d <NIC PCIe address> s REAL_TIME_CLOCK_ENABLE=1 ACCURATE_TX_SCHEDULER=1
sudo mlxfwreset -d <NIC PCIe address> reset
```

Without them, the sample stops with an error that lists these attributes.

### Synchronizing Clocks

Before starting the sample, it is important to properly synchronize the CPU clock with the NIC clock. This way, timestamps provided by the system clock are synchronized with the time in the NIC.

For this purpose, at least the phc2sys service must be used. To install it on an Ubuntu system:

```plaintext
sudo apt install linuxptp
```

To start the phc2sys service properly, a config file must be created in /lib/systemd/system/phc2sys.service:

```plaintext
[Unit]
Description=Synchronize system clock or PTP hardware clock (PHC)
Documentation=man:phc2sys
 
[Service]
Restart=always
RestartSec=5s
Type=simple
ExecStart=/bin/sh -c "taskset -c 15 /usr/sbin/phc2sys -s /dev/ptp$(ethtool -T ens6f0 | grep PTP | awk '{print $4}') -c CLOCK_REALTIME -n 24 -O 0 -R 256 -u 256"
 
[Install]
WantedBy=multi-user.target
```

Replace ens6f0 with the network interface of the NIC port given to the sample with -n (for 17:00.0: `ls /sys/bus/pci/devices/0000:17:00.0/net/`), and 15 with a CPU core of the system.

Now phc2sys service can be started:

```plaintext
sudo systemctl stop systemd-timesyncd
sudo systemctl disable systemd-timesyncd
sudo systemctl daemon-reload
sudo systemctl start phc2sys.service
```

To check the status of phc2sys:

```plaintext
$ sudo systemctl status phc2sys.service
 
● phc2sys.service - Synchronize system clock or PTP hardware clock (PHC)
     Loaded: loaded (/lib/systemd/system/phc2sys.service; disabled; vendor preset: enabled)
     Active: active (running) since Mon 2023-04-03 10:59:13 UTC; 2 days ago
       Docs: man:phc2sys
   Main PID: 337824 (sh)
      Tasks: 2 (limit: 303788)
     Memory: 560.0K
        CPU: 52min 8.199s
     CGroup: /system.slice/phc2sys.service
             ├─337824 /bin/sh -c "taskset -c 15 /usr/sbin/phc2sys -s /dev/ptp\$(ethtool -T enp23s0f1np1 | grep PTP | awk '{print \$4}') -c CLOCK_REALTIME -n 24 -O 0 -R >
             └─337829 /usr/sbin/phc2sys -s /dev/ptp3 -c CLOCK_REALTIME -n 24 -O 0 -R 256 -u 256
 
Apr 05 16:35:52 doca-vr-045 phc2sys[337829]: [457395.040] CLOCK_REALTIME rms    8 max   18 freq +110532 +/-  27 delay   770 +/-   3
Apr 05 16:35:53 doca-vr-045 phc2sys[337829]: [457396.071] CLOCK_REALTIME rms    8 max   20 freq +110513 +/-  30 delay   769 +/-   3
Apr 05 16:35:54 doca-vr-045 phc2sys[337829]: [457397.102] CLOCK_REALTIME rms    8 max   18 freq +110527 +/-  30 delay   769 +/-   3
...
```

At this point, the system and NIC clocks are synchronized so timestamps provided by the CPU are correctly interpreted by the NIC. At startup, the sample measures the offset between the two clocks and stops if it is larger than 25 milliseconds, for example when the NIC clock is in TAI and the system clock in UTC, 37 seconds apart. If the offset can't be measured, the sample only prints a warning.

> **Warning:** The timestamps you get may not reflect the real time and day. To get that, you must properly set the ptp4l service with an external grand master on the system. Doing that is out of the scope of this sample.

### Running the Sample

The sample sends 8 bursts of 32 UDP packets of 1 kB in a timed way, from the MAC address of the NIC port and 192.168.1.1:10000 to a dummy Ethernet address, 10:11:12:13:14:15, and 192.168.1.2:5000. The NIC sends a burst every t nanoseconds (command line option -t, from 1 to 2147483647), the first one 50 milliseconds after the launch. The CUDA kernel queues all the bursts at once and the NIC holds each one until its time.

| Option | Description |
|--------|-------------|
| `-n, --nic <PCIe address>` | NIC port, e.g. 17:00.0 (mandatory) |
| `-g, --gpu <PCIe address>` | GPU, e.g. ca:00.0 (mandatory) |
| `-t, --time <nanoseconds>` | Time interval between bursts, from 1 to 2147483647 (mandatory) |

The following example programs a system with GPU PCIe address ca:00.0 and NIC PCIe address 17:00.0 to send 32 packets every 5 milliseconds:

```plaintext
$ sudo ./build/doca_gpunetio_send_wait_time -n 17:00.0 -g ca:00.0 -t 5000000
```

The sample prints the following messages:

```plaintext
[main] Starting the sample
[main] Sample configuration:
	GPU ca:00.0
	NIC 17:00.0
	Interval between bursts 5000000 ns
[create_txq] Wait on time supported mode: Native
[create_txq] Creating Sample Eth Txq
[create_txq] Source MAC address <NIC port MAC address>
[create_txq] Mapping send queue buffer (<address> size 262144B dmabuf fd <fd>) with dmabuf mode
[check_clock_offset] NIC clock - host clock offset: <offset> ns (error margin <margin> ns)
[gpunetio_send_wait_time] Launching CUDA kernel to send 8 bursts of 32 packets, the first burst in 50 ms (fixed delay introduced by the sample) and the last burst in 85 ms
[gpunetio_send_wait_time] All the 256 packets have been sent
[destroy_txq] Destroying Txq
[main] Sample finished successfully
```

To verify that packets are actually sent at the right time, use a packet sniffer on the other side (e.g., tcpdump). The capture looks like this (abbreviated):

```plaintext
$ sudo tcpdump -i enp23s0f1np1 -n udp port 5000
 
17:12:23.480318 IP 192.168.1.1.10000 > 192.168.1.2.5000: UDP, length 982
...
17:12:23.480368 IP 192.168.1.1.10000 > 192.168.1.2.5000: UDP, length 982
# end of first burst of 32 packets, bump to +5ms
17:12:23.485321 IP 192.168.1.1.10000 > 192.168.1.2.5000: UDP, length 982
...
17:12:23.485369 IP 192.168.1.1.10000 > 192.168.1.2.5000: UDP, length 982
# end of second burst of 32 packets, bump to +5ms
17:12:23.490278 IP 192.168.1.1.10000 > 192.168.1.2.5000: UDP, length 982
...
```

The output should show a jump of approximately 5 milliseconds every 32 packets. Add `-A` to also print the payload, "Sent from DOCA GPUNetIO" followed by the packet number.

> **Note:** `tcpdump` may increase latency in sniffing packets and reporting the receive timestamp. As a result, the difference between bursts of 32 packets reported may be less than expected, especially with small interval times like 500 microseconds (`-t 500000`).

---
## Ethernet Simple Send

The sample shows how a CUDA kernel sends Ethernet packets with a DOCA Ethernet send queue (Txq) whose data path is on the GPU, so the CPU does no per-packet work.

The host builds one Ethernet/IPv4/UDP packet per CUDA thread in GPU memory. The packets go from the MAC address of the NIC port to a dummy Ethernet address, 10:11:12:13:14:15, and from 192.168.1.1:10000 to 192.168.1.2:5000 by default. The NIC computes the IPv4 and UDP checksums. A CUDA kernel then sends all the packets in a loop, as fast as the send completions allow, until Ctrl+C:

- By default, one CUDA block owns the Txq and prepares and submits the send WQEs with the low-level device functions.
- With `-q 1` (shared QP), two CUDA blocks share the Txq through doca_gpu_dev_eth_txq_send(), in thread, warp or block scope (`-e`).
- With `-p 1` (CPU proxy), a CPU thread rings the NIC doorbell for the GPU. Use it when the GPU can't write the NIC doorbell. It keeps one CPU core busy.

| Option | Description |
|--------|-------------|
| `-n, --nic <PCIe address>` | NIC port, e.g. 17:00.0 (mandatory) |
| `-g, --gpu <PCIe address>` | GPU, e.g. ca:00.0 (mandatory) |
| `-s, --size <bytes>` | Packet size, Ethernet header included and FCS excluded, from 42 to 9216 (mandatory). Above 1514 bytes, the port MTU must allow it |
| `-t, --threads <number>` | Number of CUDA threads, which is also the number of packets sent per iteration (default 512). Without -q 1, a multiple of 32 up to 1024; with -q 1, a multiple of 64 up to 2048 |
| `-q, --shared-qp <0\|1>` | Shared QP mode (default 0) |
| `-e, --exec-scope <0\|1\|2>` | With -q 1: thread (0), warp (1) or block (2) scope (default 2). The warp scope needs a GPU of compute capability 8.0 or newer |
| `-p, --proxy <0\|1>` | CPU proxy mode (default 0) |
| `--src-ip`, `--dst-ip <IPv4 address>` | IPv4 source and destination of the packets (default 192.168.1.1 and 192.168.1.2) |

The following example sends 1024-byte packets from NIC port 17:00.0 with GPU ca:00.0:

```plaintext
$ sudo ./build/doca_gpunetio_simple_send -n 17:00.0 -g ca:00.0 -s 1024
[main] Starting the sample
[main] Sample configuration:
	GPU ca:00.0
	NIC 17:00.0
	Packet size 1024
	IPv4 192.168.1.1 -> 192.168.1.2, UDP port 10000 -> 5000
	CUDA threads 512
	CPU Proxy No
	Shared QP No
[create_txq] Creating Sample Eth Txq
[create_txq] Source MAC address <NIC port MAC address>
[create_txq] Mapping send queue buffer (<address> size 524288B dmabuf fd <fd>) with dmabuf mode
[gpunetio_simple_send] Launching CUDA kernel to send packets.
[gpunetio_simple_send] Waiting for ctrl+c termination
 
# Type Ctrl+C to stop the sample
 
[gpunetio_simple_send] Signal received, exiting from sample
[destroy_txq] Destroying Txq
[main] Sample finished successfully
```

If the NIC reports a send error, the kernel stops, and the sample prints "CUDA kernel stopped sending because of a send completion error" and finishes with errors.

The packets bypass the Linux network stack of the sender, so tcpdump on the sending interface does not show them. To check them, watch the port counters of the sender (`ethtool -S <interface> | grep tx_packets_phy`), or capture on the peer port of the link: `sudo tcpdump -i <interface> -n -e udp port 5000` shows the packets from the sender's port MAC address to 10:11:12:13:14:15.

> **Note:** The destination MAC address is a dummy one, so a switch floods the packets to all its ports in the VLAN. Prefer a direct link between the two ports.

---
## Ethernet Simple Receive

The sample shows how a CUDA kernel receives Ethernet packets directly into GPU memory, with a DOCA Ethernet receive queue (Rxq) whose data path is on the GPU.

DOCA Flow steers every IPv4/UDP packet that reaches the NIC port to the Rxq, whatever its addresses and ports. Other traffic, such as ARP, ICMP or TCP, still goes to the Linux kernel. A persistent CUDA kernel of one block of 32 threads receives the packets with doca_gpu_dev_eth_rxq_recv(), in thread, warp or block scope (`-e`), and counts them. The packets can be up to 2048 bytes long. When the sample stops with Ctrl+C, it prints the number of received packets.

> **Note:** While the sample runs, the Linux kernel no longer receives UDP traffic on that port (DHCP, DNS, VXLAN...): use a dedicated port.

| Option | Description |
|--------|-------------|
| `-n, --nic <PCIe address>` | NIC port, e.g. 17:00.1 (mandatory) |
| `-g, --gpu <PCIe address>` | GPU, e.g. ca:00.0 (mandatory) |
| `-e, --exec-scope <0\|1\|2>` | Receive scope: thread (0), warp (1) or block (2) (default 2) |

To test the sample, this guide assumes two machines: one runs the sample, and the other one generates UDP packets with nping, which can easily be installed on any Linux machine. On the receiving machine, give the port an IP address, so that the generator can resolve its MAC address with ARP, then start the sample:

```plaintext
$ sudo ip link set <interface> up
$ sudo ip addr add 192.168.1.1/24 dev <interface>
$ sudo ./build/doca_gpunetio_simple_receive -n 17:00.1 -g ca:00.0
```

On the generator machine, give the port linked to the receiving machine an address in the same subnet, so that the packets to 192.168.1.1 leave through it, then send 10 UDP packets to the receiving machine:

```plaintext
$ sudo ip link set <interface> up
$ sudo ip addr add 192.168.1.2/24 dev <interface>
$ nping --udp -c 10 -p 2090 192.168.1.1 --data-length 1024 --delay 500ms
 
Starting Nping 0.7.80 ( https://nmap.org/nping ) at 2023-11-20 11:05 UTC
SENT (0.0018s) UDP packet with 1024 bytes to 192.168.1.1:2090
SENT (0.5018s) UDP packet with 1024 bytes to 192.168.1.1:2090
...
SENT (4.5048s) UDP packet with 1024 bytes to 192.168.1.1:2090
 
Max rtt: N/A | Min rtt: N/A | Avg rtt: N/A
UDP packets sent: 10 | Rcvd: 0 | Lost: 10 (100.00%)
Nping done: 1 IP address pinged in 5.50 seconds
```

The receiving machine prints:

```plaintext
[main] Starting the sample
[main] Sample configuration:
	GPU ca:00.0
	NIC 17:00.1
	Receive exec scope Block
[create_rxq] Creating Sample Eth Rxq
[create_rxq] Mapping receive queue buffer (<address> size 33554432B dmabuf fd <fd>) with dmabuf mode
[gpunetio_simple_receive] Launching CUDA kernel to receive packets
[gpunetio_simple_receive] Waiting for ctrl+c termination
 
# Type Ctrl+C to stop the sample
 
[gpunetio_simple_receive] Signal received, exiting from sample
[gpunetio_simple_receive] Total number of received packets: 10
[destroy_rxq] Destroying Rxq
[main] Sample finished successfully
```

Any other IPv4/UDP packet that reaches the port while the sample runs is counted too. The kernel prints nothing per packet. To print one line per received packet, with its MAC addresses, size and timestamp, set ENABLE_DEBUG to 1 in device/gpunetio_simple_receive_kernel.cu and rebuild.

> **Warning:** Invoking a `printf` from a CUDA kernel is not good practice for release software. It should only be used to print debug information, as it slows down the overall execution of the CUDA kernel.

---
## RDMA Client Server

This sample exhibits how to use the GPUNetIO RDMA API: CUDA kernels post RDMA Write with Immediate, RDMA Send with Immediate and RDMA Receive operations on a DOCA RDMA context whose data path is on the GPU.

The server has a GPU buffer array A of 4 buffers of 256 bytes. The client has two GPU buffer arrays, B and C, of 4 buffers of 128 bytes each. The client fills each server buffer A[i] with B[i] at offset 0 and C[i] at offset 128. Even buffers are sent with RDMA Write with Immediate, and odd buffers with RDMA Send with Immediate. For each buffer, the CUDA kernels repeat this handshake:

1. The server kernel posts two RDMA Receive operations, then writes a flag into the client's GPU memory to tell that they are posted.
2. The client kernel waits for the flag, then each of its two threads posts the operation for B[i] or C[i].
3. The server kernel waits for the two completions and prints their immediate values.

The server prints the first 4 bytes at offsets 0 and 128 of each buffer, before and after the kernels: the bytes are 1 before, then 2 at offset 0 (from B) and 3 at offset 128 (from C).

| Option | Description |
|--------|-------------|
| `-d, --device <name>` | IB device name, e.g. mlx5_0 (mandatory) |
| `-gpu, --gpu <PCIe address>` | GPU, e.g. ca:00.0 (mandatory) |
| `-c, --client <server IPv4 address>` | Run as client, and connect to the server at this address. Without it, the sample runs as server |
| `-gid, --gid-index <index>` | GID index (default 0). On RoCE, choose the RoCE v2 GID of the port IP address on both sides. With -cm, RDMA CM selects the GID of the connection |
| `-cm, --use-rdma-cm` | Connect with RDMA CM. Needed on both sides |
| `-p, --port <port>` | RDMA CM port, the same on both sides (default 13579) |
| `-sa, --server-addr <address>` | With -cm, on the client only: address of the server's RDMA port (mandatory then) |
| `-sat, --server-addr-type <ipv4\|ipv6\|gid>` | With -cm, on the client only: type of the -sa address (default ipv4). gid works on InfiniBand only |

> **Note:** With RDMA, the network device must be specified by name (e.g., mlx5_0) instead of the PCIe address (as is the case for Ethernet).

Start the server first: it listens on TCP port 2000 for the client. The two sides exchange their memory information over this connection, and, without -cm, the details of the RDMA connection. The server output should be as follows:

```plaintext
$ ./build/doca_gpunetio_rdma_client_server_write -d mlx5_0 -gpu ca:00.0
[main] Starting the sample
[oob_connection_server_setup] Socket created successfully
[oob_connection_server_setup] Done with binding
[oob_connection_server_setup] Listening for incoming connections
[oob_connection_server_setup] Client connected at IP: <client IP> and port: <port>
...
[rdma_write_server] Before launching CUDA kernel, buffer array A is:
[rdma_write_server] Buffer 0 -> offset 0: 1111 | offset 128: 1111
[rdma_write_server] Buffer 1 -> offset 0: 1111 | offset 128: 1111
[rdma_write_server] Buffer 2 -> offset 0: 1111 | offset 128: 1111
[rdma_write_server] Buffer 3 -> offset 0: 1111 | offset 128: 1111
RDMA Recv 2 ops completed with immediate values 0 and 1! Connections 0 and 0
RDMA Recv 2 ops completed with immediate values 1 and 2! Connections 0 and 0
RDMA Recv 2 ops completed with immediate values 2 and 3! Connections 0 and 0
RDMA Recv 2 ops completed with immediate values 3 and 4! Connections 0 and 0
[rdma_write_server] After launching CUDA kernel, buffer array A is:
[rdma_write_server] Buffer 0 -> offset 0: 2222 | offset 128: 3333
[rdma_write_server] Buffer 1 -> offset 0: 2222 | offset 128: 3333
[rdma_write_server] Buffer 2 -> offset 0: 2222 | offset 128: 3333
[rdma_write_server] Buffer 3 -> offset 0: 2222 | offset 128: 3333
[main] Sample finished successfully
```

On the other side, assuming the server is at IP address 192.168.1.1, the client output should be as follows:

```plaintext
$ ./build/doca_gpunetio_rdma_client_server_write -d mlx5_0 -gpu ca:00.0 -c 192.168.1.1
[main] Starting the sample
[oob_connection_client_setup] Socket created successfully
[oob_connection_client_setup] Connected with server successfully
...
[rdma_write_client] Client, terminate kernels
Client waiting on flag <address> for server to post RDMA Recvs
Thread 0 post rdma write imm 0
Thread 1 post rdma write imm 1
Client waiting on flag <address> for server to post RDMA Recvs
Thread 0 post rdma send imm 1
Thread 1 post rdma send imm 2
Client waiting on flag <address> for server to post RDMA Recvs
Thread 0 post rdma write imm 2
Thread 1 post rdma write imm 3
Client waiting on flag <address> for server to post RDMA Recvs
Thread 0 post rdma send imm 3
Thread 1 post rdma send imm 4
Client posted and completed 4 RDMA commits on connection 0. Waiting on the exit flag.
[main] Sample finished successfully
```

It is also possible to enable the RDMA CM mode, establishing two connections with the same RDMA GPU handler: the client then runs the exchange once per connection, and the server prints its buffers again for the second connection. Pass -cm, and the same -p if not the default one, on both sides. The client still needs -c for the TCP connection, plus -sa with the address of the server's RDMA port, which may differ from the -c address. The server rejects -sa and ignores -sat:

```plaintext
# Server
$ ./build/doca_gpunetio_rdma_client_server_write -d mlx5_0 -gpu ca:00.0 -cm
# Client
$ ./build/doca_gpunetio_rdma_client_server_write -d mlx5_0 -gpu ca:00.0 -c 192.168.1.1 -cm -sa 192.168.1.1 -sat ipv4
```

> **Warning:** Printing from a CUDA kernel is not recommended for performance. It may make sense for debugging purposes and for simple samples like this one.

---
## GPU DMA Copy

This sample exhibits how to use the DOCA DMA and DOCA GPUNetIO libraries to copy a memory buffer with a BlueField device, in both directions between host memory and GPU memory:

1. From the CPU to the GPU, with the DOCA DMA CPU functions: the CPU copies "This is a sample piece of text from CPU" from host memory to a GPU buffer.
2. From the GPU to the CPU, with the DOCA GPUNetIO DMA device functions: a CUDA kernel prints the GPU buffer of the first copy, then copies "This is a sample piece of text from GPU" from GPU memory to a host buffer. The CPU waits for the text and prints it.

Each copy moves 1024 bytes: the text and the zeros that follow it.

> **Note:** The sample requires a BlueField device, which provides the DMA engine: `-n` must be the PCIe address of a BlueField function. On a ConnectX NIC, the sample fails with "DOCA device <PCIe address> doesn't support DMA memcpy, a BlueField device is required".

| Option | Description |
|--------|-------------|
| `-n, --nic <BlueField PCIe address>` | BlueField function, e.g. 17:00.0 (mandatory) |
| `-g, --gpu <PCIe address>` | GPU, e.g. ca:00.0 (mandatory) |

```plaintext
$ ./build/doca_gpunetio_dma_memcpy -n 17:00.0 -g ca:00.0
[main] Starting the sample
[main] Sample configuration:
	GPU ca:00.0
	BlueField 17:00.0
[create_cpu_to_gpu_copy] The CPU source buffer value to be copied to GPU memory: This is a sample piece of text from CPU
[create_dma_buffer] Mapping GPU destination buffer (<address> size 4096B dmabuf fd <fd>) with dmabuf mode
[create_dma_buffer] Mapping GPU source buffer (<address> size 4096B dmabuf fd <fd>) with dmabuf mode
[create_gpu_to_cpu_copy] The GPU source buffer value to be copied to CPU memory: This is a sample piece of text from GPU
[run_cpu_to_gpu_copy] Success, DMA memcpy job done successfully
CUDA KERNEL INFO: The GPU destination buffer value after the memcpy: This is a sample piece of text from CPU
[run_gpu_to_cpu_copy] CPU received message from GPU: This is a sample piece of text from GPU
[main] Sample finished successfully
```

Each buffer takes one host page, so the mapping lines show 65536B on systems with 64KB pages. The sample doesn't need GDRCopy: it writes the GPU source buffer with cudaMemcpy.

The DOCA GPUNetIO DMA device functions don't report the completion of a copy, to the GPU or to the CPU. The CPU therefore polls the host buffer until it holds the whole text. If the text doesn't arrive within 1 second, the sample fails with "The DMA copy from the GPU didn't complete within 1000 ms".

---
## DOCA Verbs Samples

The gpunetio_verbs_* samples measure the bandwidth or the latency of RDMA operations that CUDA kernels post directly with the DOCA GPUNetIO Verbs device functions. Each sample creates RC queue pairs (QPs) with DOCA Verbs, places their queues in GPU memory and exports them to the GPU.

| Sample | Operation | Result | CUDA kernels on |
|--------|-----------|--------|-----------------|
| gpunetio_verbs_write_bw | RDMA Write, with the low-level WQE functions | Bandwidth | Client |
| gpunetio_verbs_write_lat | RDMA Write ping-pong | Latency | Both sides |
| gpunetio_verbs_put_bw | RDMA Write with doca_gpu_dev_verbs_put() | Bandwidth | Client |
| gpunetio_verbs_put_signal_bw | Put with signal: RDMA Write followed by an atomic fetch-and-add on a remote flag | Bandwidth | Client |
| gpunetio_verbs_put_signal_lat | Put with signal ping-pong | Latency | Both sides |
| gpunetio_verbs_put_counter_bw | Put with counter: RDMA Write followed, once the write has completed, by an atomic fetch-and-add on a remote counter | Bandwidth | Client |
| gpunetio_verbs_put_counter_lat | Put with counter ping-pong | Latency | Both sides |
| gpunetio_verbs_get_bw | RDMA Read with doca_gpu_dev_verbs_get() | Bandwidth | Client |
| gpunetio_verbs_twosided_bw | RDMA Send and Receive | Bandwidth | Both sides |

Every sample measures 14 message sizes, from 1 byte to 256 KiB, and runs an untimed warmup kernel before the timed kernel of each size.

### Connection

Each sample runs as a server and a client, normally on two hosts, each with an NVIDIA GPU and an RDMA NIC port, on InfiniBand or RoCE. Start the server first, without `-c`: it listens on TCP port 2000 and accepts one client. Once the server has printed "Listening for incoming connections", start the client, with `-c` and an IPv4 address of the server. The client tries to connect only once, and any address of the server works, for example the one of a management interface: the TCP connection only exchanges the options, the buffer addresses and keys, and the QP information, while the data moves on the `-d` port.

Both sides must run the same sample with the same `-i` and `-t`, and the same `-x` for gpunetio_verbs_twosided_bw: the samples check these options over the TCP connection and stop on a mismatch. The other options apply to one side only.

### Command Line Options

| Option | Description |
|--------|-------------|
| `-d, --device <name>` | IB device name, e.g. mlx5_0 (mandatory). Port 1 is used |
| `-g, --gpu <PCIe address>` | GPU, e.g. ca:00.0 (mandatory, on both sides) |
| `-c, --client <server IPv4 address>` | Run as client, and connect to the server at this address. Without it, the sample runs as server |
| `-gid, --gid-index <index>` | GID index of port 1 (default 0). On RoCE, choose the RoCE v2 GID of the port IPv4 address, for example with show_gids |
| `-i, --iters <number>` | Iterations per message size (default 2048). In the bandwidth samples, the number of operations per kernel, a multiple of -t |
| `-t, --thread <number>` | Number of CUDA threads of the bandwidth samples (default 512), even and at least 64 |
| `-n, --nic-handler <0-5>` | How the NIC doorbell is rung (default 0, see below). Not available in gpunetio_verbs_twosided_bw |
| `-e, --exec-shared <0\|1>` | In put_bw, put_signal_bw, put_counter_bw, get_bw and twosided_bw: shared QP scope, per thread (0, default) or per warp (1). The warp scope needs a GPU of compute capability 8.0 or newer, and `-t` a multiple of 64 (a multiple of 32 in gpunetio_verbs_twosided_bw) |
| `-z, --data-direct <0\|1>` | Data Direct (default 0, see below). Not available in gpunetio_verbs_get_bw and gpunetio_verbs_twosided_bw |
| `-x, --inline <0\|1>` | gpunetio_verbs_twosided_bw only: receive inline (default 0) |

The NIC handler (`-n`) selects how the NIC doorbell is rung after posting WQEs:

- 0, AUTO: GPU_SM_DB if the GPU can map the NIC doorbell, otherwise CPU_PROXY. The sample logs the choice: "NIC handler AUTO resolved to ...".
- 1, CPU_PROXY: a CPU thread rings the doorbell for the GPU. It keeps one CPU core busy and adds latency.
- 2, GPU_SM_DB: the GPU updates the doorbell record and rings the doorbell.
- 3, GPU_SM_BF: BlueFlame. Only gpunetio_verbs_write_lat posts through BlueFlame, on GPUs of compute capability 9.0 or newer. The bandwidth samples write_bw, put_bw, put_signal_bw, put_counter_bw and get_bw reject it, and so do put_counter_lat and put_signal_lat.
- 4, GPU_SM_NO_DBR, and 5, CPU_PROXY_NO_DBR: like 2 and 1, without updating the doorbell record. They need NIC support.

In the samples where only the client runs CUDA kernels, the server ignores `-n`.

Data Direct (`-z 1`) registers the GPU buffers of one side through the Data Direct device of its NIC. It needs a sample built with CUDA 12.8 or newer and libmlx5 headers that support Data Direct, and a NIC with a Data Direct device. In the bandwidth samples where the server only receives, `-z 1` on the server also moves its receive buffers from host memory to GPU memory. Pass `-z 1` to both sides to use Data Direct at both ends.

### Running the Samples

All the samples run the same way, for example gpunetio_verbs_write_bw:

```plaintext
# Server, at IP address 192.168.1.1
$ ./build/doca_gpunetio_verbs_write_bw -d mlx5_0 -g ca:00.0
# Client
$ ./build/doca_gpunetio_verbs_write_bw -d mlx5_0 -g ca:00.0 -c 192.168.1.1
```

The server output should be as follows:

```plaintext
[main] Starting the sample
[create_verbs_resources] NIC handler AUTO resolved to GPU_SM_DB
[oob_verbs_connection_server_setup] Socket created successfully
[oob_verbs_connection_server_setup] Done with binding
[oob_verbs_connection_server_setup] Listening for incoming connections
[oob_verbs_connection_server_setup] Client connected at IP: <client IP> and port: <port>
[connect_verbs_qp] QP has been successfully connected and ready to use
[verbs_server] Waiting for the client to complete the benchmark
[server_validate_test] Validation successful! Data received correctly from client
[main] Sample finished successfully
```

The client output should be as follows:

```plaintext
[main] Starting the sample
[create_verbs_resources] NIC handler AUTO resolved to GPU_SM_DB
[doca_gpu_verbs_create_qp_hl] doca_verbs_cq_set_cq_context set with context <address>
[oob_verbs_connection_client_setup] Socket created successfully
[oob_verbs_connection_client_setup] Connected with server successfully
[connect_verbs_qp] QP has been successfully connected and ready to use
[verbs_client] Launching gpunetio_verbs_write_bw kernel with 1 CUDA Blocks, 512 CUDA threads, 2048 total number of iterations, 4 iterations per cuda thread, GPU_SM_DB nic handler mode
------------------------------------------------------------------------------------
 #bytes     #iterations    BW average[Gbps]   MsgRate[Mpps]    CUDA Kernel[ms]
------------------------------------------------------------------------------------
<one line per message size, from 1 to 262144 bytes>
[main] Sample finished successfully
```

In the bandwidth table, BW average counts the payload bytes and MsgRate the operations, over the duration of the timed kernel (CUDA Kernel). The latency samples print this table on both sides, where Unidirectional is half a round trip and Bidirectional a full round trip:

```plaintext
----------------------------------------------------------------------------------------------
 #bytes 	#iterations    Unidirectional[usec]    Bidirectional[usec]    CUDA Kernel[usec]
----------------------------------------------------------------------------------------------
<one line per message size, from 1 to 262144 bytes>
```

> **Note:** The samples use a fixed QP path MTU of 4096 bytes, so port 1 of the `-d` device must have an active MTU of 4096 on both sides (active_mtu in ibv_devinfo). On RoCE, this needs an MTU of 4200 bytes or more on both network interfaces and on any switch between them, for example `sudo ip link set <interface> mtu 9000`. With the default Ethernet MTU of 1500, messages of 2048 bytes and more don't get through, and the run hangs or fails the data validation. If a peer stops in the middle of a run, the CUDA kernel of the other side may wait forever: stop it with Ctrl+C. Most CUDA kernels don't check the status of the completions, so a network error may show up as a hang or as a failed data validation rather than as an error message.

### gpunetio_verbs_write_bw

The client kernel, one block of `-t` threads, posts RDMA Writes from its GPU memory to the server: each thread prepares one WQE per iteration, the last thread rings the doorbell for the whole block, and the kernel waits only for the completion of the last WQE. `-t` must be at most the GPU's maximum number of threads per block (1024 on current GPUs), and `-i` at most 2048. The server receives into host memory, or GPU memory with `-z 1`, and checks every byte when the client has finished.

### gpunetio_verbs_write_lat

Both sides run a kernel of one thread. In each iteration, the server writes a message into the client's GPU memory, and the client answers with a write of the same size as soon as the last byte has arrived. Both sides print the latency table. There is no `-t` option.

### gpunetio_verbs_put_bw

The client kernel runs two blocks of `-t`/2 threads, which share one QP through doca_gpu_dev_verbs_put(): in thread scope (`-e 0`), each thread reserves its WQE and rings the doorbell; in warp scope (`-e 1`), lane 0 of each warp reserves 32 WQEs and rings the doorbell once for the warp. `-t` can be up to 2048, and `-i` has no upper limit. The server checks every byte, as in gpunetio_verbs_write_bw.

### gpunetio_verbs_put_signal_bw

Like gpunetio_verbs_put_bw, but each put is doca_gpu_dev_verbs_put_signal(): an RDMA Write followed, on the same QP, by an atomic fetch-and-add of 1 on a 64-bit flag of the server. In warp scope, one fetch-and-add is posted per warp. After the data check, the server prints the value of the flags for each message size, `Flag value for message size <size> is <value>`: 2 times `-i` in thread scope (warmup and timed kernels), and 2 times `-i`/32 in warp scope.

### gpunetio_verbs_put_signal_lat

Ping-pong of puts with signal between two kernels of one thread: each side writes the message into the peer's GPU memory and adds 1 to the peer's flag, then waits for its own flag to be incremented by the peer. The client sends first. Both sides print the latency table. There is no `-t` option.

### gpunetio_verbs_put_counter_bw

Like gpunetio_verbs_put_signal_bw, but with a QP group: a main QP for the RDMA Writes and a companion QP, which waits for the completion of each write before the atomic fetch-and-add (doca_gpu_dev_verbs_put_counter()). The counter is therefore updated only after the data is written. The NIC must support core direct (WAIT) operations. `-n 3` is not supported.

### gpunetio_verbs_put_counter_lat

Ping-pong of puts with counter between two kernels of one thread, each side with a QP group as in gpunetio_verbs_put_counter_bw, so the NICs of both sides must support core direct (WAIT) operations. The client sends first. Both sides print the latency table. There is no `-t` option, and `-n 3` is not supported.

### gpunetio_verbs_get_bw

The client kernel runs two blocks of `-t`/2 threads, which share one QP and read the server's host memory into the client's GPU memory with RDMA Reads (doca_gpu_dev_verbs_get()). The client checks the data it read, and prints "Validation successful! Data received correctly from server". `-t` can be up to 2048.

### gpunetio_verbs_twosided_bw

Both sides run a kernel of one block of `-t` threads, up to 1024. In each round, the server posts `-t` receives and tells the client with an RDMA Write on a flag, and the client then posts `-t` sends. Both sides print the bandwidth table, and the server checks the received data: "Data validation successful! Data received correctly from client". The GPU must be able to ring the NIC doorbell, as there is no `-n` option. With `-x 1`, the message sizes are 1, 4, 8, 12, 16, 20, 24, 28, 30 and 32 bytes, and the payload arrives inside the receive completion (CQE inline), which the server kernel checks. It needs NIC support on both sides, and is not meant for performance measurements.
