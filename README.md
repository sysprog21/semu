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
- VirtIO: virtio-net, mapped as TAP interface

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

## Build Linux kernel image and root file system

## License

`semu` is released under the MIT License.
Use of this source code is governed by a MIT-style license that can be found in the LICENSE file.
