#!/usr/bin/env bash
#
# parse_results_new.sh
#
# Parses new logs named "emulator_SMP_{N}.log" (N=1..32). Each log's final lines look like:
#
#   [SEMU LOG]: Real boot time: 233.04606 seconds, called 3628365913 times semu_timer_clocksource
#   [SEMU LOG]: ns_per_call = 6.26153, predict_sec = 225.41525, scale_factor = 0.04436
#   [SEMU LOG]: test_total_clocksource_ns = 92301869299, real_total_clocksource_ns = 46863590994, percentage = 0.20109
#   [SEMU LOG]: real_ns_per_call = 12.91589, diff_ns_per_call = 6.65436
#
# We output results_summary.txt with 11 columns in tab-delimited format:
#   1) SMP
#   2) real_boot_time
#   3) times_called
#   4) ns_per_call
#   5) predict_sec
#   6) scale_factor
#   7) test_total_clocksource_ns
#   8) real_total_clocksource_ns
#   9) percentage
#   10) real_ns_per_call
#   11) diff_ns_per_call
#
# We specifically remove any ANSI color codes and ensure each line is a single line, so the output doesn't break.

LOGDIR="logs"           # Directory containing the log files
OUTFILE="results_summary.txt"

# Print header (11 columns)
echo -e "SMP\treal_boot_time\ttimes_called\tns_per_call\tpredict_sec\tscale_factor\ttest_total_clocksource_ns\treal_total_clocksource_ns\tpercentage\treal_ns_per_call\tdiff_ns_per_call" > "$OUTFILE"

for N in $(seq 1 32); do
    FILE="$LOGDIR/emulator_SMP_${N}.log"

    if [[ ! -f "$FILE" ]]; then
        echo "Skipping N=$N; file not found: $FILE"
        continue
    fi

    # Initialize variables
    real_boot_time=""
    times_called=""
    ns_per_call=""
    predict_sec=""
    scale_factor=""
    test_total_clocksource_ns=""
    real_total_clocksource_ns=""
    percentage=""
    real_ns_per_call=""
    diff_ns_per_call=""

    # A helper function to grep for a specific pattern once, strip ANSI codes, unify line
    grep_single_line() {
        # Usage: grep_single_line "<pattern>"
        # We'll grep for this pattern, take only the first match, remove color codes, unify line
        grep -m1 "$1" "$FILE" \
          | sed 's/\x1b\[[0-9;]*m//g' \
          | tr '\n' ' '
    }

    # 1) Real boot time line
    # e.g. "[SEMU LOG]: Real boot time: 233.04606 seconds, called 3628365913 times semu_timer_clocksource"
    line_boot="$(grep_single_line 'Real boot time:')"
    if [[ -n "$line_boot" ]]; then
        # extract real_boot_time, times_called
        real_boot_time="$(echo "$line_boot" | sed -E 's/.*Real boot time: ([0-9.]+) seconds, called ([0-9]+) .*/\1/')"
        times_called="$(echo "$line_boot"    | sed -E 's/.*Real boot time: ([0-9.]+) seconds, called ([0-9]+) .*/\2/')"
    fi

    # 2) ns_per_call line
    # e.g.: "[SEMU LOG]: ns_per_call = 6.26153, predict_sec = 225.41525, scale_factor = 0.04436"
    line_nscall="$(grep_single_line 'ns_per_call =')"
    if [[ -n "$line_nscall" ]]; then
        ns_per_call="$(echo "$line_nscall"     | sed -E 's/.*ns_per_call = ([0-9.]+).*/\1/')"
        predict_sec="$(echo "$line_nscall"     | sed -E 's/.*predict_sec = ([0-9.]+).*/\1/')"
        scale_factor="$(echo "$line_nscall"    | sed -E 's/.*scale_factor = ([0-9.]+).*/\1/')"
    fi

    # 3) total_clocksource_ns line
    # e.g. "[SEMU LOG]: test_total_clocksource_ns = 92301869299, real_total_clocksource_ns = 46863590994, percentage = 0.20109"
    line_totals="$(grep_single_line 'test_total_clocksource_ns =')"
    if [[ -n "$line_totals" ]]; then
        test_total_clocksource_ns="$(echo "$line_totals"  | sed -E 's/.*test_total_clocksource_ns = ([0-9]+).*/\1/')"
        real_total_clocksource_ns="$(echo "$line_totals"  | sed -E 's/.*real_total_clocksource_ns = ([0-9]+).*/\1/')"
        percentage="$(echo "$line_totals"                 | sed -E 's/.*percentage = ([0-9.]+).*/\1/')"
    fi

    # 4) real_ns_per_call line
    # e.g. "[SEMU LOG]: real_ns_per_call = 12.91589, diff_ns_per_call = 6.65436"
    line_realns="$(grep_single_line 'real_ns_per_call =')"
    if [[ -n "$line_realns" ]]; then
        real_ns_per_call="$(echo "$line_realns" | sed -E 's/.*real_ns_per_call = ([0-9.]+).*/\1/')"
        diff_ns_per_call="$(echo "$line_realns" | sed -E 's/.*diff_ns_per_call = ([0-9.]+).*/\1/')"
    fi

    # Print a single line with 11 columns in tab-delimited format
    echo -e "${N}\t${real_boot_time}\t${times_called}\t${ns_per_call}\t${predict_sec}\t${scale_factor}\t${test_total_clocksource_ns}\t${real_total_clocksource_ns}\t${percentage}\t${real_ns_per_call}\t${diff_ns_per_call}" >> "$OUTFILE"
done

echo "Data parsed and saved to $OUTFILE."
