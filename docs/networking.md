# Networking and Wget Notes

This document records the current host Ethernet backend, guest TCP path, and BusyBox networking setup.

## Host Device Model

The host-side Ethernet device lives under `src/io_devices/ether/`.

- `ether.c` owns the MMIO-facing device state.
- `ether_nat.c` is the default user-mode NAT backend.
- `ether_null.c` and `ether_udp.c` provide alternate backends.
- `ether_backend.h` is the backend interface.

The device is exposed as PCI function `00:01.0` (`1DB7:1000`, Ethernet class)
with a 4 KiB memory BAR. The guest enumerator assigns BAR0 at boot and enables
bus mastering plus MSI. The old fixed `0x00750000` MMIO window remains as a
compatibility alias for older guest images.

RX is pumped when the VM poll hook runs or when the guest reads
`ETHER_OFF_RX_LEN`/`ETHER_OFF_STATUS`. Once a frame has been DMA-written into
the guest buffer, the PCI function raises MSI; if MSI has not been enabled it
uses the legacy INTA route. The guest keeps one delivered frame pending until
the network stack consumes it, providing backpressure to the host-side queue.

Debug logging is off by default. Set `LAMP_NET_TRACE=1` to trace NAT and Ethernet packet movement:

```bash
LAMP_NET_TRACE=1 ./build-release/vm --bin ./bios/boot.bin --serial-stdin
```

Normal host runtime logging is also disabled by default. Set `LAMP_VM_LOG=1`
when startup, device-registration, or VNC lifecycle messages are needed.

## NAT Topology

The NAT backend exposes a small SLIRP-like network:

- guest IP: `10.0.2.15`
- gateway and host loopback alias: `10.0.2.2`
- DNS alias: `10.0.2.3`
- netmask: `255.255.255.0`

The backend supports:

- ARP replies for gateway/DNS IPs
- ICMP echo replies
- UDP forwarding
- TCP connect-forward with a small userspace TCP shim

`10.0.2.2` maps to host `127.0.0.1`, so local test servers can be reached from the guest without privileged host networking:

```bash
python3 -m http.server 8080 --bind 127.0.0.1
```

Guest test:

```text
wget http://10.0.2.2:8080/index.html
cat index.html
nslookup example.com 10.0.2.3
```

## TCP Constraints

The TCP path is designed for simple client workloads such as `ping`, `nc`, and BusyBox `wget`.

Guest IPv4 receive paths share common validation for Ethernet type, IPv4 header length,
destination address, header checksum, and fragmentation. Fragment reassembly is not
implemented, so fragmented IPv4 packets are dropped.

Important constraints:

- NAT and UDP-tunnel backends retain up to 16 pending RX frames. The guest-facing
  MMIO device still presents one frame at a time through
  `ether_state_t.rx_len/rx_lo`, preserving the existing ABI while absorbing
  short host-side bursts.
- The NAT backend avoids emitting pure ACK-only frames when doing so would
  crowd out payload frames.
- The guest TCP receive path must respect the caller's requested read length. BusyBox `wget` often reads one byte at a time; copying more than the requested length corrupts userspace stack data.
- A payload segment may also carry `FIN`; the guest TCP path advances `rcv_nxt` for both the payload and FIN so the next read can return EOF.

The current implementation is not a complete TCP stack. It does not implement retransmission, congestion control, window scaling, or robust multi-packet reassembly.

## Socket Semantics

The guest socket layer currently supports:

- TCP client sockets through `socket`, `connect`, `read`/`write`, `send`/`recv`, and `close`.
- UDP sockets through `socket`, `bind`, `connect`, `read`/`write`, `send`/`recv`, and `sendto`/`recvfrom`.
- Raw ICMP sockets for BusyBox `ping`.

UDP `connect()` records the peer and allocates an ephemeral local port if needed. This
allows libc or BusyBox code that uses ordinary `write()` and `read()` on a connected
datagram socket to work, which is required by `nslookup`.

UDP checksums are emitted as zero, which is valid for IPv4. Incoming UDP checksum
verification is not implemented yet.

## BusyBox Configuration

BusyBox networking applets are controlled by `user/busybox_lamp_min.config`.

Current practical notes:

- `ping`, `nc`, `nslookup`, and `wget` are enabled.
- `CONFIG_FEATURE_WGET_STATUSBAR` is disabled. The progress bar path depends on terminal/time behavior that is not yet robust enough in the guest libc/kernel combination, and it can stall after successful downloads.
- `wget -O - ...` is less reliable than saving to a file today. The recommended smoke test is plain `wget URL` followed by `cat`.

Rebuild/install BusyBox:

```bash
bash user/build_busybox.sh
bash user/install_busybox_to_disk.sh --input busybox-1.37.0/busybox --disk disk.img
```

## Debug Checklist

If `wget` regresses:

1. Verify the host server is reachable from the host.
2. Run with `LAMP_NET_TRACE=1` and check that SYN/SYN-ACK, request payload, response payload, and FIN move through NAT.
3. Use `wget http://10.0.2.2:8080/index.html` first; avoid `-O -` while debugging.
4. If the guest crashes during `read`, inspect socket read length handling first.
5. If the file is not created, inspect `open` flags, ext4 `O_CREAT`, and `unlink`.
