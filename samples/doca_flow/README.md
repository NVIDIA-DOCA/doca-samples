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

### Program Flags:
- `-a, --dev <DEVICE-ADDRESS>`              Device address (e.g., pci/03:00.0)

For additional information per sample, use the `-h` option:

```sh
/tmp/build/samples/<sample_name> -h
```

### CLI Example

The following is a CLI example for running the samples when port `03:00.0` is configured (multi-port e-switch) as manager port:

```sh
/tmp/build/samples/doca_<sample_name> -- -a pci/03:00.0 -l 60
```

**Info:** To avoid the test being impacted by unexpected packets, it only accepts packets like the following examples:

- IPv4 destination address is `1.1.1.1`
- IPv6 destination address is `0101:0101:0101:0101:0101:0101:0101:0101`

## Samples

**Note:** All CT UDP samples demonstrate the usage of the connection's duplication filter. Duplication filter is used if the user is interested in preventing same connection rule insertion in a high-rate workload environment.

### Flow CT 2 Ports

This sample illustrates how to create a simple pipeline on two standalone e-switches. Multi-port e-switch must be disabled.

```sh
sudo devlink dev eswitch set pci/<pcie-address0> mode switchdev
sudo devlink dev eswitch set pci/<pcie-address1> mode switchdev
sudo devlink dev param set pci/<pcie-address0> name esw_multiport value false cmode runtime
```

#### Sample Logic

- Initializing DOCA Flow by indicating `mode_args="switch,hws"` in the `doca_flow_cfg` struct.
- Initializing DOCA Flow CT.
- Starting two DOCA Flow uplink ports where port 0 and 1 each has a special role of being a switch manager port.

**Info:** Ports are configured according to the parameters provided to `doca_dpdk_port_probe()` in the main function.

#### Creating a Pipeline on Each Port

- Building an UDP pipe to filter non-UDP packets.
- Building a CT pipe with built-in counter to hold UDP session entries.
- Building a counter pipe for CT miss packets (CT cannot count miss packets).
- Building a port forward pipe for direct port forwarding of CT match packets.
- Building an RSS software pipe from which CT miss packets are directed to the sample main thread for parsing and processing.

#### Packet Processing on Each Port

The sample demonstrates two different packet processing paths:

**CT MISS Path (Software Processing):**
- The first UDP packet triggers the miss flow as the CT pipe is empty.
- Performing 5-tuple packet parsing.
- Calling `doca_flow_ct_add_entry()` to create a hardware rule according to the parsed 5-tuple info.
- Miss packets are forwarded through Counter Miss Pipe -> RSS Software Pipe -> Queue 0 -> Software processing.

**CT MATCH Path (Direct Port Forwarding):**
- The second UDP packet based on the same 5-tuple hits the hardware rule inserted before.
- Match packets are forwarded through CT Pipe (with built-in counting) -> Port Forward Pipe -> Direct port forwarding.

**Reference:**

- `doca_flow/flow_ct_2_ports/flow_ct_2_ports_sample.c`
- `doca_flow/flow_ct_2_ports/flow_ct_2_ports_main.c`
- `doca_flow/flow_ct_2_ports/meson.build`

### Flow CT UDP

This sample illustrates how to create a simple UDP pipeline with a CT pipe in it.

#### Sample Logic

- Initializing DOCA Flow by indicating `mode_args="switch,hws"` in the `doca_flow_cfg` struct.
- Initializing DOCA Flow CT.
- Starting two DOCA Flow uplink representor ports where port 0 has a special role of being a switch manager port.

**Info:** Ports are configured according to the parameters provided to `doca_dpdk_port_probe()` in the main function.

#### Creating a Pipeline on the Main Port

- Building an UDP pipe to filter non-UDP packets.
- Building a CT pipe to hold UDP session entries.
- Building a counter pipe with an example 5-tuple entry to which non-unidentified UDP sessions should be sent.
- Building a VXLAN encapsulation pipe to encapsulate all identified UDP sessions.
- Building an RSS pipe from which all packets are directed to the sample main thread for parsing and processing.

