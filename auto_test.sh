#!/usr/bin/env bash

# Create a directory to store logs (optional)
mkdir -p logs

for N in $(seq 1 32); do
    echo "============================================="
    echo "Starting experiment with SMP=$N"
    echo "============================================="

    echo "Building and running 'make check SMP=$N'..."
    make check SMP=$N 2>&1 | tee "logs/emulator_SMP_${N}.log"

    echo "Done with SMP=$N. Logs saved:"
    echo "  - logs/emulator_SMP_${N}.log"
    echo
done

echo "All experiments complete!"
