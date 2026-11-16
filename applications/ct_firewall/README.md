# DOCA CT Firewall Application

This document describes the DOCA CT Firewall reference application on an
NVIDIA® BlueField® networking platform or NVIDIA® ConnectX® adapter.

## Introduction

DOCA CT Firewall needs a device with DOCA Flow connection tracking and TCP
window validation; it is verified on NVIDIA ConnectX-8. IPv6 connection
tracking needs BlueField-3 or ConnectX-8 or later (see *Running
Prerequisites*).

DOCA CT Firewall is a reference application that tracks and offloads every TCP
and UDP connection between two uplinks with the DOCA Flow Connection Tracking
(CT) API. A connection is admitted in software; once its hardware entry is
live, its packets are forwarded in hardware and, with window validation, only
state changes, such as FIN and RST, reach software.

It is not a complete firewall: it has no allow/deny rules and no port
translation.

The application combines these capabilities in one data path:

- Connection tracking of every TCP and UDP connection (5-tuple plus zone).
- TCP window validation in hardware, on by default (`--liberal`,
  `--no-window-validation`).
- Destination and source NAT of the IP address (`--dnat`, `--snat`).
- Zones from the VLAN tags: the same addresses and ports on two VLANs, or on
  two inner VLANs of double-tagged (QinQ) traffic, are two connections.
- Deferred offload of TCP connections (`--offload-after`).
- IPv4 and IPv6 (IPv6 needs device support for IPv6 connection tracking; NAT
  of ARP, ping and ICMP errors: IPv4 only).

> **Warning:** With `--dnat` or `--snat`, the hosts on segment A (port 0, see
> *System Design*) must be trusted: one forged frame sent from segment A can
> take the `--snat` address or open a translated UDP connection toward a
> client. Connect only trusted hosts to segment A.

### Supported Behavior and Limitations

- **No allow/deny policy.** Every TCP and UDP connection is tracked and
  offloaded; ARP, ICMP and ICMPv6 are forwarded through software (with NAT,
  see *Address Translation (NAT)*); other protocols are dropped. Filter
  unwanted traffic before the firewall.
- **TCP opens on the handshake.** Illegal openers, such as a SYN-ACK with no
  SYN, and data on a half-open connection are dropped.
- **Connections already in progress are picked up mid-flow**, with relaxed
  window checks as with `--liberal`. Software forwards them until both
  directions are seen (and, with `--offload-after`, that many payload bytes
  have passed); a connection is not picked up once a FIN has been seen.
- **Both directions must cross the firewall.** A TCP connection seen in one
  direction only stays in software and is forgotten 5–10 s after its last
  packet (sooner with an `--aging-timeout` below 5 s). Route both directions
  through the firewall.
- **A packet handled in software that arrives from the wrong segment for its
  connection is dropped** (`wrong_uplink_drops`) and does not keep the
  connection alive; packets handled in hardware are not checked this way. A
  host that forges another host's addresses and sends the first TCP packet of
  that host's connection can therefore block the real TCP connection while it
  keeps sending, and for 5–10 s after its last forged packet (sooner with an
  `--aging-timeout` below 5 s). Unfragmented UDP is not affected once its
  connection is installed. A forged first UDP datagram installs its
  connection at once; the real host's fragmented datagrams of that flow are
  then dropped (`frag_drops`) until the connection idles out
  (`--aging-timeout`), and the flow's other traffic keeps it alive. Filter
  spoofed source addresses before the firewall.
- **Out-of-window TCP segments are dropped in hardware.** Use `--liberal` to
  relax validation or `--no-window-validation` to disable it, for every
  connection.
- **A reused 5-tuple is refused until the old connection's hardware entry is
  removed**: normally one SYN retransmission (about 1 s), longer if the
  removal is refused. Every SYN (either direction) on the ports of a
  connection that still has a hardware entry is dropped and counted
  (`reuse_drops`); the client's retransmission then opens the new connection.
  A connection the firewall still sees as open is not affected, so a client
  that reuses the ports of a connection that ended only on its side (for
  example after a crash) cannot open the new one until the old entry is
  removed: after it idles out (`--aging-timeout`, default 30 s) or, with
  window validation, a reset. With `--no-window-validation` a SYN on such
  ports is forwarded instead, unless the old entry is already being removed.
- **With `--no-window-validation`, a reset does not remove an offloaded
  connection.** Once its hardware entry is live, its FIN and RST are forwarded
  in hardware without reaching software, so the entry is removed only when it
  idles out (`--aging-timeout`); `conns_aged` and `conn_totals` count it then.
  A reset that arrives while the connection is being installed still removes
  it.
- **One idle timeout for all offloaded connections** (`--aging-timeout`,
  default 30 s). A connection that idles out is picked up again with relaxed
  checks (with NAT, see *Address Translation (NAT)*). Set `--aging-timeout`
  above the longest idle period you expect.
- **IPv4 TCP and UDP fragments are reassembled and checked in software** (at
  software rate), then forwarded as received. Fragmented TCP must belong to
  an existing connection; a fragmented SYN or RST is dropped. Admitted
  reassembled datagrams of an offloaded connection are replayed through
  connection tracking to refresh its idle timer, so fragmented UDP keeps its
  hardware entry alive; each replay is one more transmit of the whole
  datagram, which is not sent on the wire. Fragmented TCP gets the
  connection-state checks but no window-validation verdict: the original
  fragments leave before the reassembled segment is replayed through
  connection tracking. IPv6 fragments and fragments of other protocols are
  dropped: keep IPv6 datagrams within the MTU.
- **Both directions of a connection must carry the same 802.1Q VLAN tags.**
  802.1ad (`0x88a8`) and triple-tagged frames do not open connections: filter
  them before the firewall.
- **IPv6 needs a device with IPv6 connection tracking** (see *Running
  Prerequisites*); otherwise IPv6 is dropped. IPv6 extension headers other
  than Fragment are not supported.
- **NAT rewrites IP addresses only, not ports**, of connections opened from
  segment A, and needs `--ct-queues 1`. See *Address Translation (NAT)*.
- **IPv6 NAT translates TCP and UDP only.** Add static neighbor entries on the
  hosts and use one MTU on both segments (see *Address Translation (NAT)*).
