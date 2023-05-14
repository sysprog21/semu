# semu

A minimalist RISC-V emulator capable of running Linux the kernel and corresponding userland.
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

[Device Tree](https://www.kernel.org/doc/html/latest/devicetree/) compiler (dtc) is required. Install by entering the following command:
```shell
sudo apt install device-tree-compiler
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

## License

`semu` is released under the MIT License.
Use of this source code is governed by a MIT-style license that can be found in the LICENSE file.
