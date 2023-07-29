# semu

A minimalist RISC-V system emulator capable of running Linux the kernel and corresponding userland.
`semu` implements the following:
- RISC-V instruction set architecture: RV32IMA
- Privilege levels: S and U modes
- Control and status registers (CSR)
- Virtual memory system: RV32 MMU
- UART: 8250/16550
- PLIC (platform-level interrupt controller): 32 interrupts, no priority
- Standard SBI, with the timer extension
- VirtIO: virtio-blk acquires disk image from the host, and virtio-net is mapped as TAP interface

## Prerequisites

[Device Tree](https://www.kernel.org/doc/html/latest/devicetree/) compiler (dtc) is required.
To install it on Debian/Ubuntu Linux, enter the following command:
```shell
$ sudo apt install device-tree-compiler
```

For macOS, use the following command:
```shell
$ brew install dtc
```

For demonstration purposes, ext4 is used for file system mounting.
`ext4` is a native Linux filesystem, offering stability, high capacity, reliability,
and performance while requiring minimal maintenance. The `mkfs.ext4` command can
create an ext4 file system from disk partitions. This command is a symbolic link of
the [mke2fs](https://man7.org/linux/man-pages/man8/mke2fs.8.html) command, and its
usage is the same as the mke2fs command.

For most GNU/Linux distributions, `mkfs.ext4` command should be installed in advance.
For macOS, use the following command:
```shell
$ brew install e2fsprogs
```

## Build and Run

Build the emulator:
```shell
$ make
```

Download prebuilt Linux kernel image:
```shell
$ make check
```

Please be patient while `semu` is running.

Reference output:
```
Starting syslogd: OK
Starting klogd: OK
Running sysctl: OK
Starting network: OK

Welcome to Buildroot
buildroot login:
```

Enter `root` to access shell.

You can exit the emulator using: \<Ctrl-a x\>. (press Ctrl+A, leave it, afterwards press X)

## Usage

```shell
./semu -k linux-image [-b dtb-file] [-i initrd-image] [-d disk-image]
```

* `linux-image` is the path to the Linux kernel `Image`.
* `dtb-file` is optional, as it specifies the user-specified device tree blob.
* `initrd-image` is optional, as it specifies the user-specified initial RAM disk image.
* `disk-image` is optional, as it specifies the path of a disk image in ext4 file system for the virtio-blk device.

## Build Linux kernel image and root file system

An automated build script is provided to compile the RISC-V cross-compiler, Busybox, and Linux kernel from source.
Please note that it only supports the Linux host environment.

```shell
$ make build-image
```

## License

`semu` is released under the MIT License.
Use of this source code is governed by a MIT-style license that can be found in the LICENSE file.
