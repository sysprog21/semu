# Networking in semu

This document explains how to configure and use networking in semu across different platforms.

## Overview

semu provides platform-specific network backends optimized for each operating system:

- Linux: TAP (kernel-level) and user-mode (SLIRP) networking
- macOS: vmnet.framework (kernel-level NAT; bridge mode planned) and user-mode (SLIRP) networking

All backends use the same VirtIO-Net device interface, ensuring consistent behavior across platforms.

## Platform Comparison

| Feature | Linux TAP | Linux user | macOS vmnet | macOS user |
|---------|-----------|------------|-------------|------------|
| Implementation | TUN/TAP kernel module | minislirp (userspace) | vmnet.framework (kernel) | minislirp (userspace) |
| Privileges | `sudo` or `CAP_NET_ADMIN` | None | `sudo` or entitlement | None |
| Performance | Excellent | Good | Excellent | Good |
| NAT | Manual setup | Built-in | Built-in (shared mode) | Built-in |
| DHCP | Manual setup | Built-in | Built-in (shared mode) | Built-in |
| Bridge mode | Supported | No | Planned¹ | No |
| VM-to-VM | Yes (manual setup) | No | Yes (host mode) | No |

¹ Bridge mode via vmnet host/bridged configurations is not yet exposed in the CLI; only shared/NAT mode is currently available.

> **Note:** On macOS, semu automatically falls back to the user (SLIRP) backend when vmnet cannot be initialized (for example, when it is launched without `sudo` or the vmnet entitlement).
> The guest Linux image included in this repository is built without `CONFIG_PACKET`,
> so DHCP clients such as `udhcpc` cannot run unless you rebuild the kernel with that option enabled.
> Use static addressing inside the guest if you keep the default image.

## Requirements

### Linux
- TAP mode: `sudo` privileges or `CAP_NET_ADMIN` capability
- user mode: No special privileges required

### macOS
- macOS 11.0 or later (Big Sur+)
- vmnet mode: Root privileges or `com.apple.vm.networking` entitlement
- user mode: No special privileges required

## Usage

### Linux

TAP mode (requires sudo):
```shell
# Run semu with TAP backend
sudo ./semu -k Image -b minimal.dtb -i rootfs.cpio -n tap

# Host-side setup (in separate terminal)
sudo ip addr add 192.168.10.1/24 dev tap0
sudo ip link set tap0 up
```

User mode (no sudo required):
```shell
# Run semu with user-mode networking (SLIRP)
./semu -k Image -b minimal.dtb -i rootfs.cpio -n user
```

Inside the VM:
```shell
# For user mode
ip addr add 10.0.2.15/24 dev eth0
ip link set eth0 up
ip route add default via 10.0.2.2
ping 10.0.2.2  # Ping SLIRP gateway
```

### macOS

vmnet mode (requires sudo, shared/NAT mode today; falls back to `user` if vmnet cannot be started):
```shell
# Run semu with vmnet.framework
sudo ./semu -k Image -b minimal.dtb -i rootfs.cpio -n vmnet
# Or simply:
sudo ./semu -k Image -b minimal.dtb -i rootfs.cpio  # vmnet is default on macOS
```

Inside the VM (vmnet):
```shell
# Configure the interface (static example; replace the prefix/gateway if your host uses a different vmnet subnet)
ip link set eth0 up
# Typical shared-mode subnet is 192.168.64.0/24
ip addr add 192.168.64.10/24 dev eth0
ip route add default via 192.168.64.1
ping 192.168.64.1           # Ping vmnet gateway
ping 8.8.8.8                # Test external connectivity
```

If you build the guest kernel with `CONFIG_PACKET=y`, you can use a DHCP client instead of the static configuration shown above.

User mode (no sudo required, for development):
```shell
# Run semu with user-mode networking (SLIRP)
./semu -k Image -b minimal.dtb -i rootfs.cpio -n user
```

Inside the VM (user mode):
```shell
# Configure network (same as Linux user mode)
ip addr add 10.0.2.15/24 dev eth0
ip link set eth0 up
ip route add default via 10.0.2.2
ping 10.0.2.2               # Ping SLIRP gateway
ping 8.8.8.8                # Test external connectivity
```

### Quick Start

Linux (easiest):
```shell
make check NETDEV=user
```

macOS (easiest, no sudo):
```shell
make check NETDEV=user
```

macOS (best performance, requires sudo):
```shell
sudo make check NETDEV=vmnet
# Or simply:
sudo make check
```

## Advanced Configuration

