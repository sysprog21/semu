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
- Three types of I/O support using VirtIO standard:
    - virtio-blk acquires disk image from the host.
    - virtio-net is mapped as TAP interface.
    - virtio-snd uses [PortAudio](https://github.com/PortAudio/portaudio) for sound playback on the host with one limitations:
        - As some unknown issues in guest Linux OS (confirmed in v6.7 and v6.12), you need
          to adjust the buffer size to more than four times of period size, or
          the program cannot write the PCM frames into guest OS ALSA stack.
            - For instance, the following buffer/period size settings on `aplay` has been tested
              with broken and stutter effects yet complete with no any errors: `aplay --buffer-size=32768 --period-size=4096 /usr/share/sounds/alsa/Front_Center.wav`.

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
./semu -k linux-image [-b dtb-file] [-i initrd-image] [-d disk-image] [-s shared-directory]
```

* `linux-image` is the path to the Linux kernel `Image`.
* `dtb-file` is optional, as it specifies the user-specified device tree blob.
* `initrd-image` is optional, as it specifies the user-specified initial RAM disk image.
* `disk-image` is optional, as it specifies the path of a disk image in ext4 file system for the virtio-blk device.
* `shared-directory` is optional, as it specifies the path of a directory on the host that will be shared with the guest operating system through virtio-fs, enabling file access from the guest via a virtual filesystem mount.

## Mount and unmount a directory in semu

To mount the directory in semu:

```shell
$ mount -t virtiofs myfs [shared-directory]
```

* `shared-directory` is the path of a directory you want to mount in semu.

To unmount the directory in semu:

```shell
$ umount [shared-directory]
```

* `shared-directory` is the path of a directory you want to unmount in semu.


## Build Linux kernel image and root file system

An automated build script is provided to compile the RISC-V cross-compiler, Busybox, and Linux kernel from source.
Please note that it only supports the Linux host environment.

To build everything, simply run:

```shell
$ make build-image
```

This command invokes the underlying script: `scripts/build-image.sh`, which also offers more flexible usage options.

### Script Usage

```
./scripts/build-image.sh [--buildroot] [--linux] [--all] [--external-root] [--clean-build] [--help]

Options:
  --buildroot         Build Buildroot rootfs
  --linux             Build Linux kernel
  --all               Build both Buildroot and Linux
  --external-root     Use external rootfs instead of initramfs
  --clean-build       Remove entire buildroot/ and/or linux/ directories before build
  --help              Show this message
```

### Examples

Build the Linux kernel only:

```
$ scripts/build-image.sh --linux
```

Build Buildroot only:

```
$ scripts/build-image.sh --buildroot
```

Build Buildroot and generate an external root file system (ext4 image):

```
$ scripts/build-image.sh --buildroot --external-root
```

Force a clean build:

```
$ scripts/build-image.sh --all --clean-build
$ scripts/build-image.sh --linux --clean-build
$ scripts/build-image.sh --buildroot --clean-build
```

## License

`semu` is released under the MIT License.
Use of this source code is governed by a MIT-style license that can be found in the LICENSE file.
