# semu

A minimalist RISC-V emulator capable of running [xv6](https://github.com/mit-pdos/xv6-riscv).
`semu` implements the following:
- RISC-V instruction set architecture: RV64G (general-purpose ISA)
- Privilege levels
- Control and status registers
- Virtual memory system: Sv39
- UART
- CLINT
- PLIC (platform-level interrupt controller)
- Virtio

The role of a CPU is to execute a program consisting of binaries sequentially by three steps:
- Fetch: Fetches the next instruction to be executed from the memory where the program is stored.
- Decode: Splits an instruction sequence into a form that makes sense to the CPU, defined in [Volume I: Unprivileged ISA](https://riscv.org/technical/specifications/).
- Execute: Performs the action required by the instruction. In hardware, arithmetic operations such as addition and subtraction are performed by ALU (Arithmetic logic unit).

## Acknowledgements

`semu` is inspired by [rvemu](https://github.com/d0iasm/rvemu).

## License

`semu` is released under the MIT License.
Use of this source code is governed by a MIT-style license that can be found in the LICENSE file.
