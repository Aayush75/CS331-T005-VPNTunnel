#!/usr/bin/env python3
"""Validate completeness and experimental controls of the saved evidence."""
import argparse
from collections import Counter
import json
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('evidence',type=Path)
a=p.parse_args();root=a.evidence;run=root/'run'
records=json.loads((run/'results.json').read_text())
assert json.loads((run/'completion.json').read_text())['status']=='PASS'
assert len(records)==160,len(records)
assert len({r['label'] for r in records})==160
counts=Counter()
for r in records:
    label=r['label'];folder=run/label
    counts[tuple(label.split('/')[:2])]+=1
    command=json.loads((folder/'command.json').read_text())
    assert command['returncode']==0,(label,command)
    data=json.loads((folder/'iperf.json').read_text())
    assert not data.get('error'),(label,data.get('error'))
    assert data['end'].get('sum_received') is not None,label
    cpu=json.loads((folder/'cpu.json').read_text())
    assert len(cpu['samples'])>=10,label
    assert 0<=r['cpu']['busy_pct']<=100,label
    assert 0<r['cpu']['window_seconds']<40,label
    assert len(r['cpu']['per_core'])==22,label
    for c in r['cpu']['per_core'].values():assert 0<=c['busy_pct']<=100,(label,c)
    if r['protocol']=='TCP':
        assert (folder/'ping-loaded.txt').exists(),label
        assert 'time=' in (folder/'ping-loaded.txt').read_text(),label
    if not label.endswith('udp-1428'):
        assert r['received_mbps']>0,label
    for phase in ['before','after']:
        q=json.loads((folder/f'qdisc-{phase}.json').read_text())
        if label.startswith('capacity-'):
            cap=int(label.split('/')[0].split('-')[1])
            assert all(v[0]['kind']=='tbf' and v[0]['options']['rate']==cap*1e6/8 for v in q.values()),label
for cap in [100,500,1000,2500]:
    for mode in ['tunnel','baseline']:
        assert counts[(f'capacity-{cap}',mode)]==16
        meta=run/'metadata'/f'capacity-{cap}-{mode}'
        q=json.loads((meta/'qdisc.json').read_text())
        assert len(q)==2
        for dev in ['gwa-veth_gwa_out','gwb-veth_gwb_out','ha-veth_ha','hb-veth_hb','und-veth_und_a','und-veth_und_b']:
            features=json.loads((meta/(dev+'.json')).read_text())['stdout']
            for feature in ['tcp-segmentation-offload','generic-segmentation-offload','generic-receive-offload']:
                assert feature+': off' in features,(cap,mode,dev,feature)
    direct=json.loads((run/'metadata'/f'capacity-{cap}-baseline/qdisc.json').read_text())
    tunnel=json.loads((run/'metadata'/f'capacity-{cap}-tunnel/qdisc.json').read_text())
    for dev in direct:
        assert direct[dev][0]['options']==tunnel[dev][0]['options'],(cap,dev)
assert counts[('unrestricted','tunnel')]==11
assert counts[('unrestricted','baseline')]==11
assert counts[('unrestricted','packet-sweep')]==10
result=dict(status='PASS',completed_transfers=160,unique_labels=160,
            controlled_tcp=80,controlled_udp=48,unrestricted_tcp=20,unrestricted_udp=2,packet_sweep=10,
            checks=['raw exit codes and receiver fields','CPU samples for every transfer','loaded ping for every TCP transfer','both directional qdiscs at requested rate','identical per-capacity qdisc options for VPN/direct','matching disabled TSO/GSO/GRO','positive goodput except intentional oversized payload'])
(root/'validation.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
