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
  
def sswi_irq_format(nums):
    s = ""
    for i in range(nums):
        s += f"<&cpu{i}_intc 1>, "    # 1 is the SSWI interrupt number (Supervisor Software Interrupt)
    return s[:-2]
  
def mswi_irq_format(nums):
    s = ""
    for i in range(nums):
        s += f"<&cpu{i}_intc 3>, "    # 3 is the MSWI interrupt number (Machine Software Interrupt)
    return s[:-2]
  
def mtimer_irq_format(nums):
    s = ""
    for i in range(nums):
        s += f"<&cpu{i}_intc 7>, "    # 7 is the MTIMER interrupt number (Machine Timer Interrupt)
    return s[:-2]

def dtsi_template (cpu_list: str, plic_list, sswi_list, mswi_list, mtimer_list, clock_freq):
    return f"""/{{
    cpus {{
        #address-cells = <1>;
        #size-cells = <0>;
        timebase-frequency = <{clock_freq}>;
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

        sswi0: sswi@4500000 {{
          #interrupt-cells = <0>;
          #address-cells = <0>;
          interrupt-controller;
          interrupts-extended = {sswi_list};
          reg = <0x4500000 0x4000>;
          compatible = "riscv,aclint-sswi";
        }};

        mswi0: mswi@4400000 {{
          #interrupt-cells = <0>;
          #address-cells = <0>;
          interrupt-controller;
          interrupts-extended = {mswi_list};
          reg = <0x4400000 0x4000>;
          compatible = "riscv,aclint-mswi";
        }};
        
        mtimer0: mtimer@4300000 {{
          interrupts-extended = {mtimer_list};
          reg = <0x4300000 0x8000>;
          compatible = "riscv,aclint-mtimer";
        }};
    }};
}};
"""

dtsi = sys.argv[1]
harts = int(sys.argv[2])
clock_freq = int(sys.argv[3])

with open(dtsi, "w") as dts:
    dts.write(dtsi_template(cpu_format(harts), plic_irq_format(harts), sswi_irq_format(harts), mswi_irq_format(harts), mtimer_irq_format(harts), clock_freq))
