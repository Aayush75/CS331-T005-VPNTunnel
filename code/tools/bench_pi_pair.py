#!/usr/bin/env python3
"""Coordinate matched physical Pi-to-Pi direct/VPN benchmarks over Wi-Fi SSH.

No credentials are persisted. Requires prepared Pis, existing SSH control sockets,
and sudo authentication. CPU samples stay on each Pi until measurements finish.
"""
import argparse
from datetime import datetime,timezone
import getpass
import json
from pathlib import Path
import shlex
import subprocess
import time


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pi-a',default='10.7.45.18');p.add_argument('--pi-b',default='10.7.16.136')
    p.add_argument('--socket-a',default='/tmp/vpt-pi-a-wifi.sock');p.add_argument('--socket-b',default='/tmp/vpt-pi-b-wifi.sock')
    p.add_argument('--output',type=Path,required=True);p.add_argument('--runs',type=int,default=5);p.add_argument('--seconds',type=int,default=20);p.add_argument('--udp-seconds',type=int,default=10)
    args=p.parse_args();password=getpass.getpass('Pi sudo password: ')
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    stamp='pi-pair-'+str(int(time.time()));base='/home/cn/results/'+stamp
    agent='/home/cn/CS331-T005-VPNTunnel/code/tools/pi_bench_agent.py'
    hosts={'a':args.pi_a,'b':args.pi_b};sockets={'a':args.socket_a,'b':args.socket_b}
    commands=[];results=[];active=[]
    def save(name,data):(out/name).write_text(json.dumps(data,indent=2)+'\n')
    def ssh(side,command,root=True,timeout=90):
        actual='sudo -S -p "" bash -c '+shlex.quote(command) if root else command
        start=datetime.now(timezone.utc).isoformat()
        r=subprocess.run(['ssh','-S',sockets[side],'-o','ConnectTimeout=6',f'cn@{hosts[side]}',actual],input=password+'\n' if root else None,capture_output=True,text=True,timeout=timeout)
        commands.append(dict(side=side,command=command,root=root,start_utc=start,returncode=r.returncode,stdout=r.stdout,stderr=r.stderr));save('commands.json',commands)
        if r.returncode:raise RuntimeError(f'Pi {side} failed: {command}: {r.stderr} {r.stdout}')
        return r.stdout
    def worker(side,action,c):return ssh(side,'python3 '+shlex.quote(agent)+' '+action+' '+shlex.quote(json.dumps(c)))
    save('parameters.json',dict(pi_a=args.pi_a,pi_b=args.pi_b,remote_root=base,runs=args.runs,tcp_seconds=args.seconds,omitted_warmup_seconds=2,udp_seconds=args.udp_seconds,udp_rates_mbit=[500,800,950],udp_payload_bytes=1200,link='physical Ethernet, 1000 Mbit/s full duplex',offloads='unchanged defaults; recorded per mode',order=['direct','tunnel'],cpu_sample_seconds=.5,telemetry_seconds=.5,throttling_poll_seconds=5,clock_ticks_source='each Pi sysconf SC_CLK_TCK',management='Wi-Fi SSH, no bulk evidence downloads during measurements'))
    try:
        for mode in ['direct','tunnel']:
            print('===',mode,'===',flush=True)
            folders={s:base+'/'+mode+'/pi-'+s for s in 'ab'}
            active=[]
            for side in 'ab':
                folder=folders[side]
                ssh(side,'mkdir -p '+shlex.quote(folder))
                cmd='nohup python3 '+shlex.quote(agent)+' monitor '+shlex.quote(json.dumps(dict(out=folder)))+' > '+shlex.quote(folder+'/monitor.log')+' 2>&1 < /dev/null &'
                ssh(side,cmd);active.append((side,folder))
            if mode=='direct':
                for side in 'ab':ssh(side,'vpt-bench down')
                for side,src,dst in [('a','10.77.1.10','10.77.2.10'),('b','10.77.2.10','10.77.1.10')]:
                    data=json.loads(worker(side,'ping',dict(out=folders[side],source=src,destination=dst,count=1,name='vpn-off-before.json')))
                    if data['returncode']==0:raise RuntimeError('Inner endpoint reachable with VPN off')
            else:
                ssh('b','vpt-bench up');ssh('a','vpt-bench up')
                time.sleep(2)
            source='172.31.250.11' if mode=='direct' else '10.77.1.10'
            destination='172.31.250.12' if mode=='direct' else '10.77.2.10'
            for side in 'ab':
                worker(side,'metadata',dict(out=folders[side]))
                worker(side,'idle',dict(out=folders[side],seconds=8))
            worker('a','ping',dict(out=folders['a'],source=source,destination=destination,count=50))
            worker('b','ping',dict(out=folders['b'],source=destination,destination=source,count=50))
            worker('b','server-start',dict(out=folders['b'],bind=destination))
            try:
                for reverse in [False,True]:
                    for n in range(1,args.runs+1):
                        label=f'tcp-{"reverse" if reverse else "forward"}-{n}'
                        c=dict(out=folders['a'],label=label,source=source,destination=destination,protocol='TCP',seconds=args.seconds,reverse=reverse)
                        r=json.loads(worker('a','client',c));r['mode']=mode;results.append(r);save('results.json',results)
                        print(mode,label,f"{r['received_mbps']:.2f} Mbit/s",r['telemetry'],r['throttling'],flush=True)
                        if r['throttling']!='throttled=0x0':raise RuntimeError('Throttling flagged; evidence retained')
                for rate in [500,800,950]:
                    for reverse in [False,True]:
                        label=f'udp-{rate}-{"reverse" if reverse else "forward"}'
                        c=dict(out=folders['a'],label=label,source=source,destination=destination,protocol='UDP',seconds=args.udp_seconds,rate_mbit=rate,reverse=reverse)
                        r=json.loads(worker('a','client',c));r['mode']=mode;results.append(r);save('results.json',results)
                        print(mode,label,f"sent {r['sent_mbps']:.2f}, received {r['received_mbps']:.2f} Mbit/s, loss {r['loss_pct']}",flush=True)
                        if r['throttling']!='throttled=0x0':raise RuntimeError('Throttling flagged; evidence retained')
            finally:worker('b','server-stop',dict(out=folders['b']))
            if mode=='tunnel':
                for side in 'ab':
                    ssh(side,'vpt-bench down; cp /run/vpt-benchmark/controller.log '+shlex.quote(folders[side]+'/controller.log'))
                for side,src,dst in [('a',source,destination),('b',destination,source)]:
                    data=json.loads(worker(side,'ping',dict(out=folders[side],source=src,destination=dst,count=1,name='vpn-off-after.json')))
                    if data['returncode']==0:raise RuntimeError('Inner endpoint reachable after VPN down')
            for side,folder in active:worker(side,'finish',dict(out=folder))
            active=[]
        save('completion.json',dict(status='PASS',transfers=len(results),finished_utc=datetime.now(timezone.utc).isoformat()))
    finally:
        for side in 'ab':
            try:
                ssh(side,'vpt-bench down; if [ -d /sys/module/vpt1_foundation ]; then rmmod vpt1_foundation; fi; dmesg | tail -20; vcgencmd get_throttled; ps -eo comm | grep -E "^(vptctl|iperf3)$" || true')
            except Exception as e:print('Cleanup issue',side,str(e),flush=True)
        for side,folder in active:
            try:worker(side,'server-stop',dict(out=folder));worker(side,'finish',dict(out=folder))
            except Exception as e:print('Monitor cleanup issue',side,str(e),flush=True)
    print('MEASUREMENTS COMPLETE; remote root:',base,flush=True)

if __name__=='__main__':main()
