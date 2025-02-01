import os
import matplotlib.pyplot as plt

def parse_results_summary(summary_path):
    """
    Reads a results_summary_N.txt file and returns a dict:
       {smp_value: (percentage, predicted_ns_per_call, real_ns_per_call)}
    """
    smp_info = {}
    with open(summary_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            parts = line.split()
            # Skip the header line if it's present
            if parts[0] == "SMP":
                continue
            
            # Columns in summary file (1-based):
            # 1: SMP
            # 2: real_boot_time
            # 3: times_called
            # 4: ns_per_call        (predicted ns)
            # 5: predict_sec
            # 6: scale_factor
            # 7: test_total_clocksource_ns
            # 8: real_total_clocksource_ns
            # 9: percentage
            # 10: real_ns_per_call
            # 11: diff_ns_per_call
            #
            # Zero-based index references:
            # parts[0]  -> SMP
            # parts[3]  -> ns_per_call (predicted)
            # parts[8]  -> percentage
            # parts[9]  -> real_ns_per_call
            if len(parts) >= 10:
                try:
                    smp_val = int(parts[0])
                    predicted_ns_per_call = float(parts[3])
                    percentage = float(parts[8])
                    smp_info[smp_val] = (percentage, predicted_ns_per_call)
                except ValueError:
                    continue

    return smp_info

def parse_and_scale_time_log(time_log_path, scale_factor):
    scaled_values = []
    with open(time_log_path, "r") as f:
        for line in f:
            line = line.strip()
            if line.startswith("diff:"):
                # e.g. "diff: 50176382"
                parts = line.split(":")
                if len(parts) == 2:
                    try:
                        val_ns = float(parts[1].strip())     # raw ns
                        val_ms = val_ns / 1_000_000          
                        scaled_val = val_ms * scale_factor   # scale by 'percentage'
                        scaled_values.append(scaled_val)
                    except ValueError:
                        pass
    return scaled_values

def main():
    base_statistic_dir = "."  # Your base folder for logs & summaries
    num_summaries = 7                 # We have results_summary_1.txt ... results_summary_6.txt
    num_smp = 32                      # Each summary covers SMP=1..32
    
    for i in range(1, num_summaries + 1):
        summary_file = os.path.join(base_statistic_dir, f"results_summary-{i}.txt")
        time_log_dir = os.path.join(base_statistic_dir, f"time_log-{i}")  # e.g. statistic/time_log-1
        output_dir   = os.path.join(base_statistic_dir, f"plots-{i}")
        
        # Create output directory for plots
        os.makedirs(output_dir, exist_ok=True)
        
        # 1) Parse the summary file => { SMP: (percentage, predicted_ns, real_ns) }
        smp_data = parse_results_summary(summary_file)
        print(f"[INFO] Parsed {len(smp_data)} SMP entries from {summary_file}")

        # We'll store an average (of scaled diff) per SMP for a "trend" plot
        averages = []
        
        for smp_val in range(1, num_smp + 1):
            # Retrieve info for this SMP
            if smp_val not in smp_data:
                print(f"[WARNING] SMP={smp_val} not found in {summary_file}. Skipping.")
                averages.append(0.0)
                continue
            
            percentage, pred_ns_per_call = smp_data[smp_val]
            
            # 2) Parse & scale the time_log_{SMP}.txt
            time_log_path = os.path.join(time_log_dir, f"time_log_{smp_val}.txt")
            if not os.path.exists(time_log_path):
                print(f"[WARNING] File not found: {time_log_path}")
                averages.append(0.0)
                continue
            
            scaled_values = parse_and_scale_time_log(time_log_path, percentage)
            if not scaled_values:
                print(f"[WARNING] No valid data in {time_log_path}")
                averages.append(0.0)
                continue
            
            # 3) Plot scaled diff values (blue line)
            plt.figure(figsize=(8, 4))
            plt.plot(scaled_values, color='blue', marker=None, linewidth=1, linestyle='-',
                     label='real ns_per_call (average per 1e6 times)')
            
            # 4) Add horizontal lines for predicted (red) + real (green) ns_per_call
            plt.axhline(y=pred_ns_per_call, color='red', linestyle='--',
                        label='Predicted ns_per_call')
            
            plt.title(f"SMP={smp_val} --- real vs. predict ns_per_call\n"
                      f"(percentage={percentage}, pred={pred_ns_per_call:.3f}ns)")
            plt.xlabel("per 1e6 times called")
            plt.ylabel("Nanoseconds (ns)")
            plt.legend()
            
            # Save the figure for this SMP
            plot_path = os.path.join(output_dir, f"time_log_{smp_val}_scaled.png")
            plt.savefig(plot_path, dpi=150)
            plt.close()
            
            # Compute average of scaled diff
            avg_val = sum(scaled_values) / len(scaled_values)
            averages.append(avg_val)
        
        # 5) Plot the average trend (per SMP)
        smp_indices = list(range(1, num_smp + 1))
        plt.figure(figsize=(8, 4))
        plt.plot(smp_indices, averages, marker=None, linewidth=1)
        plt.title(f"Average scaled diff (ns) across SMP=1..{num_smp} (Set {i})")
        plt.xlabel("SMP #")
        plt.ylabel("Average scaled diff (ns)")
        
        avg_trend_path = os.path.join(output_dir, "scaled_averages_trend.png")
        plt.savefig(avg_trend_path, dpi=150)
        plt.close()
        
        print(f"[INFO] Finished set {i}, results in {output_dir}")

if __name__ == "__main__":
    main()
