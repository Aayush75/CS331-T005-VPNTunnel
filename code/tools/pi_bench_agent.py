#!/usr/bin/env python3
"""Remote worker for bench_pi_pair.py. All files stay in the supplied evidence root."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cpu_monitor import snapshot


def now():
    return dict(epoch=time.time(),utc=datetime.now(timezone.utc).isoformat(),monotonic=time.monotonic())


def save(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(data,indent=2)+'\n')


def call(argv,timeout=30):
    r=subprocess.run(argv,capture_output=True,text=True,timeout=timeout)
    return dict(argv=argv,returncode=r.returncode,stdout=r.stdout,stderr=r.stderr)


def telemetry():
    data={}
    for name,path in [('temperature_millic','/sys/class/thermal/thermal_zone0/temp'),('frequency_khz','/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq'),('governor','/sys/devices/system/cpu/cpufreq/policy0/scaling_governor')]:
        try:data[name]=Path(path).read_text().strip()
        except OSError:data[name]=None
    return data


def monitor(out):
    out.mkdir(parents=True,exist_ok=True)
    stop=threading.Event()
    signal.signal(signal.SIGTERM,lambda *_:stop.set())
    signal.signal(signal.SIGINT,lambda *_:stop.set())
    flags='unknown';last_flags=0
    with (out/'cpu.jsonl').open('w',buffering=1) as f:
        while True:
            sample=snapshot();sample['telemetry']=telemetry()
            if time.monotonic()-last_flags>=5:
                flags=call(['vcgencmd','get_throttled'])['stdout'].strip();last_flags=time.monotonic()
            sample['telemetry']['throttling']=flags
            f.write(json.dumps(sample)+'\n')
            if stop.wait(.5) or (out/'stop-monitor').exists():break
        sample=snapshot();sample['telemetry']=telemetry();sample['telemetry']['throttling']=flags
        f.write(json.dumps(sample)+'\n')
    save(out/'monitor-finished.json',dict(clock_ticks_per_second=os.sysconf('SC_CLK_TCK'),finished=now()))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=['monitor','metadata','idle','ping','server-start','server-stop','client','finish'])
    p.add_argument('config',help='JSON configuration')
    args=p.parse_args();c=json.loads(args.config);out=Path(c['out']);out.mkdir(parents=True,exist_ok=True)
    if args.action=='monitor':monitor(out);return
    if args.action=='metadata':
        cmds=[['uname','-a'],['lscpu'],['ip','-j','-d','link'],['ip','-j','address'],['ip','-j','route'],['ethtool','eth0'],['ethtool','-k','eth0'],['ethtool','-i','eth0'],['timedatectl','show','-p','NTPSynchronized'],['vcgencmd','measure_temp'],['vcgencmd','get_throttled'],['vpt-bench','ready'],['ss','-lnptu']]
        data=dict(captured=now(),model=Path('/proc/device-tree/model').read_text().rstrip('\x00'),commands=[call(x) for x in cmds],telemetry=telemetry(),clock_ticks_per_second=os.sysconf('SC_CLK_TCK'))
        data['sha256']={str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in [Path('/usr/local/bin/vptctl'),Path('/usr/local/lib/vpt1/vpt1_foundation.ko')]}
        save(out/'metadata.json',data);print(json.dumps(dict(status='OK',telemetry=data['telemetry'])));return
    if args.action=='idle':
        start=now();time.sleep(c.get('seconds',8));save(out/'idle-window.json',dict(start=start,end=now()));print('IDLE_RECORDED');return
    if args.action=='ping':
        argv=['ping','-n','-D','-I',c['source'],'-c',str(c.get('count',50)),'-i',str(c.get('interval',.1)),'-W','1',c['destination']]
        result=call(argv,timeout=c.get('count',50)*c.get('interval',.1)+10);save(out/c.get('name','ping-idle.json'),result)
        print(json.dumps(dict(returncode=result['returncode'],stdout=result['stdout'])));return
    if args.action=='server-start':
        log=(out/'server.log').open('w')
        proc=subprocess.Popen(['iperf3','-s','-B',c['bind']],stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        save(out/'server.json',dict(pid=proc.pid,bind=c['bind'],started=now()))
        time.sleep(.4)
        if proc.poll() is not None:raise RuntimeError('iperf server failed: '+(out/'server.log').read_text())
        print('SERVER_STARTED');return
    if args.action=='server-stop':
        info=out/'server.json'
        if info.exists():
            pid=json.loads(info.read_text())['pid']
            try:
                if Path(f'/proc/{pid}/comm').read_text().strip()=='iperf3':os.kill(pid,signal.SIGTERM)
            except (OSError,ProcessLookupError):pass
        print('SERVER_STOPPED');return
    if args.action=='client':
        dest=out/c['label'];dest.mkdir(parents=True,exist_ok=False)
        argv=['iperf3','-c',c['destination'],'-B',c['source'],'-t',str(c['seconds']),'-J','--get-server-output']
        if c['protocol']=='TCP':argv+=['-O','2']
        else:argv+=['-u','-b',str(c['rate_mbit'])+'M','-l','1200']
        if c.get('reverse'):argv+=['-R']
        ping=None;handle=None
        before=now()
        try:
            if c['protocol']=='TCP':
                handle=(dest/'ping-loaded.txt').open('w')
                ping=subprocess.Popen(['ping','-n','-D','-I',c['source'],'-i','.1','-W','1',c['destination']],stdout=handle,stderr=subprocess.STDOUT)
            r=subprocess.run(argv,capture_output=True,text=True,timeout=c['seconds']+30)
            after=now();(dest/'iperf.json').write_text(r.stdout)
            command=dict(argv=argv,returncode=r.returncode,stderr=r.stderr,start=before,end=after,parameters=c)
            save(dest/'command.json',command)
            if r.returncode:raise RuntimeError('iperf failed: '+r.stderr+r.stdout)
            data=json.loads(r.stdout)
            if data.get('error'):raise RuntimeError(data['error'])
            recv=data.get('end',{}).get('sum_received')
            if not recv:raise RuntimeError('No receiver result')
            summary=dict(label=c['label'],protocol=c['protocol'],reverse=c.get('reverse',False),rate_mbit=c.get('rate_mbit'),received_mbps=recv['bits_per_second']/1e6,sent_mbps=data['end'].get('sum_sent',{}).get('bits_per_second',0)/1e6,received_bytes=recv['bytes'],loss_pct=recv.get('lost_percent') if recv.get('packets',0) else None,received_packets=recv.get('packets',0)-recv.get('lost_packets',0),retransmits=data['end'].get('sum_sent',{}).get('retransmits'),start=before,end=after,telemetry=telemetry(),throttling=call(['vcgencmd','get_throttled'])['stdout'].strip())
            save(dest/'summary.json',summary);print(json.dumps(summary))
        finally:
            if ping:
                ping.send_signal(signal.SIGINT)
                try:ping.wait(timeout=3)
                except subprocess.TimeoutExpired:ping.kill();ping.wait()
                handle.close()
        return
    if args.action=='finish':
        (out/'stop-monitor').touch()
        for _ in range(30):
            if (out/'monitor-finished.json').exists():break
            time.sleep(.2)
        if not (out/'monitor-finished.json').exists():raise RuntimeError('Monitor did not finish')
        for root,dirs,files in os.walk(out):
            os.chown(root,1000,1000)
            for f in files:os.chown(Path(root)/f,1000,1000)
        print('EVIDENCE_SAVED')

if __name__=='__main__':main()
