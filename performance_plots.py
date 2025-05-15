import matplotlib.pyplot as plt
import numpy as np

baseline_times = [2.022, 2.021, 2.021, 2.021, 2.019, 2.020, 2.025, 2.020, 2.022, 2.022]
fast_baseline_times = [1.41, 1.42, 1.42, 1.41, 1.41, 1.41, 1.41, 1.41, 1.42, 1.42]
socket_times = [1.92, 1.93, 1.92, 1.94, 1.93, 1.94, 1.92, 1.92, 1.93, 1.93]
dcuz_times = [1.93, 1.92, 1.94, 1.92, 1.92, 1.94, 1.92, 1.93, 1.94, 1.94]

all_data = [baseline_times, fast_baseline_times, socket_times, dcuz_times]

plt.violinplot(all_data, showmeans=False, showmedians=True)
plt.title("Benchmark runtimes with and without CozNet")
plt.ylabel("Runtime (s)")
plt.xticks([1, 2, 3, 4], labels=["Baseline", "Improved Baseline", "Sockets Only", "Everything"])
plt.savefig("performance")

fast_mean = np.mean(fast_baseline_times)
socket_mean = np.mean(socket_times)
dcuz_mean = np.mean(dcuz_times)
print("Socket Only Overhead:", (socket_mean - fast_mean) / fast_mean)
print("DCuz Overhead:", (dcuz_mean - fast_mean) / fast_mean)

print("DCuz min overhead:", (np.min(dcuz_times) - np.max(fast_baseline_times)) / np.max(fast_baseline_times))
print("DCuz max overhead:", (np.max(dcuz_times) - np.min(fast_baseline_times)) / np.min(fast_baseline_times))