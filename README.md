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

## Build and Run

Build the emulator:
```shell
$ make
```

Download prebuilt xv6 kernel and file system image:
```shell
$ make check
```

Please be patient while `semu` is running.

Reference output:
```
$ ./semu kernel.bin fs.img

xv6 kernel is booting

init: starting sh
$
```

## RISC-V ISA Coverage Test

Although `semu` was intended to run xv6 with minimal efforts, it would be still better if we can validate the ISA compatibility.
The support of [riscv-tests](https://github.com/riscv-software-src/riscv-tests) is integrated, and you should set up [RISC-V GNU Compiler Toolchain](https://github.com/riscv-collab/riscv-gnu-toolchain) in advance.
You can obtain prebuilt GNU toolchain for `riscv64` via [Automated Nightly Release](https://github.com/riscv-collab/riscv-gnu-toolchain/releases).
Then, run the following command:
```shell
$ make ENABLE_RISCV_TESTS=1 clean run-tests
```

You can check the generated report as following:
```shell
[==========] Running 71 test(s) from riscv-tests.
[ RUN      ] rv64ui-p-add
[       OK ] rv64ui-p-add
[ RUN      ] rv64ui-p-addi
[       OK ] rv64ui-p-addi

...

[ RUN      ] rv64ua-p-lrsc
  a0 = 0x80002008
  tohost = 0x53b
  An exception occurred.
[  FAILED  ] rv64ua-p-lrsc
[==========] 71 test(s) from riscv-tests ran.
[  PASSED  ] 55 test(s).
[  FAILED  ] 16 test(s), listed below:
[  FAILED  ] rv64ui-p-fence_i
[  FAILED  ] rv64ua-p-amoand_d
...
```

If you want to execute a specific test instead of the whole test, please run the following command:
```shell
$ ./semu --test <test_case_name>
```

You can refer to `tests/isa-test.c` for the name of specific test case.
Take `rv64ui-p-add` for example, the report would be:
```shell
$ ./semu --test rv64ui-p-add
[==========] Running 1 test(s) from riscv-tests.
[ RUN      ] rv64ui-p-add
[       OK ] rv64ui-p-add
[==========] 1 test(s) from riscv-tests ran.
[  PASSED  ] 1 test(s).
```

## Acknowledgements

`semu` is inspired by [rvemu](https://github.com/d0iasm/rvemu).

## License

`semu` is released under the MIT License.
Use of this source code is governed by a MIT-style license that can be found in the LICENSE file.