- **When capacity is full, new connections are dropped**, not forwarded
  unchecked. Size `--sessions` and `--pool-size` for your load (see
  *Capacity*).
- **If the hardware stops completing connection operations, the application
  exits** with an error. Run it under a supervisor that restarts it (see
  *Troubleshooting*). Without `--sync`, two cases go unnoticed: if the
  hardware never completes the removal of an idle entry that no connection
  holds, or a failed install is not undone, those hardware resources stay in
  use until the application shuts down.
- **While the application is stopped, nothing is forwarded between the two
  segments**, also after a crash. After a restart, connections in progress
  are picked up mid-flow and continue after a retransmission; a connection
  translated by NAT continues only if its client side sends first (otherwise
  the client must reconnect).

## System Design

The application runs on two ports of an NVIDIA® BlueField® networking platform
or NVIDIA® ConnectX® adapter configured in switch mode, forming a two-port
bump-in-the-wire firewall:

```
   segment A  <--->  [ port 0 ]   doca_ct_firewall   [ port 1 ]  <--->  segment B
                     (switch manager PF)            (representor)
```

- **Port 0 (segment A)** is the switch manager PF (`-a`), the first uplink.
  Its traffic is tracked and forwarded to port 1.
- **Port 1 (segment B)** is a representor (`-r`), the second uplink. Its
  traffic is tracked and forwarded to port 0.

Traffic entering either port is classified as follows:

- TCP and UDP enter connection tracking. ARP, ICMP and ICMPv6 go to software
  and are forwarded, translated with IPv4 NAT. With IPv6 NAT, ICMPv6 is
  dropped in hardware; ARP and IPv4 ICMP are still forwarded unchanged.
  Everything else, and IPv6 on a device without IPv6 connection tracking
  (ICMPv6 included), is dropped in hardware.
- IPv4 TCP and UDP fragments go to software, which reassembles each datagram,
  checks it against the connections and forwards the original fragments. At
  most 64 datagrams per worker are in progress, with up to 8 fragments and
  9000 bytes each; an incomplete datagram is dropped 1–2 s after its first
  fragment. Nothing is evicted: one host can send 64 incomplete datagrams and
  fill a worker's table; new fragmented datagrams of every host on that worker
  are then dropped (with a warning) until those time out. Other fragments
  (ICMP included) and IPv6 fragments are dropped.
- A new TCP connection: the SYN and the SYN-ACK go to software and are
  forwarded. The hardware entry is installed at the SYN-ACK (or after
  `--offload-after` payload bytes). Later segments are handled in hardware.
- A new UDP flow: its first datagram goes to software and the entry is
  installed at once.
- An established connection is matched, window-validated, translated and
  forwarded in hardware.

The zone is the VLAN ID of an 802.1Q tag. A frame with two tags (QinQ) is in
the zone of the pair, outer and inner. Untagged and priority-tagged (VLAN ID 0)
frames share zone 0, and an inner VLAN ID 0 leaves a frame in the zone of its
outer VLAN. A reply that arrives untagged, or on other tags, is in another zone
and does not match the connection. A frame with an 802.1ad tag or more than two
tags never opens a connection and is dropped (`parse_drops`), but one that
matches a live connection of its zone is forwarded in hardware. Forwarded
frames keep their tags.

### Address Translation (NAT)

`--dnat <ip>` rewrites the destination, and `--snat <ip>` the source, of the
connections opened from segment A. Only the IP address changes; ports are
kept.

- **Requirements.** NAT needs `--ct-queues 1`. Both flags take an address of
  the same family, and only connections of that family are translated.
- **Untranslated replies.** As soon as a connection's hardware entry is
  added (after its window-validation setup is written), a packet from
  segment B with its untranslated reverse addresses and ports (with `--dnat`,
  from the address the client connected to; with `--snat`, to the client's
  own address) is taken as a reply in software: it is forwarded without
  window validation, and a TCP reset in it ends the connection; until the
  entry is live, other TCP packets of the connection are dropped
  (`install_drops`). Filter such packets before the firewall if segment B is
  not trusted.
- **Direction.** Connections opened from segment B are not translated. A
  connection already in progress is translated only if the firewall first sees
  it from segment A; seen first from segment B, it is tracked untranslated and
  may not pass until the client reconnects. This also applies after it idles
  out.
- **Fragments.** With `--snat`, fragmented datagrams of different clients (in
  different zones, or behind one router) all carry the `--snat` address. If
  two of them send fragmented datagrams with the same IP identification to one
  destination at the same time, the receiver can mix up their fragments. Use
  `--snat` where such traffic cannot coincide, or keep datagrams within the
  MTU.
- **Port collisions.** Two connections that become identical after
  translation collide: with `--snat`, two clients that use one source port
  toward one destination; with `--dnat`, one client that uses one source port
  toward two destination addresses on the same port. Use NAT where such
  connections cannot coincide.
- **ARP, ping and ICMP errors (IPv4).** Software rewrites ARP and ping, so the
  hosts need no static neighbor entries. ICMP errors (for example
  fragmentation needed) of a tracked connection are translated with it, so Path
  MTU Discovery works; an error from a router keeps the router's address. Other
  ICMP, and errors that match no connection, are dropped (`nat_other_drops`).
- **Pings (IPv4).** A ping is tracked in software by its identifier and never
  offloaded: at most 1024 at a time, each forgotten 5–10 s after its last
  packet (sooner with an `--aging-timeout` below 5 s). A ping that reuses an
  identifier already in use toward the same address, or a new ping with a bad
  ICMP checksum, is dropped.
- **`--dnat`.** Every address the client reaches through the firewall,
  including in ARP, leads to the `--dnat` host. Keep other hosts off the
  client's segment.
