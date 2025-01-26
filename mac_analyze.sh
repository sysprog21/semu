#!/usr/bin/env bash
#
# parse_results_macos.sh
#
# Parses log files of the form: emulator_SMP_{N}.log
# Each log ends with lines like:
#   [SEMU LOG]: Real boot time: 43.63820 seconds, called 220128512 times semu_timer_clocksource
#   [SEMU LOG]: ns_per_call = 4.46425, predict_sec = 8.92851, scale_factor = 1.12001
#
# We output a table with columns:
#   1) SMP
#   2) real_boot_time
#   3) times_called
#   4) ns_per_call
#   5) predict_sec
#   6) scale_factor
#
# We do not parse total_clocksource_ns or percentage, as they do not appear in this log snippet.

LOGDIR="mac_log"      # Directory containing emulator_SMP_{N}.log
OUTFILE="results_summary.txt"

# Print a header:
echo -e "SMP\treal_boot_time\ttimes\t\tns_per_call\tpredict_sec\tscale_factor" > "$OUTFILE"

# Iterate from SMP=1..32 (adjust if needed)
for N in $(seq 1 32); do
    FILE="$LOGDIR/emulator_SMP_${N}.log"

    if [[ ! -f "$FILE" ]]; then
        echo "Skipping N=$N, file not found: $FILE"
        continue
    fi

    # Initialize variables
    real_boot_time=""
    times_called=""
    ns_per_call=""
    predict_sec=""
    scale_factor=""

    # 1) Parse the "Real boot time" line:
    # Example:
    #   [SEMU LOG]: Real boot time: 43.63820 seconds, called 220128512 times semu_timer_clocksource
    line_boot="$(grep 'Real boot time:' "$FILE")"
    if [[ -n "$line_boot" ]]; then
        # Remove ANSI color codes, if any
        line_no_ansi="$(echo "$line_boot" | sed 's/\x1b\[[0-9;]*m//g')"
        # e.g. "[SEMU LOG]: Real boot time: 43.63820 seconds, called 220128512 times semu_timer_clocksource"
        # We'll extract:
        #   real_boot_time = 43.63820
        #   times = 220128512
        real_boot_time="$(echo "$line_no_ansi" | sed -E 's/.*Real boot time: ([0-9.]+) seconds, called ([0-9]+) .*/\1/')"
        times_called="$(echo "$line_no_ansi"    | sed -E 's/.*Real boot time: ([0-9.]+) seconds, called ([0-9]+) .*/\2/')"
    fi

    # 2) Parse the "ns_per_call" line:
    # Example:
    #   [SEMU LOG]: ns_per_call = 4.46425, predict_sec = 8.92851, scale_factor = 1.12001
    line_ns="$(grep 'ns_per_call =' "$FILE")"
    if [[ -n "$line_ns" ]]; then
        # Also remove ANSI codes
        ns_no_ansi="$(echo "$line_ns" | sed 's/\x1b\[[0-9;]*m//g')"
        # e.g. "ns_per_call = 4.46425, predict_sec = 8.92851, scale_factor = 1.12001"
        # We'll extract them
        ns_per_call="$(echo "$ns_no_ansi"  | sed -E 's/.*ns_per_call = ([0-9.]+).*/\1/')"
        predict_sec="$(echo "$ns_no_ansi"  | sed -E 's/.*predict_sec = ([0-9.]+).*/\1/')"
        scale_factor="$(echo "$ns_no_ansi" | sed -E 's/.*scale_factor = ([0-9.]+).*/\1/')"
    fi

    # 3) Print a line with the data
    echo -e "${N}\t${real_boot_time}\t${times_called}\t${ns_per_call}\t${predict_sec}\t${scale_factor}" >> "$OUTFILE"
done

echo "Done. Results saved to ${OUTFILE}."
