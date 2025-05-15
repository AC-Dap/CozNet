import argparse
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

if __name__ == "__main__":
    parser = argparse.ArgumentParser(prog='DCuz')
    parser.add_argument('results', default="results.csv", help="Results CSV File")
    parser.add_argument('-n', '--top_n', default=10, help="How many of the top lines to plot", type=int)
    parser.add_argument('-o', '--output', default="top_optimizations.png", help="Path of output plots")

    args = parser.parse_args()

    df = pd.read_csv(args.results)

    print("Read CSV:")
    print(df.head())

    # Group by unique line
    lines = df['line'].unique()

    # Get baseline comparison
    baseline = df.loc[df['line'] == "baseline", "result"].iloc[0]
    print("Baseline:", baseline)

    # For each line, find the largest diff (pos or neg) compared to "baseline"
    diffs = []
    for line in lines:
        results = df[df['line'] == line]
        diff = np.max(np.abs(results["result"] - baseline))
        diffs.append((diff, line))

    # Sort lines by largest diff
    diffs = sorted(diffs, reverse=True)

    # Plot lines in order
    def calculate_rows_cols(n):
        """Calculates the optimal number of rows and columns for n subplots."""
        cols = np.floor(np.sqrt(n))
        rows = np.ceil(n / cols)
        return int(rows), int(cols)

    r, c = calculate_rows_cols(args.top_n)
    fig, axes = plt.subplots(r, c, figsize=(15, 5 * r))
    axes = axes.flatten()

    for i in range(args.top_n):
        _, line = diffs[i]
        results = df[df['line'] == line].copy() # Use .copy() to avoid SettingWithCopyWarning

        # Sort data points by 'speedup' before plotting
        results.sort_values(by='speedup', inplace=True)

        xs = results['speedup']
        ys = results['result']

        ax = axes[i]
        ax.plot(xs, ys, marker='o', linestyle='-')
        ax.axhline(baseline, color='r', linestyle='--', linewidth=1)
        ax.set_title(line)
        ax.set_xlabel('Virtual Speedup')
        ax.set_ylabel('Runtime')

    # Hide any remaining unused subplots
    for j in range(args.top_n, len(axes)):
        fig.delaxes(axes[j])

    plt.tight_layout()
    plt.savefig(args.output)
    print("Output saved to", args.output)