### Linux: Persistent TAP Device

To avoid recreating TAP device on each run:

```shell
# Create persistent TAP device
sudo ip tuntap add dev tap0 mode tap user $USER
sudo ip addr add 192.168.10.1/24 dev tap0
sudo ip link set tap0 up

# Now run semu without sudo (if using persistent TAP)
./semu -k Image -b minimal.dtb -i rootfs.cpio -n tap
```

### Linux: Capability-based Access

Instead of `sudo`, grant specific capabilities:

```shell
# Grant CAP_NET_ADMIN to semu binary
sudo setcap cap_net_admin=eip ./semu

# Now run without sudo
./semu -k Image -b minimal.dtb -i rootfs.cpio -n tap
```

### macOS: Entitlement (Advanced)

For production use or to avoid requiring `sudo`, you can request the `com.apple.vm.networking` entitlement from Apple. This requires:

1. Being a registered Apple Developer
2. Contacting Apple to request the entitlement
3. Signing your application with the appropriate provisioning profile

### Creating an Entitlements File

Create `semu.entitlements`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.vm.networking</key>
    <true/>
</dict>
</plist>
```

### Code Signing

```shell
# Sign with entitlements (requires Apple Developer certificate)
codesign --entitlements semu.entitlements -s "Your Developer ID" semu
```

Note: The `com.apple.vm.networking` entitlement is restricted and requires approval from Apple.

## Troubleshooting

### Linux Issues

"Failed to open TAP device"
```
Solution 1: Run with sudo
sudo ./semu -k Image -b minimal.dtb -i rootfs.cpio -n tap

Solution 2: Grant CAP_NET_ADMIN capability
sudo setcap cap_net_admin=eip ./semu

Solution 3: Create persistent TAP device owned by your user
sudo ip tuntap add dev tap0 mode tap user $USER
```

"No route to host" in VM
```shell
# Check routing inside VM
ip route

# Add default route if missing
ip route add default via 192.168.10.1  # For TAP
ip route add default via 10.0.2.2      # For user mode
```

Slow network performance (user mode)
- User-mode networking (SLIRP) is slower than TAP due to userspace processing
- For better performance, use TAP mode with `sudo`

### macOS Issues

"[vmnet] failed to create interface: 1"
```
This means insufficient privileges. Solutions:

1. Run with sudo (recommended)
   sudo ./semu -k Image -b minimal.dtb -i rootfs.cpio -n vmnet

2. Use user mode instead (no sudo required)
   ./semu -k Image -b minimal.dtb -i rootfs.cpio -n user

3. Apply entitlement (requires Apple Developer account)
   codesign --entitlements semu.entitlements -s "Developer ID" semu
```

Which backend should I use on macOS?
- For development/testing: user mode (no sudo, easier)
- For performance: vmnet mode (requires sudo, faster)
- For CI/automation: user mode (no privilege requirements)

DHCP not working in VM (vmnet mode)
```shell
# Inside VM, check if udhcpc is available
which udhcpc

# If not, try manual IP configuration
ip addr add 192.168.64.2/24 dev eth0
ip link set eth0 up
ip route add default via 192.168.64.1
ping 192.168.64.1
```

Check host-side vmnet interface
```shell
# On macOS host
ifconfig | grep -A 5 bridge100
# vmnet creates bridge interfaces (e.g., bridge100)
```

### Common Issues (All Platforms)

Network interface not detected in VM
```shell
# Check if virtio-net device is present
lspci | grep -i net
# Or
ip link show

# Ensure eth0 is up
ip link set eth0 up
```

Cannot ping external hosts
```shell
# Check DNS
cat /etc/resolv.conf

# Test connectivity step by step
ping 127.0.0.1          # Loopback (should always work)
ping <gateway_ip>       # Gateway (10.0.2.2 or 192.168.10.1 or 192.168.64.1)
ping 8.8.8.8            # External IP (tests routing)
ping google.com         # External DNS (tests DNS resolution)
```

## Implementation Details

### Architecture Overview

```
┌───────────────────────────────────────────────────────────┐
│                      semu Process                         │
│  ┌──────────────────────────────────────────────────────┐ │
│  │         virtio-net Device Emulation                  │ │
│  │         (Platform-agnostic VirtIO-Net)               │ │
│  └───────────────────────┬──────────────────────────────┘ │
│                          │                                │
│  ┌───────────────────────▼──────────────────────────────┐ │
│  │         Network Device Abstraction Layer             │ │
│  │         (netdev.c/h - Platform router)               │ │
│  └────┬─────────────────────────────────┬───────────────┘ │
│       │                                 │                 │
│  ┌────▼──────────┐                 ┌────▼──────────┐      │
│  │ Linux/macOS   │                 │ macOS only    │      │
│  │ slirp.c       │                 │ netdev-vmnet.c│      │
│  └────┬──────────┘                 └────┬──────────┘      │
└───────┼─────────────────────────────────┼─────────────────┘
        │                                 │
   ┌────▼────────────┐              ┌─────▼───────────┐
   │ minislirp       │              │ vmnet.framework │
   │ (userspace NAT) │              │ (kernel NAT)    │
   │ Linux + macOS   │              │ macOS only      │
   └─────────────────┘              └─────────────────┘