- **`--snat` (IPv4).** One client host per zone uses the IPv4 `--snat`
  address, for up to 64 zones, including zone 0 for untagged traffic; new
  clients in further zones are refused. The owner is the first host whose ARP
  request, ping or new connection is admitted, until restart. A log line names
  its MAC and IP address; clients behind one router count as one host. The
  server's ARP for the `--snat` address goes to the address of the owner's
  latest ARP request or gratuitous ARP, so a client re-addressed with the same
  MAC (for example by DHCP) needs no restart. Another host's ARP requests, new
  pings and new connections are dropped (`nat_other_drops`, with a warning at
  most once a second); to change the client host, restart the application and
  flush the server's neighbor entry for the `--snat` address. With clients
  behind a router, flush the router's neighbor entry for the server after the
  application starts (`ip neigh flush to <server address>`) so that the router
  asks again. With `--dnat` as well, give the client host one address on this
  segment.
- **IPv6 NAT.** Neighbor discovery and ICMPv6 are not translated; they are
  dropped in hardware. IPv4 ARP and ICMP pass unchanged. Add a static
  neighbor entry for the translated address on each host (`ip -6 neigh
  replace <address> lladdr <MAC> dev <interface> nud permanent`). IPv6 `ping`
  and Path MTU Discovery do not work through IPv6 NAT, so use one MTU on both
  segments.

## Application Architecture

The application separates control decisions (software) from per-packet work
(hardware):

- **Software** — one worker thread per CT queue observes handshakes, installs
  connections, drives aging, and forwards packets not yet offloaded.
- **Hardware** — derives the zone from the VLAN tags, matches the 5-tuple plus
  zone, applies TCP window validation and the NAT action, and forwards to the
  other uplink.

Packets are distributed across workers by IP addresses only, so all
connections between one pair of hosts are handled by one worker.

**Installing a connection.** A worker installs a new connection without
waiting (with `--sync`, it waits for each install). By default the SYN-ACK
is forwarded only once the hardware entry is live; with `--offload-after`,
it is forwarded at once. For a mid-flow pickup or an install after
`--offload-after` bytes, the segment that completes the condition is
forwarded, and a mid-flow one is also replayed through hardware to bring its
state up to date (`reinjections`), as is each reassembled datagram of an
offloaded connection, refreshing its idle timer. Other segments of the
connection that reach software during the install can be dropped
(`install_drops`); senders retransmit them, so an install can cost one
retransmission timeout (about 210 ms). Data carried in the SYN or SYN-ACK
itself (TCP Fast Open) is forwarded and counts once toward `--offload-after`;
a retransmitted SYN or SYN-ACK adds nothing and never triggers the install,
which waits for the next segment without SYN. Separate data segments sent
before the SYN-ACK arrives are dropped and must be retransmitted.

**Re-enabling a connection.** If hardware disables tracking of an offloaded
connection (for example after a SYN on the established connection), the
worker re-enables it (`conns_reenabled`); meanwhile its segments that carry
data or a FIN are dropped (`install_drops`) and retransmitted.

After a mid-flow pickup, an install after `--offload-after` bytes, or a
re-enable, segments that carry data or a FIN and reach software are still
dropped (`install_drops`) for at least 100 µs: one that reached software just
before the entry went live or was re-enabled can arrive that much later, and
forwarding it unvalidated could get the peer's ACK of it dropped in
hardware. The 100 µs is a measured margin, not a guarantee. A FIN that
arrives within it is retransmitted, so a connection that closes right after
crossing `--offload-after` can take one retransmission timeout longer to close.

During an install or re-enable, packets forwarded by software can be overtaken
by packets forwarded in hardware. After a re-enable, TCP segments can be
dropped as out of window (`window_validation[invalid]`) until retransmitted.
Use `--liberal` to avoid those window-validation drops.

## DOCA Libraries

This application leverages the following DOCA libraries:

