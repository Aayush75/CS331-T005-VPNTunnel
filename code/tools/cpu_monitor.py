#!/usr/bin/env python3
"""Portable Linux system/per-core CPU monitor for native hosts and both Pi gateways.

python3 tools/cpu_monitor.py --output results/pi-a-cpu --seconds 300
CPU percentages include all host activity; record a separate idle baseline.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import threading
import time
from datetime import datetime, timezone
HZ = os.sysconf('SC_CLK_TCK')


def dump(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n')


def snapshot():
    cpus = {}
    for line in Path('/proc/stat').read_text().splitlines():
        parts = line.split()
        if parts and re.fullmatch(r'cpu\d*', parts[0]):
            cpus[parts[0]] = list(map(int, parts[1:9]))
    soft = {}
    for line in Path('/proc/softirqs').read_text().splitlines():
        if ':' in line:
            name, counts = line.split(':', 1)
            if name.strip() in ('NET_RX', 'NET_TX'):
                soft[name.strip()] = list(map(int, counts.split()))
    return dict(monotonic=time.monotonic(), utc=datetime.now(timezone.utc).isoformat(),
                cpus=cpus, softirqs=soft, loadavg=Path('/proc/loadavg').read_text().strip())


class CPU:
    def __init__(self):
        self.samples = []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.collect, daemon=True)

    def collect(self):
        while not self.stop.is_set():
            self.samples.append(snapshot())
            self.stop.wait(0.5)

    def start(self):
        self.thread.start()
        return self

    def finish(self, path, omit=0):
        self.stop.set()
        self.thread.join()
        self.samples.append(snapshot())
        dump(path, dict(clock_ticks_per_second=HZ, interval_seconds=0.5, samples=self.samples))
        eligible = [s for s in self.samples if s['monotonic'] >= self.samples[0]['monotonic'] + omit]
        if len(eligible) < 2:
            raise RuntimeError('Insufficient CPU samples')
        a, z = eligible[0], eligible[-1]
        elapsed = z['monotonic'] - a['monotonic']
        per = {}
        for name, initial in a['cpus'].items():
            delta = [v-u for u,v in zip(initial,z['cpus'][name])]
            total = sum(delta)
            busy = total-delta[3]-delta[4]
            per[name] = dict(busy_pct=100*busy/total if total else 0,
                user_cpu_seconds=(delta[0]+delta[1])/HZ,
                system_cpu_seconds=delta[2]/HZ, softirq_cpu_seconds=delta[6]/HZ,
                busy_cpu_seconds=busy/HZ, iowait_cpu_seconds=delta[4]/HZ,
                busy_core_equivalents=busy/HZ/elapsed)
        aggregate = per.pop('cpu')
        return dict(window_seconds=elapsed, omitted_start_seconds=omit, **aggregate,
                    busiest_core_pct=max(x['busy_pct'] for x in per.values()), per_core=per)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--seconds',type=float,required=True)
    args=parser.parse_args()
    if args.seconds <= 0:
        parser.error('--seconds must be positive')
    args.output.mkdir(parents=True,exist_ok=False)
    cpu=CPU().start()
    def finish_on_signal(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGINT, finish_on_signal)
    signal.signal(signal.SIGTERM, finish_on_signal)
    try:
        time.sleep(args.seconds)
    except KeyboardInterrupt:
        pass
    summary=cpu.finish(args.output/'cpu.json')
    dump(args.output/'summary.json',summary)
    print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
