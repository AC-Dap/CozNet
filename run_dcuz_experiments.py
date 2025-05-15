import argparse
import pandas as pd
import os
import subprocess
from subprocess import DEVNULL

def get_all_mappings(mappings_folder):
    all_mappings = []
    for f in os.listdir(mappings_folder):
        mapping = pd.read_csv(os.path.join(mappings_folder, f), names=['module', 'source', 'line', 'offset'])
        all_mappings.append(mapping)
    return pd.concat(all_mappings)

def run_experiment(script, script_args, module, offset, speedup):
    env = dict(os.environ)
    env['LD_PRELOAD'] = './dcuz.so'
    env['DCUZ_MODULE'] = module
    env['DCUZ_OFFSET'] = offset
    env['DCUZ_SPEEDUP'] = str(speedup)

    process = subprocess.Popen([script, *script_args], stdout=DEVNULL, stderr=DEVNULL, env=env)
    pid = process.pid
    process.wait()

    with open(f"{pid}.txt", 'r') as f:
        lines = f.readlines()
        virtual_time = int(lines[-1]) - int(lines[-2])

    return virtual_time


if __name__ == "__main__":
    parser = argparse.ArgumentParser(prog='DCuz')
    parser.add_argument('-m', '--mappings', help="Folder of source code mappings to experiment with")
    parser.add_argument('--min_experiments', default=5, type=int)
    parser.add_argument('-o', '--output', default="results.csv", help="Output CSV File")
    parser.add_argument("script", help="The script to run")
    parser.add_argument("script_args", nargs="*", help="Script arguments", default=[])

    args = parser.parse_args()

    all_mappings = get_all_mappings(args.mappings)
    print(all_mappings.head())

    speedups = [0.2, 0.4, 0.6, 0.8, 1]
    experiment_results = []
    i = 0
    for index, rows in all_mappings.iterrows():
        for speedup in speedups:
            result = run_experiment(args.script, args.script_args, rows['module'], rows['offset'], speedup)
            experiment_results.append({
                'line': f"{rows['source']}:{rows['line']}",
                'speedup': speedup,
                'result': result
            })
            print(f"{rows['source']}:{rows['line']} with {speedup * 100}% speedup had {result}")

        # Occasionally save progress
        i += 1
        if i % 100 == 0:
            pd.DataFrame(experiment_results).to_csv(args.output, index=False)
    baseline = run_experiment(args.script, args.script_args,
                              all_mappings.loc[0, 'module'], all_mappings.loc[0, 'offset'], 0)
    experiment_results.append({
        'line': "baseline",
        'speedup': 0,
        'result': baseline
    })

    pd.DataFrame(experiment_results).to_csv(args.output, index=False)
