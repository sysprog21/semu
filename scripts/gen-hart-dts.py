import sys

def cpu_template (id):
    return f"""cpu{id}: cpu@{id} {{
            device_type = "cpu";
            compatible = "riscv";
            reg = <{id}>;
            riscv,isa = "rv32ima";
            mmu-type = "riscv,sv32";
            cpu{id}_intc: interrupt-controller {{
                #interrupt-cells = <1>;
                #address-cells = <0>;
                interrupt-controller;
                compatible = "riscv,cpu-intc";
            }};
        }};
        """

def cpu_format(nums):
    s = ""
    for i in range(nums):
        s += cpu_template(i)
    return s

def plic_irq_format(nums):
    s = ""
    for i in range(nums):
        s += f"<&cpu{i}_intc 9>, "
    return s[:-2]

def clint_irq_format(nums):
    s = ""
    for i in range(nums):
        s += f"<&cpu{i}_intc 3 &cpu{i}_intc 7>, "
    return s[:-2]

def dtsi_template (cpu_list: str, plic_list, clint_list):
    return f"""/{{
    cpus {{
        #address-cells = <1>;
        #size-cells = <0>;
        timebase-frequency = <65000000>;
        {cpu_list}
    }};

    soc: soc@F0000000 {{
        plic0: interrupt-controller@0 {{
            #interrupt-cells = <1>;
            #address-cells = <0>;
            compatible = "sifive,plic-1.0.0";
            reg = <0x0000000 0x4000000>;
            interrupt-controller;
            interrupts-extended = {plic_list};
            riscv,ndev = <31>;
        }};

        clint0: clint@4300000 {{
            compatible = "riscv,clint0";
            interrupt-controller;
            interrupts-extended =
            {clint_list};
            reg = <0x4300000 0x10000>;
        }};
    }};
}};
"""

dtsi = sys.argv[1]
harts = int(sys.argv[2])

with open(dtsi, "w") as dts:
    dts.write(dtsi_template(cpu_format(harts), plic_irq_format(harts), clint_irq_format(harts)))