- **DOCA Flow** (including the Connection Tracking feature) — see
  [DOCA Flow](https://docs.nvidia.com/doca/sdk/doca-flow/index.html).
- **DOCA Arg Parser** — see
  [DOCA Arg Parser](https://docs.nvidia.com/doca/sdk/doca-arg-parser/index.html).
- **DOCA DPDK Bridge** — the DPDK port and queue setup behind the two uplinks.

## Compiling the Application

DOCA reference applications are installed with full source code and build
instructions. This allows the applications to be compiled as-is, or the source
code to be modified to create custom versions. Please refer to the
[DOCA Installation Guide for Linux](https://docs.nvidia.com/doca/sdk/doca-installation-guide-for-linux/index.html)
for details on how to install BlueField-related software.

The source code for the application is located in the following directory:

```
/opt/mellanox/doca/applications/ct_firewall/
```

### Prerequisites

The application depends on the DOCA Flow, DOCA Arg Parser, and DOCA DPDK Bridge
libraries and on DPDK, all installed with the DOCA SDK. No additional
development package is required.

### Compiling All Applications

All DOCA applications are defined under a single meson project. To build them
all:

```
cd /opt/mellanox/doca/applications
meson /tmp/build
ninja -C /tmp/build
```

The compiled binary is `/tmp/build/ct_firewall/doca_ct_firewall`.

### Compiling Only the CT Firewall Application

To build only this application:

```
cd /opt/mellanox/doca/applications
meson /tmp/build -Denable_all_applications=false -Denable_ct_firewall=true
ninja -C /tmp/build
```

## Running the Application

### Running Prerequisites

**Device.** A device with DOCA Flow connection tracking and TCP window
validation; verified on NVIDIA ConnectX-8. IPv6 connection tracking needs
BlueField-3 or ConnectX-8 or later; the `CT Firewall ready` line shows
`ipv6_ct=on` when it is available. Without it, IPv6 is dropped in hardware, a
warning is logged at startup, and an IPv6 `--dnat`/`--snat` is refused.

**Device configuration (`mlxconfig`).** `LAG_RESOURCE_ALLOCATION` must be `1`
on both ports:

```
sudo mlxconfig -d <port-0 PCI> set LAG_RESOURCE_ALLOCATION=1
sudo mlxconfig -d <port-1 PCI> set LAG_RESOURCE_ALLOCATION=1
```

The setting takes effect after a power cycle of the host. Refer to the
[DOCA Flow Connection Tracking](https://docs.nvidia.com/doca/sdk/doca-flow-connection-tracking/index.html)
documentation for the other device settings connection tracking requires.

**Switch mode, one E-Switch for both ports.** Put both ports in switch mode,
then enable the multiport E-Switch on both, in this order:

```
sudo devlink dev eswitch set pci/<port-0 PCI> mode switchdev
sudo devlink dev eswitch set pci/<port-1 PCI> mode switchdev
sudo devlink dev param set pci/<port-0 PCI> name esw_multiport value true cmode runtime
sudo devlink dev param set pci/<port-1 PCI> name esw_multiport value true cmode runtime
```

`-a` then takes the port-0 PCI address and `-r` the port-1 PCI address; the
application opens port 1 as a representor of the switch manager.

While the application runs, it owns both ports: the kernel network interfaces
of these ports lose connectivity, so do not use them for management access.

**Uplinks up.** Both uplink interfaces must be administratively up before the
application starts, with the same MTU on both sides (`<n>`, the MTU the hosts
on both segments use):

```
sudo ip link set <port-0 interface> mtu <n> up
sudo ip link set <port-1 interface> mtu <n> up
```

**Huge pages.** Reserve and mount huge pages for DPDK; the standard DOCA
setting is 2048 pages of 2 MB (1 GiB pages work as well; they must be mounted):

```
echo '2048' | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
```

**Lcores.** Give the EAL `-l` option one core for the main thread plus one per
CT queue (`--ct-queues`): `-l 0-1` for the default single queue. After the
`--` separator, `-l` is the log level instead.

### Quick Start

The walk-throughs use host A (`10.0.0.1/24`) behind port 0 and host B
(`10.0.0.2/24`) behind port 1. Host A and host B must be separate hosts (or
separate network namespaces). On one machine, the kernel delivers the traffic
locally and it never crosses the firewall. Adjust PCI addresses, cores and
addresses to your setup. The commands run the binary from the build directory,
`/tmp/build/ct_firewall/` (see *Compiling the Application*). The traffic
generator is `iperf3`; any TCP client and server will do. Before the first run,
start the server on host B: `iperf3 -s`. ARP and `ping` work across the
firewall, with or without NAT (IPv4).

`iperf3` opens a control and a data connection, so each TCP run shows two
connections (`iperf3 -u` counts its data flow in `conns_udp_installed`).

**(a) Plain forwarding.**

```
cd /tmp/build/ct_firewall
./doca_ct_firewall -l 0-1 -- -a pci/0000:08:00.0 -r pci/0000:08:00.1
```

On host A run `iperf3 -c 10.0.0.2`. Stop the application with Ctrl-C; the
shutdown report shows `conns_installed=2` and the bulk of the packets on
`fast_path_forward`. For traffic in the other direction and in both directions,
run `iperf3 -c 10.0.0.2 -R` and `iperf3 -c 10.0.0.2 --bidir`. To open the
connection from segment B instead, run `iperf3 -s` on host A and
`iperf3 -c 10.0.0.1` on host B. Each run adds two connections.

**(b) Destination NAT.** Connections opened from segment A get destination
`10.0.0.2`:

```
./doca_ct_firewall -l 0-1 -- -a pci/0000:08:00.0 -r pci/0000:08:00.1 --dnat 10.0.0.2
```

No static neighbor entries are needed. On host A run `ping 10.0.0.10`, then
`iperf3 -c 10.0.0.10`. Host B sees the ARP request, the ping and the connection
for `10.0.0.2` and answers with its own MAC address; host A sees the replies
come from `10.0.0.10`. A connection opened from host B (segment B) is not
translated.

**(c) VLANs.** No option is needed: each VLAN is its own zone. With the plain
run from (a), on both hosts add a VLAN 100 sub-interface (`ip link add link
<interface> name <interface>.100 type vlan id 100`), address it
(`10.0.100.1/24` and `10.0.100.2/24`). On host A run `iperf3 -c 10.0.100.2`.
Host B receives the frames still tagged, and the connection is tracked
separately from the same addresses untagged or on another VLAN.

**(d) Dropped traffic.** Illegal openers (for example a SYN-ACK or a FIN with
no connection) are dropped and counted in `state_drops`; out-of-window
segments are dropped in hardware and counted in `window_validation[invalid]`.
Both counters are in the live report and the shutdown report (see
*Statistics*).

**(e) Fragmented traffic.** With the plain run from (a),
`iperf3 -u -l 3000 -c 10.0.0.2` sends 3000-byte UDP payloads, which exceed a
1500-byte MTU, so the IP layer fragments them; they arrive complete and
`frag_reassembled` counts them, and once the flow is offloaded `reinjections`
and `reinject_to_ct` rise with it. `ping -s 3000 10.0.0.2` is still dropped:
only TCP and UDP fragments are forwarded. For TCP, place the client behind a
router whose link to the firewall has a 1500-byte MTU while the client's own
link has 9000, clear DF on the client (`sysctl net.ipv4.ip_no_pmtu_disc=1`),
and on host B advertise a large MSS toward the client (`advmss 8960` on its
route to the client's network). The router fragments every data segment;
`iperf3` runs at software rate (expect retransmissions), `frag_reassembled`,
`reinjections` and `reinject_to_ct` rise together, and
`window_validation[invalid]` stays 0.

**Try it with real traffic.** Start the plain run from (a), then watch the
live report that the application logs every 5 s. Its lines are
`connections: installed=.. adopted=.. udp=..`,
`window-validation: ... invalid=..` and `fast_path[port N]=..`. The
`iperf3 -s` server from above must still be running on host B. On host A,
send TCP, multi-stream TCP and UDP traffic:

```
ping -c 3 10.0.0.2; iperf3 -c 10.0.0.2 -t 10; iperf3 -c 10.0.0.2 -t 10 -P 8; iperf3 -c 10.0.0.2 -u -b 1G -t 5
```

Each run raises `installed` by its TCP connections (iperf3 adds one control
connection), and the UDP run also raises `udp` by 1; `invalid` stays 0. After
a multi-stream run, `adopted` may rise by 1: late packets of a stream iperf3
reset are picked up and then removed again. That is expected.

To see a mid-flow pickup after a restart, run `iperf3 -c 10.0.0.2 -t 60` on
host A. While it runs, stop the application with Ctrl-C and start it again.
You can add `--sync` to try synchronous install as well; pickup works in
either mode. iperf3 shows 0 bits/s while the application is down, then
resumes. The new run shows `adopted` of 1 or more, and `fast_path` rises
again. When you stop the application with Ctrl-C, the shutdown report
repeats the counters under the names in *Statistics*.

### Application Execution

The application is provided in source form; compile it first (see *Compiling
the Application*). It is invoked as
`doca_ct_firewall <EAL flags> -- <program flags>`.
EAL (DPDK) flags precede the `--` separator; the application flags follow it.

Usage:

```
Usage: doca_ct_firewall [DPDK Flags] -- [DOCA Flags] [Program Flags]

DOCA Flags:
  -h, --help                        Print a help synopsis
  -v, --version                     Print program version information
  -l, --log-level                   Set the (numeric) log level for the program <10=DISABLE, 20=CRITICAL, 30=ERROR, 40=WARNING, 50=INFO, 60=DEBUG, 70=TRACE>
  --sdk-log-level                   Set the SDK (numeric) log level for the program <10=DISABLE, 20=CRITICAL, 30=ERROR, 40=WARNING, 50=INFO, 60=DEBUG, 70=TRACE>
  --log-filter                      Filter logs from specific modules, separated by comma
  -j, --json <path>                 Parse command line flags from an input json file

Program Flags:
  -a, --device                      device
  -r, --rep                         device representor
  --sync                            Install connections synchronously (the worker waits for each hardware install and removal); default asynchronous
  --no-window-validation            Disable TCP window validation; connections are tracked and forwarded without sequence checks
  --liberal                         Disable strict window validation on every connection (out-of-window segments are not dropped)
  --max-ack-window <n>              Acknowledgement-window bound exponent, 0 selects the default 64 KiB; 0..7; default 0
  --offload-after <bytes>           Keep a TCP connection in software until this many payload bytes passed, 0 installs at the SYN-ACK; 0..2147483647; default 0
  --dnat <ip>                       Rewrite the destination of connections opened from the first uplink to <ip> (same IP version only; needs --ct-queues 1); IPv4 or IPv6 address; default none
  --snat <ip>                       Rewrite the source of connections opened from the first uplink to <ip> (same IP version only; needs --ct-queues 1); IPv4 or IPv6 address; default none
  --ct-queues <n>                   Connection-tracking queues, one worker thread each; 1..16; default 1
  --sessions <n>                    Connection-tracking sessions per address family, aggregate across queues; 1..3800000; default 8192
  --pool-size <n>                   Maximum TCP connections offloaded with window validation at once; 1..8388608; default 8192
  --aging-timeout <n>               Seconds a connection may stay idle before it is released; 1..65535; default 30
  --stats-interval <n>              Seconds between periodic statistics reports, 0 disables; 0..3600; default 5
```

Example, from the build directory `/tmp/build/ct_firewall/` (adjust the PCI
addresses and core list to your setup):

```
cd /tmp/build/ct_firewall
./doca_ct_firewall -l 0-1 --file-prefix ctfw -- \
    -a pci/0000:08:00.0 -r pci/0000:08:00.1
```

`-a` and `-r` take the PCI addresses of port 0 and port 1 (see *Running
Prerequisites*); for the identifier syntax, see the
[DOCA Arg Parser](https://docs.nvidia.com/doca/sdk/doca-arg-parser/index.html#device_syntax)
documentation. `--file-prefix` gives the run its own DPDK shared-memory prefix,
for hosts that run more than one DPDK application. Once the ports are up and
the workers are running, the application logs one line with the effective
settings:

```
CT Firewall ready: mode=async window_validation=on liberal=off max_ack_window=0 offload_after=0 dnat=none snat=none workers=1 aging_timeout=30s ipv6_ct=off
```

#### Statistics

Every `--stats-interval` seconds (5 by default, 0 disables) a shorter live
report of the main counters is logged under its own field names (`rx=`,
`pool-exhausted=`, `threshold=`, `sw-expired=`, `fast_path[port 0]=`,
`reinject:`, `window-validation:`, ...), each matching the counter of the same
meaning below; `connection-totals:` covers removed connections only. On
termination (Ctrl-C) the application prints a final summary. `ipv6_dropped`
appears only when the device lacks IPv6 connection tracking, and with
`--ct-queues` above 1 a per-worker
`rx_packets`/`tx_packets` section precedes the closing rule:

```
==================== CT Firewall Stats ====================
rx_packets=... tx_packets=...
conns_installed=... conns_adopted=... conns_threshold_installed=... conns_udp_installed=... conns_reenabled=... conns_aged=... reinjections=...
pool_exhausted=...
frag_drops=... frag_reassembled=... state_drops=...
conns_sw_expired=...
aged_rm_failed=...
parse_drops=...
wrong_uplink_drops=...
reuse_drops=...
other_forwarded=... nat_other_drops=...
install_failures=...
install_drops=...
install_retries=... wv_limit_drops=... wv_limit_defers=... wv_ops_in_use=...
sweep_lagged=...
---------------------- TCP window validation ----------------------
window_validation[valid]: pkts=... bytes=...
window_validation[state_change]: pkts=... bytes=...
window_validation[invalid]: pkts=... bytes=...
window_validation[disabled]: pkts=... bytes=...
---------------------- Forwarding ----------------------
reinject_to_ct: pkts=... bytes=...
reinject_missed: pkts=... bytes=...
ct_misses: pkts=... bytes=...
ipv6_dropped: pkts=... bytes=...
fast_path_forward[port 0]: pkts=... bytes=...
fast_path_forward[port 1]: pkts=... bytes=...
slow_path_forward[port 0]: pkts=... bytes=...
slow_path_forward[port 1]: pkts=... bytes=...
unknown_port_drops: fast_path=... to_software=... transmit=...
---------------------- Connections (hardware) ----------------------
conn_totals[origin]: pkts=... bytes=...
conn_totals[reply]: pkts=... bytes=...
conn_totals_open_at_exit[origin]: pkts=... bytes=...
conn_totals_open_at_exit[reply]: pkts=... bytes=...
=============================================================
```

| Counter | Meaning |
| --- | --- |
| `rx_packets` / `tx_packets` | Packets received / transmitted by software. |
| `conns_installed` / `conns_adopted` / `conns_threshold_installed` / `conns_udp_installed` | TCP at the handshake / TCP picked up mid-flow / TCP after `--offload-after` bytes / UDP flows. |
| `conns_reenabled` | Offloaded connections whose disabled tracking was re-enabled. |
| `conns_aged` | Hardware entries removed, on idle timeout, RST (with `--no-window-validation`, only one that arrives before the entry is live), or when a new connection reuses the tuple. |
| `conn_totals[origin]` / `conn_totals[reply]` | Packets and bytes counted on the hardware entries of removed connections (idle timeout, RST as for `conns_aged`, tuple reuse), per direction; origin is the side that opened the connection. |
| `conn_totals_open_at_exit[...]` | The same, for connections whose entry was still in place at termination. |
| `conns_sw_expired` | Connections without a hardware entry released after the idle timeout (unanswered connection attempts and connections in progress seen in one direction only after 5–10 s, sooner with an `--aging-timeout` below 5 s). |
| `aged_rm_failed` | Idle entries left in hardware until shutdown because too many removals were already waiting to be retried. Deferred removals are retried. |
| `pool_exhausted` | Packets dropped because capacity was full (including the 1024 pings tracked under NAT). |
| `frag_drops` | Fragments dropped: incomplete or timed out, overlapping, over the limits, not TCP or UDP, IPv6, or refused with their connection. |
| `state_drops` | Illegal TCP openers (for example a SYN-ACK with no SYN, or a reset that does not match the SYN) and data on half-open connections. |
| `parse_drops` | Frames software could not parse, including 802.1ad or triple-tagged frames that match no connection. |
| `reuse_drops` | SYNs (either direction) on the ports of a connection whose hardware entry still exists, dropped until that entry is removed; the peer's retransmission then opens the new connection. Also stray SYNs on a live connection. |
| `wrong_uplink_drops` | Packets handled in software that belong to a tracked connection but arrived from the wrong segment for it; they are dropped. |
| `frag_reassembled` | Fragmented IPv4 datagrams forwarded. |
| `other_forwarded` | ARP, ICMP and ICMPv6 forwarded by software (translated with IPv4 NAT). |
| `nat_other_drops` | With IPv4 `--dnat`/`--snat`: ARP and ICMP packets that cannot be translated (see *Address Translation (NAT)*), and ARP requests, new pings and new connections from a second `--snat` client; also an ARP reply that could not be copied for lack of packet buffers (with a warning). |
| `install_failures` | Operations that hardware completed with a failure: installs (a failed removal is counted and stops the application; with `--sync`, a failed window-validation setup is logged, not counted). Packets of the affected connection can be dropped (`install_drops`) until it is installed again, normally on the client's retransmitted SYN or, mid-flow, a later segment (without `--sync`, a retransmitted SYN-ACK alone does not reopen it). The application keeps running unless it stops with an error (see *Troubleshooting*). |
| `install_drops` | Packets dropped while their connection was being installed or re-enabled, or whose install failed for a reason other than capacity. |
| `reinjections` / `reinject_to_ct` / `reinject_missed` | Replayed segments of connections picked up mid-flow and reassembled datagrams of offloaded connections: sent / reached tracking / dropped because the entry was gone. |
| `window_validation[...]` | Packets per validation verdict; `invalid` are out-of-window drops. With `--no-window-validation` nothing is validated: the section is marked `(off)` and `valid` counts the TCP packets forwarded in hardware. |
| `ct_misses` | Packets that missed the connection table (openers, first UDP datagrams, packets before the entry is live). |
| `ipv6_dropped` | IPv6 packets dropped for lack of IPv6 connection tracking. |
| `fast_path_forward` / `slow_path_forward` `[port N]` | Packets forwarded to port N by hardware / by software. |
| `unknown_port_drops` | Packets dropped because they came from neither uplink (`fast_path`, `to_software`) or left software with no destination (`transmit`); expected 0. |
| `install_retries` | TCP installs that could not be submitted at once because the CT queue was full, or that were queued behind such an install; the worker loop submits them later (window validation, without `--sync`). A handshake's SYN-ACK stays held until the install completes; a mid-flow install's segment was already forwarded. |
| `wv_limit_drops` / `wv_limit_defers` | Segments whose TCP install was refused because 448 window-validation operations were already in progress on the worker (without `--sync`): a handshake's SYN-ACK is dropped and its retransmission retries about 1 s later / a mid-flow install keeps forwarding in software and retries on the next segment. |
| `wv_ops_in_use` | Window-validation operations still in progress at termination, summed over the workers (shutdown summary only); expected 0 once traffic and pending installs have drained. |
| `sweep_lagged` | Once-a-second sweep rounds that started before the previous round had finished; connections held in software are then released later, never earlier. |

The `conn_totals` values are read when an entry is removed, from values
refreshed about once a second, so a connection reset while sending can miss
up to its last second. Packets handled in software (the handshake, traffic
before a mid-flow pickup) are not included; a fragmented datagram of an
offloaded connection counts once, through its replay.

A fragment is counted in `frag_drops`, once per fragment, also when its
datagram is refused for a connection reason such as the wrong segment or the
connection state; that reason's counter does not count it, except that a
datagram whose connection install the hardware refuses for lack of room is
also counted once in `pool_exhausted`.

### Command Line Flags

#### General Flags

| Flag | Description |
| --- | --- |
| `-h`, `--help` | Print a help synopsis. |
| `-v`, `--version` | Print program version information. |
| `-l`, `--log-level` | Set the program log level. |
| `--sdk-log-level` | Set the SDK log level. |
| `--log-filter` | Filter logs from specific modules, separated by comma. |
| `-j`, `--json <path>` | Parse command line flags from an input JSON file. |

Refer to the [DOCA Arg Parser](https://docs.nvidia.com/doca/sdk/doca-arg-parser/index.html)
documentation for more information on the general flags and execution modes.

#### Program Flags

| Flag | Argument | Default | Description |
| --- | --- | --- | --- |
| **Devices** | | | |
| `-a`, `--device` | PCI address | required | Switch manager device (PF) that owns the switch. |
| `-r`, `--rep` | PCI address | required | Representor forming the second uplink. |
| **Connection behavior** | | | |
| `--sync` | — | off (asynchronous) | Install connections synchronously (the worker waits for each hardware install and removal); the default is asynchronous. |
| `--no-window-validation` | — | off | Disable TCP window validation; connections are tracked and forwarded without sequence checks. A reset on an offloaded connection then does not remove it: it idles out (see *Supported Behavior and Limitations*). |
| `--liberal` | — | off | Disable strict window validation on every connection (out-of-window segments are not dropped). |
| `--max-ack-window <n>` | integer (0–7) | 0 | Exponent of the largest acknowledgement window that window validation accepts; 0 is the default, 64 KiB. Raise it for high bandwidth-delay links whose acknowledgements are dropped as out of window (`window_validation[invalid]`). |
| `--offload-after <bytes>` | integer (0–2147483647) | 0 | Keep a TCP connection in software until this many payload bytes passed; 0 installs at the SYN-ACK. |
| **NAT** | | | |
| `--dnat <ip>` | IPv4 or IPv6 address | none | Rewrite the destination of connections opened from segment A (port 0) to `<ip>`; same IP version for both flags; requires `--ct-queues 1`. See *Address Translation (NAT)*. |
| `--snat <ip>` | IPv4 or IPv6 address | none | Rewrite the source of connections opened from segment A (port 0) to `<ip>`; same IP version for both flags; requires `--ct-queues 1`. See *Address Translation (NAT)*. |
| **Scale and lifetime** | | | |
| `--ct-queues <n>` | integer (1–16) | 1 | Connection-tracking queues; one worker thread each. |
| `--sessions <n>` | integer (1–3800000) | 8192 | Connection-tracking sessions per address family, aggregate across queues. The device's flow-counter capacity also limits this value: if the ports fail to start, lower `--sessions`. The software connection tables are sized from this value — one share per address family when the device supports IPv6 connection tracking — so memory use, including huge pages, grows with it. |
| `--pool-size <n>` | integer (1–8388608) | 8192 | Maximum number of TCP connections offloaded with window validation at once. Memory and startup time grow with it (about 5 GB and 40 s at the maximum). |
| `--aging-timeout <n>` | integer (1–65535) | 30 | Seconds a connection may stay idle before it is released. |
| **Observability** | | | |
| `--stats-interval <n>` | integer (0–3600) | 5 | Seconds between periodic statistics reports; 0 disables. |

#### Capacity

When capacity is full, the packet is dropped and counted in `pool_exhausted`;
nothing is forwarded unchecked. Capacity returns as connections age out. The
limits are:

- `--sessions`: connection-tracking sessions. With `--ct-queues` above 1,
  each worker gets an equal share, and all connections between one pair of
  hosts use one worker, so one busy host pair can fill its worker's share
  while the total is well below `--sessions`.
- `--pool-size`: TCP connections offloaded with window validation at once.
- 4096 connections per worker waiting to be picked up mid-flow or for
  `--offload-after`. A segment with ACK set for an unknown connection holds
  one of them for 5–10 s (sooner with an `--aging-timeout` below 5 s), so a
  flood of such segments can delay mid-flow pickups.
- 1024 pings tracked at once under IPv4 NAT.

With window validation and without `--sync`, each worker also keeps at most
512 window-validation operations (installs and re-enables) in progress. Once
448 are in progress, new TCP installs are refused and the rest are kept for
re-enables: a handshake's SYN-ACK is dropped (`wv_limit_drops`) and its
retransmission retries about 1 s later; a mid-flow install keeps forwarding
in software and retries on the next segment (`wv_limit_defers`).

**Host memory.** The connection tables hold about one entry per session for
each address family (about twice `--sessions` on a device with IPv6 connection
tracking), divided among the workers. At about 208 bytes per entry, that is an
estimated 1.58 GB at `--sessions 3800000` with IPv6, plus lookup indexes in
huge pages; NAT adds a second index. The tables are reserved at start-up and
become resident as they fill: started with that setting, the application had
0.5 GB resident and 0.8 GB of huge pages in use before any connection.

The times given for releasing connections held in software (5–10 s) hold
while the worker keeps up with its once-a-second software sweep; a worker
that falls behind (`sweep_lagged` rises) releases them later, never earlier.

Set `--sessions` and `--pool-size` above the number of concurrent connections
you expect, counting connections that stay idle up to `--aging-timeout`.

### Troubleshooting

Refer to the [NVIDIA BlueField Platform Software Troubleshooting Guide](https://docs.nvidia.com/networking/display/bfswtroubleshooting)
for any issue encountered with the compilation, installation, or execution of
the DOCA applications.

Symptoms specific to this application:

- **Ping or ARP fails with NAT** — with an IPv4 translation, check
  `nat_other_drops` (for example a second `--snat` client); `--log-level 60`
  logs the reason for each drop. With an IPv6 translation, neighbor
  discovery and ICMPv6 are dropped in hardware and no counter shows it: add
  static neighbor entries (see *Address Translation (NAT)*).
- **Packets of a new connection are dropped (`install_drops` rises)** — they
  reached software while the connection was being installed; the sender
  retransmits them. If its install fails, the packet is dropped, and a TCP
  retransmission or the next UDP datagram retries; `install_failures` counts
  such failures (with `--sync`, a failed window-validation setup is only
  logged).
- **New connections are dropped (`pool_exhausted` rises)** — capacity is full;
  raise `--sessions` or `--pool-size` (see *Capacity*).
- **New connections open about 1 s late (`wv_limit_drops` rises)** — the
  worker already had 448 window-validation operations (installs and
  re-enables) in progress; each refused connection opens on its SYN-ACK
  retransmission. Spread the load over more `--ct-queues` (not available with
  NAT); connections between one pair of hosts stay on one worker.
- **The application stops with an error naming a CT queue** — the hardware
  stopped completing connection operations: an operation reported a timeout,
  the software sweep found an install, update or connection removal still
  incomplete more than 10 seconds after it was submitted (with `--sync`, any
  removal not done within 10 seconds), a connection entry could not be
  removed, or a failed install could not be cleaned up. The application
  exits with a non-zero status. Restart it; run it under a supervisor that
  restarts it.
- **A connection opens on one side but replies are dropped (`state_drops`
  rises)** — the zone is the VLAN ID (both IDs of a double tag), so both
  directions must carry the same VLAN tags. A reply that arrives untagged, or
  on a different VLAN, is in another zone and does not match the connection.
  Configure the switches or host interfaces so the traffic is tagged with the
  same VLANs (or untagged) on both sides of the firewall.
- **The application refuses to start** — a setting was rejected, and the
  error log names it. The startup errors and their fixes are listed below.

| Startup error | Fix |
|---|---|
| `No representor given: the second uplink (-r/--rep <PCI>) is required` | Pass the second uplink with `-r`. |
| `--<flag> must be in <min>..<max> (got <value>)` | Use a value within the range shown. |
| `--<flag>: '<ip>' is not an IPv4 or IPv6 address` | Pass a numeric IPv4 or IPv6 address. |
| `--<flag>: '<ip>' is the unspecified address; a translation target must be a real address` | Use a real address, not `0.0.0.0` or `::`. |
| `Address translation (--dnat/--snat) requires a single CT queue (--ct-queues 1)` | Run NAT with `--ct-queues 1`. |
| `--<flag>: '<ip>' is IPv4 but the other translation address is IPv6; --dnat and --snat must name the same address family` | Give `--dnat` and `--snat` addresses of the same family. |
| `--liberal and --max-ack-window require TCP window validation; remove --no-window-validation` | Drop `--no-window-validation`, or drop the validation options. |
| `Each connection uses two hardware flow counters per address family; if the device cannot provide them, lower --sessions` (after `Failed to init DOCA ports`) | Lower `--sessions`. |
| `IPv6 --dnat/--snat needs IPv6 connection tracking, which this device does not support` | Use IPv4 translation, or a device with IPv6 connection tracking. |
| `Only <n> lcores available for <m> workers` | Each CT queue needs one worker lcore plus one for the main thread: add cores to `-l` or lower `--ct-queues`. |

## Application Code Flow

1. Parse the command-line flags: `doca_argp_init()`,
   `doca_argp_register_param()` per flag, `doca_argp_start()`. Every flag is
   range-checked before any device is opened.
2. Open the switch manager device and its representor, initialize DOCA Flow in
   switch mode with the requested number of CT queues, and start both ports
   with connection tracking enabled.
3. Build the hardware pipeline on the switch manager port; one pipeline serves
   both uplinks.
    - `doca_flow_pipe_cfg_create()`, `doca_flow_pipe_cfg_set_match()`,
      `doca_flow_pipe_cfg_set_actions()`, `doca_flow_pipe_create()` per pipe.
    - `doca_flow_pipe_cfg_set_ct_connections()` sizes the connection-tracking
      pipe from `--sessions`; `doca_flow_pipe_basic_add_entry()` adds the zone
      entry that copies the outer VLAN ID into the zone and one entry per
      inner VLAN ID that sets it into the zone;
      `doca_flow_pipe_control_add_entry()` adds the fixed classification
      entries.
    - Start one worker thread per CT queue.
4. Worker loop, per packet delivered to software.
    - Forward ARP, ICMP and ICMPv6 to the other port; with IPv4 NAT, first
      rewrite the addresses of ARP, ping and ICMP errors of tracked
      connections, and drop what cannot be translated.
    - Reassemble IPv4 TCP and UDP fragments and forward them once the
      connection admits them; drop other fragments and unparsable frames; look
      up the connection table; drop illegal TCP openers.
    - Admit a new connection: `doca_flow_ct_entry_prepare()`,
      `doca_flow_ct_tcp_window_validation_update()` (when validation is on),
      `doca_flow_ct_add_entry()`; completions arrive through
      `doca_flow_ct_entries_process()`. Without `--sync`, at most 448
      window-validation installs are in progress per worker, and an add that
      finds the CT queue full is retried from the worker loop.
    - Forward the packet on the other port.
    - Re-enable a disabled connection:
      `doca_flow_ct_tcp_window_validation_query()`, then
      `doca_flow_ct_tcp_window_validation_update()`; until the update is
      complete, at least 100 µs have passed and the segments queued before it
      have been read, drop the connection's segments that carry data or a FIN.
    - Remove the entry of a connection torn down by an RST that reaches
      software (with `--no-window-validation`, only before the entry is live):
      `doca_flow_ct_query_entry()` reads the entry's per-direction counters and
      `doca_flow_ct_rm_entry()` removes it.
    - Once an entry is live, forward the connection's held SYN-ACK, or replay
      the segment that completed a mid-flow pickup through hardware; after an
      install at `--offload-after` bytes or a mid-flow pickup, drop the
      connection's segments that carry data or a FIN until at least 100 µs
      after the entry went live and those queued before then have been read.
5. Worker loop, once per second: `doca_flow_aging_handle()` reports idle
   entries, `doca_flow_ct_query_entry()` reads the entry's per-direction
   counters and `doca_flow_ct_rm_entry()` removes it; a software sweep round
   starts that releases idle connections that have no hardware entry and
   checks for overdue operations. The round is worked through at most 1024
   table entries per loop iteration; unfinished work carries over, up to two
   passes over the table.
6. On termination (Ctrl-C): `doca_flow_resource_query_entry()` reads the
   hardware counters for the final report, and `doca_flow_ct_query_entry()`
   the counters of connections still offloaded; `doca_flow_pipe_destroy()`, port
   stop, and `doca_flow_destroy()` release the resources.

## References

- [DOCA Flow](https://docs.nvidia.com/doca/sdk/doca-flow/index.html)
- [DOCA Flow Connection Tracking](https://docs.nvidia.com/doca/sdk/doca-flow-connection-tracking/index.html)
- [DOCA Installation Guide for Linux](https://docs.nvidia.com/doca/sdk/doca-installation-guide-for-linux/index.html)
- [DOCA Arg Parser](https://docs.nvidia.com/doca/sdk/doca-arg-parser/index.html)
- [DOCA Reference Applications](https://docs.nvidia.com/doca/sdk/doca-reference-applications/index.html)
- Application source: `/opt/mellanox/doca/applications/ct_firewall/`
