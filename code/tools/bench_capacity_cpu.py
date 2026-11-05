#!/usr/bin/env python3
"""Native namespace CPU and matched underlay-capacity benchmark. Requires root.

Example: sudo VPTCTL=/absolute/path/vptctl python3 tools/bench_capacity_cpu.py --output results/cpu-capacity
Only experiment namespaces are configured. Original unrestricted results are untouched.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import tempfile
import threading
import time
from datetime import datetime, timezone
import bench_netns as B

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


class Experiment:
    def __init__(self, args):
        self.args = args
        self.out = args.output.resolve()
        self.out.mkdir(parents=True, exist_ok=args.resume)
        self.records = json.loads((self.out/'results.json').read_text()) if args.resume and (self.out/'results.json').exists() else []
        self.commands = json.loads((self.out/'commands.json').read_text()) if args.resume and (self.out/'commands.json').exists() else []
        self.done = {r['label']:r for r in self.records}
        self.gateways = []
        self.configs = {}
        self.tmp = None
        self.setup_attempted = False

    def call(self, argv, check=True):
        r = subprocess.run(list(map(str,argv)), capture_output=True, text=True, timeout=90)
        self.commands.append(dict(argv=list(map(str,argv)), returncode=r.returncode,
                                  stdout=r.stdout, stderr=r.stderr))
        dump(self.out/'commands.json', self.commands)
        if check and r.returncode:
            raise RuntimeError(f'Command failed: {argv}: {r.stderr} {r.stdout}')
        return r

    def ns(self, ns, *argv, check=True):
        return self.call(['ip','netns','exec',f'vpt_{B.D.RUN}_{ns}',*argv],check=check)

    def setup(self):
        self.call(['ip','netns','list'])
        self.setup_attempted = True
        self.call(['bash',Path(B.NETNS)/'n1_fixture.sh','setup',B.D.RUN])
        B.host_addrs()
        self.tmp = tempfile.TemporaryDirectory(prefix='vpt_cpu_capacity_')
        keys, pubs = {}, {}
        for side in ('a','b'):
            keys[side] = str(Path(self.tmp.name)/(side+'.key'))
            # Private-key generation output is intentionally not logged.
            r = subprocess.run([B.VPTCTL,'genkey','--out',keys[side]],capture_output=True,text=True,check=True)
            pubs[side] = re.search(r'public_key\s+([0-9a-f]{64})',r.stdout).group(1)
        for side, other in [('a','b'),('b','a')]:
            self.configs[side] = B.make_config(f'demo-pi-{side}.toml',side,self.tmp.name,keys[side],pubs[other])
        self.gateways = [B.Pi('gwb',self.configs['b'],self.tmp.name),B.Pi('gwa',self.configs['a'],self.tmp.name)]

    def mode(self, mode):
        for g in self.gateways:
            g.script('down')
        if mode == 'tunnel':
            for ns, prefix in [('gwa','10.77.2.0/24'),('gwb','10.77.1.0/24')]:
                self.ns(ns,'ip','route','del',prefix,check=False)
            for g in self.gateways:
                r = g.script('up',*(['--wait','20'] if g.site=='gwa' else []))
                if r.returncode:
                    raise RuntimeError(f'Gateway start: {r.stdout} {r.stderr}')
        else:
            B.baseline_setup()
        r = self.ns('ha','ping','-c','2','-W','2',B.HB)
        if r.returncode:
            raise RuntimeError('Path unreachable')

    def metadata(self, label):
        for ns,devices in [('ha',['veth_ha']),('hb',['veth_hb']),('gwa',['veth_gwa_lan','veth_gwa_out','vpn0']),('gwb',['veth_gwb_lan','veth_gwb_out','vpn0']),('und',['veth_und_a','veth_und_b'])]:
            self.ns(ns,'ip','-j','-d','link','show',check=False)
            for dev in devices:
                r = self.ns(ns,'ethtool','-k',dev,check=False)
                dump(self.out/'metadata'/label/f'{ns}-{dev}.json',dict(returncode=r.returncode,stdout=r.stdout,stderr=r.stderr))
        self.qstats(self.out/'metadata'/label/'qdisc.json')

    def offloads(self):
        # Disable aggregation on every veth, including LAN/underlay bridge-side ports.
        for ns in ['ha','ha2','lana','gwa','und','gwb','lanb','hb','hb2']:
            links = json.loads(self.ns(ns,'ip','-j','-d','link','show').stdout)
            for link in links:
                if link.get('linkinfo',{}).get('info_kind') == 'veth':
                    self.ns(ns,'ethtool','-K',link['ifname'],'tso','off','gso','off','gro','off')

    def shape(self, capacity):
        for dev in ['veth_und_a','veth_und_b']:
            if capacity:
                # 1 ms burst, 20 ms queue bound. Include 24 bytes for Ethernet
                # preamble/IFG/FCS beyond the skb's Ethernet frame length.
                burst = max(16000, int(capacity*1e6/8*.001))
                self.ns('und','tc','qdisc','replace','dev',dev,'root','tbf',
                        'rate',f'{capacity}mbit','burst',str(burst),'latency','20ms',
                        'overhead','24','mpu','64')
            else:
                self.ns('und','tc','qdisc','del','dev',dev,'root',check=False)

    def qstats(self, path):
        data = {}
        for dev in ['veth_und_a','veth_und_b']:
            r = self.ns('und','tc','-s','-j','qdisc','show','dev',dev)
            data[dev] = json.loads(r.stdout)
        dump(path,data)

    def idle(self, label):
        folder = self.out/label
        if (folder/'cpu-idle-summary.json').exists():
            return
        folder.mkdir(parents=True,exist_ok=True)
        cpu = CPU().start()
        time.sleep(5)
        summary = cpu.finish(folder/'cpu-idle.json')
        dump(folder/'cpu-idle-summary.json',summary)
        self.ping(folder/'ping-idle.json',count=50)

    def ping(self,path,count=None):
        argv=['ip','netns','exec',f'vpt_{B.D.RUN}_ha','ping','-n','-D','-i','0.1','-W','1']
        if count:
            argv += ['-c',str(count)]
        argv += [B.HB]
        if count:
            r=subprocess.run(argv,capture_output=True,text=True,timeout=20)
            dump(path,dict(argv=argv,returncode=r.returncode,stdout=r.stdout,stderr=r.stderr))
        else:
            handle=path.with_suffix('.txt').open('w')
            p=subprocess.Popen(argv,stdout=handle,stderr=subprocess.STDOUT,text=True)
            return p,handle

    def transfer(self,label,protocol='TCP',reverse=False,rate=None,size=None,seconds=None):
        if label in self.done:
            return self.done[label]
        folder=self.out/label
        if folder.exists():
            old=folder.with_name(folder.name+'-incomplete-'+str(int(time.time())))
            folder.rename(old)
        folder.mkdir(parents=True,exist_ok=False)
        seconds=seconds or self.args.seconds
        self.qstats(folder/'qdisc-before.json')
        server_argv=['ip','netns','exec',f'vpt_{B.D.RUN}_hb','iperf3','-s','-1','-B',B.HB]
        server_log=(folder/'server.log').open('w')
        server=subprocess.Popen(server_argv,stdout=server_log,stderr=subprocess.STDOUT,text=True)
        time.sleep(.4)
        argv=['ip','netns','exec',f'vpt_{B.D.RUN}_ha','iperf3','-c',B.HB,'-B',B.HA,'-t',str(seconds),'-J','--get-server-output']
        if protocol=='UDP':
            argv+=['-u','-b',f'{rate}M','-l',str(size or 1200)]
        else:
            argv+=['-O','2']
        if reverse:
            argv+=['-R']
        ping=None
        cpu=CPU().start()
        started=time.monotonic()
        try:
            if protocol=='TCP':
                ping=self.ping(folder/'ping-loaded.json')
            r=subprocess.run(argv,capture_output=True,text=True,timeout=seconds+35)
            dump(folder/'command.json',dict(argv=argv,returncode=r.returncode,stderr=r.stderr,started_monotonic=started,ended_monotonic=time.monotonic()))
            (folder/'iperf.json').write_text(r.stdout)
            if r.returncode:
                raise RuntimeError(f'iperf failure {label}: {r.stderr} {r.stdout}')
            d=json.loads(r.stdout)
            if d.get('error'):
                raise RuntimeError(d['error'])
            recv=d.get('end',{}).get('sum_received')
            if recv is None:
                raise RuntimeError('Missing receiver result')
        finally:
            cpu_summary=cpu.finish(folder/'cpu.json',omit=2 if protocol=='TCP' else 0)
            dump(folder/'cpu-summary.json',cpu_summary)
            if ping:
                import signal
                ping[0].send_signal(signal.SIGINT)
                try:ping[0].wait(timeout=3)
                except subprocess.TimeoutExpired:ping[0].kill();ping[0].wait()
                ping[1].close()
            server.terminate()
            try:server.wait(timeout=3)
            except subprocess.TimeoutExpired:server.kill();server.wait()
            server_log.close()
            self.qstats(folder/'qdisc-after.json')
        summary=dict(label=label,protocol=protocol,reverse=reverse,rate_mbit=rate,payload_bytes=size,
            received_mbps=recv['bits_per_second']/1e6,sent_mbps=d['end'].get('sum_sent',{}).get('bits_per_second',0)/1e6,
            received_packets=recv.get('packets',0)-recv.get('lost_packets',0),
            loss_pct=recv.get('lost_percent') if recv.get('packets',0) else None,cpu=cpu_summary)
        dump(folder/'summary.json',summary)
        self.records.append(summary)
        self.done[label]=summary
        dump(self.out/'results.json',self.records)
        print(f"{label}: {summary['received_mbps']:.2f} Mbit/s; CPU {cpu_summary['busy_pct']:.2f}% total, {cpu_summary['busy_core_equivalents']:.2f} cores",flush=True)
        return summary

    def run(self):
        try:
            self.setup()
            # Unrestricted rerun: default offloads, matching previous experiments.
            for mode in ['tunnel','baseline']:
                expected=11
                completed=sum(r['label'].startswith('unrestricted/'+mode+'/') for r in self.records)
                sweep_complete=mode!='tunnel' or sum(r['label'].startswith('unrestricted/packet-sweep/') for r in self.records)==10
                if completed==expected and sweep_complete:
                    continue
                self.shape(None)
                self.mode(mode)
                self.metadata('unrestricted-'+mode)
                self.idle('unrestricted/'+mode)
                for reverse in [False,True]:
                    for i in range(1,self.args.runs+1):
                        self.transfer(f'unrestricted/{mode}/tcp-{"reverse" if reverse else "forward"}-{i}',reverse=reverse)
                self.transfer(f'unrestricted/{mode}/udp-100',protocol='UDP',rate=100,size=1376)
                if mode=='tunnel':
                    for size in [64,128,256,512,768,1024,1200,1300,1400,1428]:
                        self.transfer(f'unrestricted/packet-sweep/udp-{size}',protocol='UDP',rate=200,size=size,seconds=8)
            self.offloads()
            for index,capacity in enumerate(self.args.capacities):
                self.shape(capacity)
                # Alternate configuration order to reduce systematic order bias.
                for mode in (['baseline','tunnel'] if index%2==0 else ['tunnel','baseline']):
                    prefix=f'capacity-{capacity}/{mode}/'
                    if sum(r['label'].startswith(prefix) for r in self.records)==16:
                        continue
                    self.mode(mode)
                    self.metadata(f'capacity-{capacity}-{mode}')
                    label=f'capacity-{capacity}/{mode}'
                    print(f'=== {label} ===',flush=True)
                    self.idle(label)
                    for reverse in [False,True]:
                        for i in range(1,self.args.runs+1):
                            self.transfer(f'{label}/tcp-{"reverse" if reverse else "forward"}-{i}',reverse=reverse)
                    for fraction in [.5,.8,.95]:
                        for reverse in [False,True]:
                            self.transfer(f'{label}/udp-{fraction}-{"reverse" if reverse else "forward"}',protocol='UDP',reverse=reverse,rate=capacity*fraction,size=1200,seconds=8)
            dump(self.out/'completion.json',dict(status='PASS',transfers=len(self.records),finished_utc=datetime.now(timezone.utc).isoformat()))
        finally:
            for g in self.gateways:
                g.script('down')
                log=Path(g.env['VPT_RUN_DIR'])/'controller.log'
                if log.exists():shutil.copy2(log,self.out/f'controller-{g.site}.log')
            if self.setup_attempted:
                self.call(['bash',Path(B.NETNS)/'n1_fixture.sh','teardown',B.D.RUN],check=False)
            if self.tmp:self.tmp.cleanup()
            self.call(['ip','netns','list'],check=False)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--resume',action='store_true',help='Resume completed labels from results.json without overwriting measurements')
    p.add_argument('--runs',type=int,default=5)
    p.add_argument('--seconds',type=int,default=15)
    p.add_argument('--capacities',type=int,nargs='+',default=[100,500,1000,2500])
    args=p.parse_args()
    if os.geteuid()!=0:p.error('Root required')
    if not B.VPTCTL:p.error('Set VPTCTL')
    e=Experiment(args)
    dump(e.out/'parameters.json',dict(runs=args.runs,seconds=args.seconds,capacities=args.capacities,
        udp_seconds=8,udp_fractions=[.5,.8,.95],udp_payload=1200,cpu_sample_seconds=.5,
        qdisc='TBF on underlay bridge egress ports, 1ms burst (minimum 16KB), 20ms latency, overhead 24, mpu 64',
        controlled_offloads='TSO/GSO/GRO disabled on all namespace Ethernet interfaces',
        cpu_definition='Host-wide busy excludes idle and iowait; guest counters not double-counted. Includes generators, softirq, background activity. TCP CPU excludes first 2 seconds.'))
    e.run()

if __name__=='__main__':main()