Note: Linux also has TAP mode (not shown), macOS does not.
```

### Platform-Specific Implementations

Linux:
- `netdev.c`: TAP device creation (`/dev/net/tun`) and SLIRP initialization
- `slirp.c`: minislirp integration for user-mode NAT (cross-platform)

macOS:
- `netdev-vmnet.c`: vmnet.framework integration (C with Blocks)
  - GCD dispatch queue for async packet I/O
  - Pipe-based bridge to semu's poll() loop
  - Shared/Host/Bridged mode support
- `slirp.c`: Same minislirp userspace networking as Linux

Common:
- `virtio-net.c`: VirtIO-Net device emulation (used by both platforms)
- `netdev.h`: Platform abstraction interface

### Key Files

| File | Purpose | Platform |
|------|---------|----------|
| `virtio-net.c` | VirtIO-Net device emulation | All |
| `netdev.h` | Network backend abstraction | All |
| `netdev.c` | Backend initialization (TAP/user/vmnet) | All |
| `netdev-vmnet.c` | vmnet.framework backend (C with Blocks) | macOS |
| `slirp.c` | minislirp integration (userspace NAT) | Linux + macOS |
| `device.h` | Device IRQ definitions | All |

## Network Topology Examples

### Linux TAP Mode
```
┌──────────────┐ TAP  ┌──────────────┐ Physical ┌──────────┐
│   semu VM    │◄────►│  Host Linux  │◄────────►│ Internet │
│ 192.168.10.2 │      │ 192.168.10.1 │          └──────────┘
└──────────────┘      └──────────────┘
     eth0                  tap0
```

### Linux User Mode
```
┌──────────────┐ SLIRP ┌──────────────┐ NAT  ┌──────────┐
│   semu VM    │◄─────►│  minislirp   │◄────►│ Internet │
│  10.0.2.15   │       │   10.0.2.2   │      └──────────┘
└──────────────┘       │ (userspace)  │
     eth0              └──────────────┘
```

### macOS vmnet Shared Mode
```
┌──────────────┐ vmnet ┌──────────────┐ NAT  ┌──────────┐
│   semu VM    │◄─────►│ macOS Kernel │◄────►│ Internet │
│ (DHCP client)│       │192.168.64.1  │      └──────────┘
└──────────────┘       │  bridge100   │
     eth0              └──────────────┘
```

### macOS User Mode
```
┌──────────────┐ SLIRP ┌──────────────┐ NAT  ┌──────────┐
│   semu VM    │◄─────►│  minislirp   │◄────►│ Internet │
│  10.0.2.15   │       │   10.0.2.2   │      └──────────┘
└──────────────┘       │ (userspace)  │
     eth0              └──────────────┘

Same userspace network as Linux user mode.
```

## Testing

Quick network test:
```shell
# Linux (user mode - easiest, no sudo)
make check NETDEV=user

# Linux (TAP mode - requires sudo)
sudo make check NETDEV=tap

# macOS (user mode - easiest, no sudo)
make check NETDEV=user

# macOS (vmnet mode - requires sudo)
sudo make check NETDEV=vmnet
# Or simply:
sudo make check
```

Automated tests:
```shell
# Requires sudo on all platforms
sudo .ci/test-netdev.sh
```

## References

### General
- [VirtIO Specification](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html)

### Linux
- [TUN/TAP Documentation](https://www.kernel.org/doc/Documentation/networking/tuntap.txt)
- [minislirp Project](https://gitlab.freedesktop.org/slirp/libslirp)

### macOS
- [Apple vmnet Framework](https://developer.apple.com/documentation/vmnet)
- [QEMU vmnet Integration](https://lists.gnu.org/archive/html/qemu-devel/2022-01/msg02976.html)
- [socket_vmnet Project](https://github.com/lima-vm/socket_vmnet)
