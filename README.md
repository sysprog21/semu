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

### 1. Environment setup


Before starting everything, the following packages are essential:

```
sudo apt install autoconf automake autotools-dev curl gawk git build-essential bison flex texinfo gperf libtool libncurses-dev
```

Since the process to be demonstrated involves switching between several project directories, we can handle them with a workspace directory as:

```
mkdir semu_ws/
cd semu_ws/
```

We then clone the source code of the semu:

```
git clone https://github.com/jserv/semu.git
```

Later, the directory structure should look like:

```
semu_ws/
   |
   |----- semu/
   |
   |----- linux/
   |
   |----- buildroot/
   |
   |----- riscv-gnu-toolchain/
```

### 2. Compile RISC-V GNU Toolchain

Now, we need to compile the GNU toolchain that targets the RV32 ISA with Linux ABI. This can be done by:

```
git clone --recursive https://github.com/riscv/riscv-gnu-toolchain
cd riscv-gnu-toolchain/
mkdir build/
cd build/ 
../configure --prefix=/opt/riscv --with-arch=rv32gc --with-abi=ilp32d 
sudo make linux -j$(nproc)
```

After the compilation, type the following command:

```
export PATH=$PATH:/opt/riscv/bin
```

Alternatively, you can save the setting into the `~/.bashrc` and restart the terminal.

### 3. Create root file system with Buildroot

Next, we can use [Buildroot](https://buildroot.org/) to create the root file system, which is essential for building the kernel image.

To download and decompress the source code of Buildroot:

```
cd semu_ws/
wget https://buildroot.org/downloads/buildroot-2023.02.3.tar.gz
tar -xvf buildroot-2023.02.3.tar.gz
mv buildroot-2023.02.3/ buildroot/
```

Usually, Buildroot requires the user to set up its configurations, but here we can copy the settings from the semu:

```
cp semu/configs/buildroot.config buildroot/.config
```

Then, to build the root file system, type:

```
make -j$(nproc) #Type [enter] if you encounter any questions
```

If everything goes right, you should be able to find the result at  `buildroot/output/images/rootfs.cpio`

### 4. Compile and build Linux kernel image

Next, clone the kernel source code specified with version `v6.4-rc1`:

```
cd semu_ws/
git clone https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git -b v6.4-rc1
```

Before cross-compiling the kernel, set up the following environment variables:

```
export CROSS_COMPILE=riscv32-unknown-linux-gnu-
export ARCH=riscv
```

Again, copy the configuration file from the semu:

```
cp semu/configs/linux.config linux/.config
cd linux/
make oldconfig
```

Then, start the compilation by typing:

```
make -j$(nproc)
```

After all, you can replace the old kernel image of semu with:

```
cp arch/riscv/boot/Image ../semu/Image
```

## License

`semu` is released under the MIT License.
Use of this source code is governed by a MIT-style license that can be found in the LICENSE file.
