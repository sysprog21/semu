#!/usr/bin/env bash
#
# parse_results.sh:
#   Parse "emulator_SMP_{N}.log" and "gprof_SMP_{N}.log" for each N=1..32
#   to extract:
#     - predict_sec
#     - real_boot_time
#     - times (calls)
#     - semu_timer_clocksource percentage
#     - ns_per_call
#     - real_ns_per_call (calculated)
#     - scale_factor
#
#   Output a summary line for each SMP=N in tab-delimited format.

LOGDIR="logs"   # or wherever your logs are
OUTFILE="results_summary.txt"

echo -e "SMP\treal_boot_time\ttimes\t\tns_per_call\tpredict_sec\tscale_factor\tpercentage\treal_ns_per_call" > "$OUTFILE"

for N in $(seq 1 32); do
    EMU_LOG="$LOGDIR/emulator_SMP_${N}.log"
    GPROF_LOG="$LOGDIR/gprof_SMP_${N}.log"

    # Check that the files exist
    if [[ ! -f "$EMU_LOG" ]]; then
        echo "Warning: $EMU_LOG not found, skipping"
        continue
    fi
    if [[ ! -f "$GPROF_LOG" ]]; then
        echo "Warning: $GPROF_LOG not found, skipping"
        continue
    fi

    # Initialize variables we want to parse
    predict_sec=""
    real_boot_time=""
    times_called=""
    ns_per_call=""
    scale_factor=""
    percentage=""
    real_ns_per_call=""

    # 1) Parse from emulator_SMP_{N}.log:
    #    We look for lines:
    #    "[SEMU LOG]: Boot time: 7.82979 seconds, called 233068043 times semu_timer_clocksource"
    #    next line: "ns_per_call = 5.10756, predict_sec = 10.21511, scale_factor = 0.97894"
    #    We can do it with grep + sed or awk or a while-read approach

    # Extract the "Boot time" line
    # e.g. "Boot time: 7.82979 seconds, called 233068043 times semu_timer_clocksource"
    # We'll parse the "7.82979" as real_boot_time, "233068043" as times_called
    line_boot_time="$(grep -Eo '\[SEMU LOG\]: Boot time: [^,]*, called [^ ]* times semu_timerclocksource.*' "$EMU_LOG")"
    # Alternatively you might want a simpler approach:
    # Let's do a grep for "Boot time:" and read the line with everything
    if [[ -z "$line_boot_time" ]]; then
        # Possibly there's an ANSI color code. Let's remove them with sed
        line_boot_time="$(grep 'Boot time:' "$EMU_LOG" | sed 's/\x1b\[[0-9;]*m//g')"
    fi

    # Now parse it. Example format:
    # Boot time: 7.82979 seconds, called 233068043 times semu_timer_clocksource
    # We can use sed or awk:
    if [[ -n "$line_boot_time" ]]; then
        # Remove ANSI codes again if needed:
        no_ansi="$(echo "$line_boot_time" | sed 's/\x1b\[[0-9;]*m//g')"
        # Extract e.g. 7.82979 and 233068043
        real_boot_time="$(echo "$no_ansi" | sed -E 's/.*Boot time: ([0-9.]+) seconds, called ([0-9]+) times.*/\1/')"
        times_called="$(echo "$no_ansi" | sed -E 's/.*Boot time: ([0-9.]+) seconds, called ([0-9]+) times.*/\2/')"
    fi

    # Next line with "ns_per_call = 5.10756, predict_sec = 10.21511, scale_factor = 0.97894"
    # We'll grep and parse similarly
    line_ns_call="$(grep -Eo 'ns_per_call = [^,]*, predict_sec = [^,]*, scale_factor = [^ ]*' "$EMU_LOG" | sed 's/\x1b\[[0-9;]*m//g')"
    if [[ -n "$line_ns_call" ]]; then
        # Example: "ns_per_call = 5.10756, predict_sec = 10.21511, scale_factor = 0.97894"
        ns_per_call="$(echo "$line_ns_call" | sed -E 's/.*ns_per_call = ([0-9.]+), predict_sec = ([0-9.]+), scale_factor = ([0-9.]+)/\1/')"
        predict_sec="$(echo "$line_ns_call" | sed -E 's/.*ns_per_call = ([0-9.]+), predict_sec = ([0-9.]+), scale_factor = ([0-9.]+)/\2/')"
        scale_factor="$(echo "$line_ns_call" | sed -E 's/.*ns_per_call = ([0-9.]+), predict_sec = ([0-9.]+), scale_factor = ([0-9.]+)/\3/')"
    fi

    # 2) From gprof_SMP_{N}.log:
    #    We search for the line that includes "semu_timer_clocksource" in the "Flat profile:" section
    #    Example: "47.74  3.07  3.07   ... semu_timer_clocksource"
    #    We'll parse the first field as the percentage
    # We'll do something like:
    line_gprof="$(grep -E '[0-9]+\.[0-9]+[[:space:]]+[0-9]+\.[0-9]+[[:space:]]+[0-9]+\.[0-9]+.*semu_timer_clocksource' "$GPROF_LOG")"
    # fallback if the spacing is different, or just search for 'semu_timer_clocksource'
    if [[ -z "$line_gprof" ]]; then
        line_gprof="$(grep 'semu_timer_clocksource' "$GPROF_LOG")"
    fi

    # e.g.: " 47.74   3.07     3.07     semu_timer_clocksource"
    # The first column is 47.74. We'll parse that:
    if [[ -n "$line_gprof" ]]; then
        # remove leading spaces
        no_space="$(echo "$line_gprof" | sed 's/^[[:space:]]*//')"
        # e.g. "47.74   3.07  ...
        # let's cut by spaces
        percentage="$(echo "$no_space" | awk '{print $1}')"
    fi

    # 3) Compute real_ns_per_call
    #    real_ns_per_call = real_boot_time * (percentage/100) * 1e9 / times_called
    # Check if the fields are not empty
    if [[ -n "$real_boot_time" && -n "$percentage" && -n "$times_called" ]]; then
        real_ns_per_call="$(awk -v rbt="$real_boot_time" -v pct="$percentage" -v calls="$times_called" '
            BEGIN {
                # real_boot_time * (percentage/100) * 1e9 / times
                if (calls == 0) {
                    print 0
                } else {
                    val = rbt * (pct/100.0) * 1e9 / calls
                    printf("%.5f", val)
                }
            }')"
    fi

    # 4) Print in tab-delimited format
    echo -e "${N}\t${real_boot_time}\t${times_called}\t${ns_per_call}\t${predict_sec}\t${scale_factor}\t${percentage}\t${real_ns_per_call}" >> "$OUTFILE"

done

echo "Data parsed and saved to $OUTFILE."
