#!/usr/bin/env python3
"""
plot_results.py

Reads 5 "results_summary_{N}.txt" files, each containing columns:
 1) SMP
 2) real_boot_time
 3) times_call_semu_timer_clocksource
 4) ns_per_call
 5) predict_sec
 6) scale_factor
 7) percentage
 8) real_ns_per_call

We skip the header row in each file. For each column 2..8, we create
one figure, plotting SMP on the x-axis vs. that column's values
on the y-axis. We have 5 lines in each plot, one line per environment.

Additionally, we create one more figure for the difference:
  (predict_sec [col 5]) - (real_boot_time [col 2])

Environment CPU Descriptions:
1: 13th Gen Intel(R) Core(TM) i7-13700
2: Intel(R) Xeon(R) CPU E5-2640 v4 @ 2.40GHz
3: Intel(R) Core(TM) i7-1065G7 CPU @ 1.30GHz
4: arm (ThunderX2-99xx)
5: mag
"""

import matplotlib
# Force a non-interactive backend (no hover tooltips):
matplotlib.use("Agg")

import numpy as np
import matplotlib.pyplot as plt

# Filenames expected
#   results_summary_1.txt
#   results_summary_2.txt
#   results_summary_3.txt
#   results_summary_4.txt
#   results_summary_5.txt

ENV_FILES = [
    "results_summary_1.txt",
    "results_summary_2.txt",
    "results_summary_3.txt",
    "results_summary_4.txt",
    "results_summary_5.txt",
    "results_summary_6.txt"
]

ENV_NAMES = [
    "1: Env1",
    "2: Env2",
    "3: Env3",
    "4: Env4",
    "5: Env5",
    "6: Env6"
]

# Map from column index to a label (column 1 is SMP)
# We want columns 2..8
COL_LABELS = {
    2: "real boot time (sec)",
    3: "times call 'semu_timer_clocksource'",
    4: "ns_per_call",
    5: "predict boot time (sec)",
    6: "scale_factor",
}

def main():
    # 1) Plot each data column from 2..8
    for col in range(2, 7):
        label = COL_LABELS[col]

        plt.figure(figsize=(8, 6))
        plt.title(f"{label} vs. SMP")
        plt.xlabel("SMP")
        plt.ylabel(label)

        for i, fname in enumerate(ENV_FILES):
            try:
                # Load numeric data, skipping first row (header).
                data = np.loadtxt(fname, skiprows=1)
            except OSError:
                print(f"Warning: could not open {fname}, skipping.")
                continue

            if data.shape[1] < 6:
                print(f"Warning: {fname} does not have at least 8 columns, skipping.")
                continue

            x = data[:, 0]              # SMP is col 1 => index=0
            y = data[:, col - 1]       # col -> zero-based

            plt.plot(x, y, marker='o', label=ENV_NAMES[i])

        plt.legend(loc="best")
        plt.grid(True)

        outname = f"{label}.png"
        plt.savefig(outname, dpi=150)
        plt.close()
        print(f"Created {outname}")

    print("All standard column plots generated!")

    # 2) Create an additional figure for (predict_sec - real_boot_time).
    #    predict_sec is column 5 => index 4
    #    real_boot_time is column 2 => index 1
    label_diff = "boot time difference"  # name of the new figure

    plt.figure(figsize=(8, 6))
    plt.title(f"{label_diff} vs. SMP")
    plt.xlabel("SMP")
    plt.ylabel("boot time difference (seconds)")
    for i, fname in enumerate(ENV_FILES):
        try:
            data = np.loadtxt(fname, skiprows=1)
        except OSError:
            print(f"Warning: could not open {fname}, skipping.")
            continue

        if data.shape[1] < 6:
            print(f"Warning: {fname} does not have at least 8 columns, skipping.")
            continue

        x = data[:, 0]  # SMP
        predict_sec = data[:, 4]      # col5 => index=4
        real_bt = data[:, 1]         # col2 => index=1
        diff = predict_sec - real_bt # difference

        plt.plot(x, diff, marker='o', label=ENV_NAMES[i])

    plt.legend(loc="best")
    plt.grid(True)
    outdiff = f"{label_diff}.png"
    plt.savefig(outdiff, dpi=150)
    plt.close()
    print(f"Created {outdiff}")

    print("Done! All plots generated including difference (predict - real_boot_time).")

if __name__ == "__main__":
    main()
