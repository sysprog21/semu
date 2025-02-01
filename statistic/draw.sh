#!/usr/bin/env bash
#
# plot_results.sh
#
# Generates 7 separate .png plots using gnuplot. Each plot corresponds
# to a particular data column (2..8) from the results_summary_{1..5}.txt files.
# The x-axis is SMP (column 1), and the y-axis is the chosen data column.
#
# The files are assumed to be:
#   results_summary_1.txt
#   results_summary_2.txt
#   results_summary_3.txt
#   results_summary_4.txt
#   results_summary_5.txt
#
# Each file has at least these columns:
#   1) SMP
#   2) real_boot_time
#   3) times
#   4) ns_per_call
#   5) predict_sec
#   6) scale_factor
#   7) percentage
#   8) real_ns_per_call
#
# Usage:
#   ./plot_results.sh
# It will produce 7 PNG images, e.g. real_boot_time.png, times.png, etc.

# A small map from column index to a more descriptive name (for file titles).
declare -A COLTITLE=(
  [2]="real_boot_time"
  [3]="times"
  [4]="ns_per_call"
  [5]="predict_sec"
  [6]="scale_factor"
  [7]="percentage"
  [8]="real_ns_per_call"
)

# Check if gnuplot is installed
command -v gnuplot >/dev/null 2>&1 || {
  echo "Error: gnuplot is not installed or not in PATH."
  exit 1
}

# Loop over the data columns we want to plot
for col in 2 3 4 5 6 7 8; do
  title="${COLTITLE[$col]}"

  echo "Generating plot for column $col -> $title"

  gnuplot -persist <<EOF
  set title "$title vs SMP"
  set xlabel "SMP"
  set ylabel "$title"
  set key left top
  set grid

  # Output a PNG figure sized 800x600
  set term pngcairo size 800,600
  set output "${title}.png"

  # Skip the header row with 'skip=1'
  # We assume each file has a header line, so 'using 1:$col' means:
  #   x = column 1 (SMP)
  #   y = column $col (the data we want)
  #
  # Each line is for a different environment's file.
  plot \
    "results_summary_1.txt" using 1:$col skip 1 with linespoints title "Env1", \
    "results_summary_2.txt" using 1:$col skip 1 with linespoints title "Env2", \
    "results_summary_3.txt" using 1:$col skip 1 with linespoints title "Env3", \
    "results_summary_4.txt" using 1:$col skip 1 with linespoints title "Env4", \
    "results_summary_5.txt" using 1:$col skip 1 with linespoints title "Env5"

EOF

done

echo "All plots generated! Look for *.png in the current directory."