#### Packet Processing

- The first UDP packet triggers the miss flow as the CT pipe is empty.
- 5-tuple packet parsing is performed.
- `doca_flow_ct_add_entry()` is called to create a hardware rule according to the parsed 5-tuple info.
- The second UDP packet based on the same 5-tuple should be sent again. Packet hits the HW rule inserted before and directed to port 0 after VXLAN encapsulation.

**Reference:**

- `doca_flow/flow_ct_udp/flow_ct_udp_sample.c`
- `doca_flow/flow_ct_udp/flow_ct_udp_main.c`
- `doca_flow/flow_ct_udp/meson.build`

### Flow CT UDP Query

This sample illustrates how to query a Flow CT UDP session entry. The query can be done according to session direction (origin or reply). The pipeline is identical to that of the Flow CT UDP sample.

#### Additional Logic

- Dumping port 0 information into a file at `./port_0_info.txt`.
- Querying UDP session hardware entry created after receiving the first UDP packet:
  - Origin total bytes received
  - Origin total packets received
  - Reply total bytes received
  - Reply total packets received

**Reference:**

- `doca_flow/flow_ct_udp_query/flow_ct_udp_query_sample.c`
- `doca_flow/flow_ct_udp_query/flow_ct_udp_query_main.c`
- `doca_flow/flow_ct_udp_query/meson.build`

### Flow CT UDP Tunnel Asymmetric

This sample illustrates how the usage of the tunnel_asymmetric flag. The logic is identical to that of the Flow CT UDP Query sample.

#### Additional Logic

- Creating a tunnel in the reply direction

**Reference:**

- `doca_flow/flow_ct_udp_tunnel_asymmetric/flow_ct_udp_tunnel_asymmetric_sample.c`
- `doca_flow/flow_ct_udp_tunnel_asymmetric/flow_ct_udp_tunnel_asymmetric_main.c`
- `doca_flow/flow_ct_udp_tunnel_asymmetric/meson.build`

### Flow CT UDP Update

This sample illustrates how a CT entry can be updated after creation.

The pipeline is identical to that of the Flow CT UDP sample. In case of non-active UDP sessions, a relevant entry shall be updated with an aging timeout.

#### Additional Logic

- Querying all UDP sessions for the total number of packets received in both the origin and reply directions.
- Updating entry aging timeout to 2 seconds once a session is not active (i.e., no packets received on either side).
- Waiting until all non-active sessions are aged and deleted.

**Reference:**

- `doca_flow/flow_ct_udp_update/flow_ct_udp_update_sample.c`
- `doca_flow/flow_ct_udp_update/flow_ct_udp_update_main.c`
- `doca_flow/flow_ct_udp_update/meson.build`

### Flow CT UDP Single Match

This sample is based on the Flow CT UDP sample. The sample illustrates that a hardware entry can be created with a single match (matching performed in one direction only) in the API call `doca_flow_ct_add_entry()`.

### Flow CT Aging

This sample illustrates the use of the DOCA Flow CT aging functionality. It demonstrates how to build a pipe and add different entries with different aging times and user data.

No packets need to be sent for this sample.

#### Sample Logic

- Initializing DOCA Flow by indicating `mode_args="switch,hws"` in the `doca_flow_cfg` struct.
- Initializing DOCA Flow CT.
- Starting two DOCA Flow uplink representor ports where port 0 has a special role of being a switch manager port.

**Info:** Ports are configured according to the parameters provided to `doca_dpdk_port_probe()` in the main function.

- Building a UDP pipe to serve as the root pipe.
- Building a counter pipe with an example 5-tuple entry to which CT forwards packets.
- Adding 32 entries with a different 5-tuple match, different aging time (3-12 seconds), and setting user data. User data will contain the port ID, entry number, and status.
- Handling aging in small intervals and removing each entry after age-out.
- Running these commands until all 32 entries age out.

**Reference:**

- `doca_flow/flow_ct_aging/flow_ct_aging_sample.c`
- `doca_flow/flow_ct_aging/flow_ct_aging_main.c`
- `doca_flow/flow_ct_aging/meson.build`

