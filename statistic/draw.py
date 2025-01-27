#!/usr/bin/env python3
"""
draw.py

We have 6 data files:
  results_summary_1.txt
  results_summary_2.txt
  results_summary_3.txt
  results_summary_4.txt
  results_summary_5.txt
  results_summary_6.txt

Each has 11 columns (skip the first row as a header):
 1) SMP
 2) real_boot_time
 3) times_called
 4) ns_per_call
 5) predict_sec
 6) scale_factor
 7) test_total_clocksource_ns
 8) real_total_clocksource_ns
 9) percentage
 10) real_ns_per_call
 11) diff_ns_per_call

We want to plot columns 2..11 (10 columns) vs. column 1 (SMP).
Then create 1 more difference plot for (predict_sec - real_boot_time).

Thus, we produce 11 plots total:
 - 10 from col=2..11
 - 1 difference plot (predict_sec - real_boot_time).
"""

import matplotlib
matplotlib.use("Agg")  # for non-interactive environments

import numpy as np
import matplotlib.pyplot as plt

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
    "6: Env6",
]

# A dictionary for column labels (2..11):
COL_LABELS = {
    2: "real_boot_time",
    3: "times_called",
    4: "predict_ns_per_call",
    5: "predict_boot_time",
    6: "scale_factor",
    7: "test_total_clocksource_ns",
    8: "real_total_clocksource_ns",
    9: "percentage",
    10: "real_ns_per_call",
    11: "diff_ns_per_call",
}

def main():
    # 1) Create a figure for each column from 2..11
    for col in range(2, 12):  # 2..11 inclusive => 10 columns
        label = COL_LABELS[col]

        plt.figure(figsize=(8, 6))
        plt.title(f"{label} vs. SMP")
        plt.xlabel("SMP")
        plt.ylabel(label)

        # For each environment data
        for i, fname in enumerate(ENV_FILES):
            try:
                # skiprows=1 => skip header line
                # Load all columns => we have 11 total
                data = np.loadtxt(fname, skiprows=1)
            except OSError:
                print(f"Warning: could not open {fname}. Skipping.")
                continue
            except ValueError as e:
                print(f"ValueError loading {fname}: {e}")
                continue

            # We expect at least 11 columns
            if data.shape[1] < 11:
                print(f"Warning: {fname} does not have 11 columns. Found shape: {data.shape}")
                continue

            x = data[:, 0]           # SMP is col=1 => index=0
            y = data[:, col - 1]     # col => zero-based index

            plt.plot(x, y, marker='o', label=ENV_NAMES[i])

        plt.legend(loc="best")
        plt.grid(True)
        outfile = f"{label}.png"
        plt.savefig(outfile, dpi=150)
        plt.close()
        print(f"Created {outfile}")

    # 2) Create an additional difference plot for (predict_sec - real_boot_time).
    #    predict_sec => col=5 => index=4
    #    real_boot_time => col=2 => index=1
    label_diff = "diff_boot_time"
    plt.figure(figsize=(8, 6))
    plt.title(f"{label_diff} vs. SMP")
    plt.xlabel("SMP")
    plt.ylabel("predict_sec - real_boot_time")

    for i, fname in enumerate(ENV_FILES):
        try:
            data = np.loadtxt(fname, skiprows=1)
        except OSError:
            print(f"Warning: could not open {fname}. Skipping.")
            continue
        except ValueError as e:
            print(f"ValueError loading {fname}: {e}")
            continue

        if data.shape[1] < 11:
            print(f"Warning: {fname} does not have 11 columns. Found shape: {data.shape}")
            continue

        x = data[:, 0]
        predict_sec = data[:, 4]     # col=5 => index=4
        real_bt = data[:, 1]        # col=2 => index=1
        diff = predict_sec - real_bt

        plt.plot(x, diff, marker='o', label=ENV_NAMES[i])

    plt.legend(loc="best")
    plt.grid(True)
    outfile_diff = f"{label_diff}.png"
    plt.savefig(outfile_diff, dpi=150)
    plt.close()
    print(f"Created {outfile_diff}")

    print("Done! Generated 10 standard plots (cols=2..11) + 1 difference plot (predict - real).")

if __name__ == "__main__":
    main()
