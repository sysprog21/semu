#!/usr/bin/env bash

# Create a directory to store logs (optional)
mkdir -p logs

for N in $(seq 1 32); do
    echo "============================================="
    echo "Starting experiment with SMP=$N"
    echo "============================================="

    # 2) Build and run checks with SMP=N, capturing emulator output
    #    The 'tee' command copies output to the terminal AND a log file
    echo "Building and running 'make check SMP=$N'..."
    make check SMP=$N 2>&1 | tee "logs/emulator_SMP_${N}.log"

    # 3) After the emulator run, record gprof output
    #    We assume 'gprof ./semu' uses data from 'gmon.out'
    echo "Running gprof for SMP=$N..."
    gprof ./semu > "logs/gprof_SMP_${N}.log" 2>&1

    echo "Done with SMP=$N. Logs saved:"
    echo "  - logs/emulator_SMP_${N}.log"
    echo "  - logs/gprof_SMP_${N}.log"
    echo
done

echo "All experiments complete!"