### Flow CT TCP

This sample illustrates how to manage TCP flags with CT to achieve better control over TCP sessions.

**Info:** The sample expects to receive at least SYN and FIN packets.

#### Sample Logic

- Initializing DOCA Flow by indicating `mode_args="switch,hws"` in the `doca_flow_cfg` struct.
- Initializing DOCA Flow CT.
- Starting two DOCA Flow uplink representor ports where port 0 has a special role of being a switch manager port.

**Info:** Ports are configured according to the parameters provided to `doca_dpdk_port_probe()` in the main function.

#### Creating a Pipeline on the Main Port

- Building a TCP pipe to filter non-TCP packets.
- Building a CT pipe to hold TCP session entries.
- Building a CT miss pipe which forwards all packets to RSS pipe.
- Building an RSS pipe from which all packets are directed to the sample main thread for parsing and processing.
- Building a TCP flags filter pipe which identifies the TCP flag inside the packets. SYN, FIN, and RST packets are forwarded to the RSS pipe while all others are forwarded to the EGRESS pipe.
- Building an EGRESS pipe to forward packets to uplink representor port 1.

#### Packet Processing

- The first TCP packet triggers the miss flow as the CT pipe is empty.
- 5-tuple packet parsing is performed.
- TCP flag is examined.
- In case of a SYN flag, a hardware entry is created.
- For FIN or RST flags, the HW entry is removed and all packets are transferred to uplink representor port 1 using `rte_eth_tx_burst()` on port 0 (proxy port) by `rte_flow_dynf_metadata_set()` to 1.
- From this point on, all TCP packets belonging to the above session are offloaded directly to uplink port representor 1.

**Reference:**

- `doca_flow/flow_ct_tcp/flow_ct_tcp_sample.c`
- `doca_flow/flow_ct_tcp/flow_ct_tcp_main.c`
- `doca_flow/flow_ct_tcp/meson.build`

### Flow CT TCP Actions

This sample illustrates how to add shared and non-shared actions to CT TCP sessions. The pipeline is identical to that of the Flow CT TCP sample.

**Info:** The sample expects to receive at least SYN and FIN packets.

This sample adds a shared action on one side of the session that placed the value 1 in the packet's metadata, while on the other side of the session a non-shared action is placed. The non-shared action simply flips the order of the source-destination IP addresses and port numbers.

**Reference:**

- `doca_flow/flow_ct_tcp_actions/flow_ct_tcp_actions_sample.c`
- `doca_flow/flow_ct_tcp_actions/flow_ct_tcp_actions_main.c`
- `doca_flow/flow_ct_tcp_actions/meson.build`

### Flow CT TCP Flow Log

This sample illustrates how to use the flow log callback to alert when a session is aged/removed.

**Info:** The sample expects to receive at least SYN and FIN packets.

This sample is based on the Flow CT TCP sample. Once a session is removed (after receiving FIN packet), the callback is triggered and session counters are queried.

**Reference:**

- `doca_flow/flow_ct_tcp_flow_log/flow_ct_tcp_flow_log_sample.c`
- `doca_flow/flow_ct_tcp_flow_log/flow_ct_tcp_flow_log_main.c`
- `doca_flow/flow_ct_tcp_flow_log/meson.build`

### Flow CT TCP IPv4/IPv6

This sample illustrates how to manage a flow with a different IP type per direction.

In case of a SYN flag:

- A single HW entry of IPv4 is created as origin direction
- An additional HW entry of IPv6 is created as reply direction

From this point on, all IPv4 TCP packets (belonging to the origin direction) and all IPv6 TCP packets (belonging to the reply direction) are offloaded.

**Reference:**

- `doca_flow/flow_ct_tcp/flow_ct_tcp_sample_ipv4_ipv6.c`
- `doca_flow/flow_ct_tcp/flow_ct_tcp_ipv4_ipv6_main.c`
- `doca_flow/flow_ct_tcp/meson.build`
