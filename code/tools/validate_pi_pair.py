#!/usr/bin/env python3
"""Validate physical Pi experiment completeness and retained controls."""
import argparse
from collections import Counter
import json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('evidence',type=Path);args=p.parse_args();root=args.evidence
load=lambda p:json.loads(p.read_text())
records=load(root/'results.json');params=load(root/'parameters.json')
assert load(root/'completion.json')['status']=='PASS'
assert len(records)==32 and len({(r['mode'],r['label']) for r in records})==32
counts=Counter((r['mode'],r['protocol'],r['reverse']) for r in records)
cpus={(r['mode'],r['label']):r for r in load(root/'cpu-per-transfer.json')}
for mode in ['direct','tunnel']:
 for reverse in [False,True]:
  assert counts[(mode,'TCP',reverse)]==5
  assert counts[(mode,'UDP',reverse)]==3
 for side in 'ab':
  folder=root/'raw'/mode/f'pi-{side}';meta=load(folder/'metadata.json')
  assert meta['clock_ticks_per_second']>0
  cmds={tuple(c['argv']):c for c in meta['commands']}
  eth=cmds[('ethtool','eth0')]['stdout']
  assert 'Speed: 1000Mb/s' in eth and 'Duplex: Full' in eth and 'Link detected: yes' in eth
  assert cmds[('vpt-bench','ready')]['returncode']==0
  assert 'NTPSynchronized=yes' in cmds[('timedatectl','show','-p','NTPSynchronized')]['stdout']
  links=json.loads(cmds[('ip','-j','-d','link')]['stdout']);links={x['ifname']:x for x in links}
  assert links['eth0']['mtu']==1500
  if mode=='tunnel':assert links['vpn0']['mtu']==1428
  samples=[json.loads(s) for s in (folder/'cpu.jsonl').read_text().splitlines() if s]
  assert len(samples)>100
  assert all(len([k for k in s['cpus'] if k!='cpu'])==4 for s in samples)
  assert all(s['telemetry']['throttling']=='throttled=0x0' for s in samples),(mode,side,'throttling detected')
  assert (folder/'monitor-finished.json').exists()
  idle=load(folder/'ping-idle.json');assert idle['returncode']==0
for r in records:
 folder=root/'raw'/r['mode']/'pi-a'/r['label']
 command=load(folder/'command.json');data=load(folder/'iperf.json')
 assert command['returncode']==0 and not data.get('error')
 assert data['end']['sum_received']['bits_per_second']>0
 expected_a='172.31.250.11' if r['mode']=='direct' else '10.77.1.10'
 expected_b='172.31.250.12' if r['mode']=='direct' else '10.77.2.10'
 assert command['parameters']['source']==expected_a and command['parameters']['destination']==expected_b
 if r['protocol']=='TCP':assert '-O' in command['argv'] and (folder/'ping-loaded.txt').exists()
 else:assert command['argv'][command['argv'].index('-l')+1]=='1200'
 for side,c in cpus[(r['mode'],r['label'])]['cpu'].items():
  assert len(c['per_core'])==4 and 0<=c['busy_pct']<=100
  assert c['window_seconds']>=18 if r['protocol']=='TCP' else c['window_seconds']>=8
  assert c['throttling_flags']==['throttled=0x0']
for side in 'ab':
 direct=load(root/'raw/direct'/f'pi-{side}'/'metadata.json')
 tunnel=load(root/'raw/tunnel'/f'pi-{side}'/'metadata.json')
 features=lambda m:next(c['stdout'] for c in m['commands'] if c['argv']==['ethtool','-k','eth0'])
 assert features(direct)==features(tunnel),(side,'offload settings changed')
 assert load(root/'raw/direct'/f'pi-{side}'/'vpn-off-before.json')['returncode']!=0
 assert load(root/'raw/tunnel'/f'pi-{side}'/'vpn-off-after.json')['returncode']!=0
checks=dict(status='PASS',transfers=32,tcp=20,udp=12,checks=['five TCP runs per direction/path','three UDP rates per direction/path','receiver-side JSON and successful exits','CPU evidence on both four-core Pis for every transfer','gigabit full-duplex physical Ethernet','MTUs 1500 outer/1428 inner','unchanged Ethernet offload features','synchronized clocks','no recorded throttling','VPN-off reachability blocked before/after'])
(root/'validation.json').write_text(json.dumps(checks,indent=2)+'\n');print(json.dumps(checks,indent=2))
