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
- I/O support using VirtIO standard:
    - virtio-blk acquires disk image from the host.
    - virtio-net is mapped as TAP interface.
    - virtio-snd uses [PortAudio](https://github.com/PortAudio/portaudio) for sound playback on the host with one limitations:
        - As some unknown issues in guest Linux OS (confirmed in v6.7 and v6.12), you need
          to adjust the buffer size to more than four times of period size, or
          the program cannot write the PCM frames into guest OS ALSA stack.
            - For instance, the following buffer/period size settings on `aplay` has been tested
              with broken and stutter effects yet complete with no any errors: `aplay --buffer-size=32768 --period-size=4096 /usr/share/sounds/alsa/Front_Center.wav`.
    - virtio-input exposes SDL-backed keyboard and mouse devices to the guest.
    - virtio-gpu exposes a minimal 2D DRM/KMS device to the guest. Linux can
      bind the `virtio_gpu` driver and create `/dev/dri/card0`.
      - Only 2D scanout is currently supported; 3D, virgl, and blob resources
        are not implemented yet.
    - Press Ctrl+Alt+G to release the mouse cursor from the SDL window.

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

Download the prebuilt guest artifacts and run the default check:
```shell
$ make check
```

With the default external-root build, `make check` uses `Image`, `minimal.dtb`,
and `ext4.img`, and boots `semu` headlessly with an equivalent command line:

```shell
$ ./semu -k Image -c 1 -b minimal.dtb -H -d ext4.img
```

If `ENABLE_EXTERNAL_ROOT=0` is used, `make check` switches to the legacy
initramfs path and passes `-i rootfs.cpio` instead of `-d ext4.img`.

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

To test virtio-gpu with a visible SDL window, run `semu` manually without `-H`.
Make sure `sdl2-config` is in `PATH`, then build the emulator, DTB, kernel, and
test tools disk. Press `Ctrl+Alt+G` to release the mouse cursor from the SDL
window:

```shell
$ sdl2-config --version
$ make semu minimal.dtb Image test-tools.img
$ ./semu -k Image -c 1 -b minimal.dtb -d test-tools.img
```

Log in as `root`, source the test-tools image environment, and run one of
the DirectFB2 examples:

```
# . /root/local-env.sh
# df_drivertest
```

The installed DirectFB2 examples come from the upstream DirectFB-examples
project and can be listed in the guest with:

```
# ls /usr/local/bin/df_*
```

## Usage

```shell
./semu -k linux-image [-b dtb-file] [-d disk-image] [-i initrd-image] [-s shared-directory] [-H]
```

* `linux-image` is the path to the Linux kernel `Image`.
* `dtb-file` is optional, as it specifies the user-specified device tree blob.
* `disk-image` is the ext4 image exposed as `/dev/vda` to the guest. The
  default boot path mounts this as the root filesystem; `make` builds it
  from `rootfs.cpio` via `scripts/rootfs_ext4.sh`.
* `shared-directory` is optional, as it specifies the path of a directory on the host that will be shared with the guest operating system through virtio-fs, enabling file access from the guest via a virtual filesystem mount.
* `-H` (or `--headless`) skips SDL window creation; useful for CI and `make check`.
* `initrd-image` is optional and only used on the *legacy* boot path.
  The default `minimal.dtb` built with `ENABLE_EXTERNAL_ROOT=1` does not
  advertise initrd placement, so `-i` there requires either
  `ENABLE_EXTERNAL_ROOT=0` or a custom DTB passed with `-b`. See *Boot mode*
  below.

### Boot mode

The default build (`make`) boots the kernel directly from `/dev/vda` and
runs `/sbin/init` from the ext4 root, skipping the initramfs unpack step
entirely. This is faster, avoids the RCU-stall the kernel hits when
unpacking a large cpio, and matches how real systems deploy. The
`ext4.img` is built from `rootfs.cpio` via `scripts/rootfs_ext4.sh`,
which requires `fakeroot` and `mkfs.ext4`.

The rolling `prebuilt` release provides an optional `test-tools.img.bz2`,
an ext4-formatted disk image for larger test/user tools that should not
inflate `rootfs.cpio` or the default `ext4.img`. Use `make test-tools.img`
to download it.

If `fakeroot` is missing, the build falls back to the legacy initramfs
path (`-i rootfs.cpio`) automatically and prints a one-line warning. To
force the legacy path explicitly:

```shell
$ make ENABLE_EXTERNAL_ROOT=0
$ make ENABLE_EXTERNAL_ROOT=0 check
```

The legacy path uses what the flag is still spelled as: `-i initrd-image`.
That is a runtime choice only when the DTB also carries
`linux,initrd-{start,end}`. semu's default external-root build emits a DTB
that always boots from `/dev/vda`, so `-i` is rejected there unless you
replace the DTB with one that describes the initrd layout.
The classical *initrd* (a filesystem image mounted as `/dev/ram0`,
pivoted into via `pivot_root`) is effectively obsolete -- it required
`CONFIG_BLK_DEV_INITRD` plus the legacy ramdisk block driver, an
in-kernel filesystem driver to mount the image before any userspace code
ran, and a `/linuxrc` handoff. Mainstream distros and embedded builds
dropped that path more than a decade ago. Linux 2.6+ kept the flag and
the `linux,initrd-{start,end}` device-tree properties, but the kernel
inspects the loaded blob: a cpio archive is unpacked into the in-memory
`rootfs` (initramfs path, runs `/init`); a filesystem image still falls
back to the legacy initrd path if that driver is configured in. semu
ships and consumes a cpio (`rootfs.cpio`), so the legacy build is
exercising the initramfs path even though the CLI flag spelling stayed
`-i initrd-image`.

For detailed networking guidance, see [`docs/networking.md`](docs/networking.md).

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

This command invokes the underlying script: `scripts/build-image.sh --all`, which also offers more flexible usage options.

### Script Usage

```
./scripts/build-image.sh [--buildroot] [--linux] [--directfb2-test] [--all] [--no-ext4] [--clean-build] [--help]

Options:
  --buildroot         Build Buildroot userland (produces rootfs.cpio and,
                      unless --no-ext4 is given, ext4.img for vda boot)
  --directfb2-test    Build test-tools.img with the DirectFB2 test payload
  --linux             Build the Linux kernel
  --all               Build both Buildroot and Linux
  --no-ext4           Skip ext4.img generation; produce only rootfs.cpio
                      (matches the legacy ENABLE_EXTERNAL_ROOT=0 path)
  --clean-build       Remove buildroot/ and/or linux/ before building;
                      with --directfb2-test, also remove DirectFB2 build outputs
  --help              Show this message
```

### Examples

Build the Linux kernel only:

```
$ scripts/build-image.sh --linux
```

Build Buildroot (produces both `rootfs.cpio` and `ext4.img`):

```
$ scripts/build-image.sh --buildroot
```

Build Buildroot for the legacy initramfs-only path (no ext4):

```
$ scripts/build-image.sh --buildroot --no-ext4
```

`test-tools.img` is the shared optional disk for test payloads that should
not live in the default `rootfs.cpio` or `ext4.img`. This keeps the default
guest image small while still allowing larger tools to be collected in one
place.

Build Buildroot and the test tools image with the DirectFB2 test payload:

```
$ scripts/build-image.sh --directfb2-test
```

To add a new test tool, extend the `test-tools.img` build path in
`scripts/build-image.sh` so the tool is staged into `extra_packages`, then
update `target/local-env.sh` if the tool needs an additional binary or library
search path.

The build script copies `target/local-env.sh` to `/root/local-env.sh` in the
test tools image. After booting the VM, source it once to pick up paths such
as `/usr/local/bin` and `/usr/local/lib`, instead of running overlaid tools
through full paths like `/usr/local/bin/df_*`.

Force a clean build:

```
$ scripts/build-image.sh --all --clean-build
$ scripts/build-image.sh --linux --clean-build
$ scripts/build-image.sh --buildroot --clean-build
```

## License

`semu` is released under the MIT License.
Use of this source code is governed by a MIT-style license that can be found in the LICENSE file.
