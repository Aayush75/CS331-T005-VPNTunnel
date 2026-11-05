#!/usr/bin/env python3
"""Summarize completed native CPU/capacity measurements without discarding failures."""
import argparse
import csv
from datetime import datetime
import json
from pathlib import Path
import re
import statistics


def ping_stats(text, after_epoch=None):
    values=[]
    for line in text.splitlines():
        m=re.search(r'time[=<]([\d.]+)\s*ms',line)
        ts=re.search(r'\[([\d.]+)\]',line)
        if m and (after_epoch is None or ts and float(ts[1])>=after_epoch):
            values.append(float(m[1]))
    if not values:return dict(samples=0,mean_ms=None,p95_ms=None,max_ms=None)
    ordered=sorted(values)
    return dict(samples=len(values),mean_ms=statistics.mean(values),
                p95_ms=ordered[min(len(ordered)-1,int(.95*len(ordered)))],max_ms=max(values))


def aggregate(values):
    return dict(n=len(values),mean=statistics.mean(values),stdev=statistics.pstdev(values)) if values else None


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('evidence',type=Path)
    args=p.parse_args()
    root=args.evidence.resolve();run=root/'run'
    records=json.loads((run/'results.json').read_text())
    complete=(run/'completion.json').exists()
    rows=[]
    for rec in records:
        parts=rec['label'].split('/')
        cap=int(parts[0].split('-')[1]) if parts[0].startswith('capacity-') else None
        mode='tunnel' if parts[1]=='packet-sweep' else parts[1]
        folder=run/rec['label'];cpu=rec['cpu']
        endpoint_cpu=json.loads((folder/'iperf.json').read_text()).get('end',{}).get('cpu_utilization_percent',{})
        samples=json.loads((folder/'cpu.json').read_text())['samples']
        epoch=datetime.fromisoformat(samples[0]['utc']).timestamp()+cpu['omitted_start_seconds']
        eligible=[x for x in samples if x['monotonic']>=samples[0]['monotonic']+cpu['omitted_start_seconds']]
        peak_core=0
        for first,last in zip(eligible,eligible[1:]):
            for core,initial in first['cpus'].items():
                if core=='cpu':continue
                delta=[v-u for u,v in zip(initial,last['cpus'][core])]
                total=sum(delta)
                if total:peak_core=max(peak_core,100*(total-delta[3]-delta[4])/total)
        loaded_path=folder/'ping-loaded.txt'
        loaded=ping_stats(loaded_path.read_text(),epoch) if loaded_path.exists() else {}
        idle_folder=run/'unrestricted/tunnel' if parts[1]=='packet-sweep' else folder.parent
        idle_path=idle_folder/'ping-idle.json'
        idle=ping_stats(json.loads(idle_path.read_text())['stdout']) if idle_path.exists() else {}
        idle_cpu_path=idle_folder/'cpu-idle-summary.json'
        idle_cpu=json.loads(idle_cpu_path.read_text()) if idle_cpu_path.exists() else {}
        row=dict(label=rec['label'],capacity_mbit=cap,mode=mode,
            protocol=rec['protocol'],direction='reverse' if rec['reverse'] else 'forward',
            payload_bytes=rec.get('payload_bytes'),offered_mbit=rec.get('rate_mbit'),
            sent_mbps=rec['sent_mbps'],received_mbps=rec['received_mbps'],loss_pct=rec['loss_pct'],
            requested_udp_rate_achieved_pct=100*rec['sent_mbps']/rec['rate_mbit'] if rec.get('rate_mbit') else None,
            udp_status=('NO_RECEIVER_PACKETS' if rec['received_mbps']==0 else 'REQUESTED_RATE_NOT_REACHED' if rec.get('rate_mbit') and rec['sent_mbps']<.98*rec['rate_mbit'] else 'MEASURED') if rec['protocol']=='UDP' else None,
            host_cpu_busy_pct=cpu['busy_pct'],busy_core_equivalents=cpu['busy_core_equivalents'],
            busiest_core_busy_pct=cpu['busiest_core_pct'],peak_sample_core_busy_pct=peak_core,softirq_core_equivalents=cpu['softirq_cpu_seconds']/cpu['window_seconds'],
            cpu_seconds_per_received_gbit=cpu['busy_core_equivalents']/(rec['received_mbps']/1000) if rec['received_mbps'] else None,
            idle_host_cpu_busy_pct=idle_cpu.get('busy_pct'),idle_busy_core_equivalents=idle_cpu.get('busy_core_equivalents'),
            idle_rtt_mean_ms=idle.get('mean_ms'),loaded_rtt_mean_ms=loaded.get('mean_ms'),loaded_rtt_p95_ms=loaded.get('p95_ms'),loaded_ping_samples=loaded.get('samples'),
            cpu_window_seconds=cpu['window_seconds'],
            iperf_client_cpu_pct_one_core_scale=endpoint_cpu.get('host_total'),
            iperf_client_user_cpu_pct_one_core_scale=endpoint_cpu.get('host_user'),
            iperf_client_system_cpu_pct_one_core_scale=endpoint_cpu.get('host_system'),
            iperf_server_cpu_pct_one_core_scale=endpoint_cpu.get('remote_total'))
        rows.append(row)
    with (root/'all-transfers.csv').open('w') as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    grouped=[]
    for cap in [None,100,500,1000,2500]:
        for mode in ['baseline','tunnel']:
            for direction in ['forward','reverse']:
                selected=[r for r in rows if r['capacity_mbit']==cap and r['mode']==mode and r['protocol']=='TCP' and r['direction']==direction]
                if not selected:continue
                grouped.append(dict(capacity_mbit=cap,mode=mode,direction=direction,
                    goodput=aggregate([r['received_mbps'] for r in selected]),
                    cpu_busy_pct=aggregate([r['host_cpu_busy_pct'] for r in selected]),
                    cpu_cores=aggregate([r['busy_core_equivalents'] for r in selected]),
                    softirq_cores=aggregate([r['softirq_core_equivalents'] for r in selected]),
                    busiest_core_pct=aggregate([r['busiest_core_busy_pct'] for r in selected]),
                    peak_sample_core_pct=aggregate([r['peak_sample_core_busy_pct'] for r in selected]),
                    idle_cpu_busy_pct=selected[0]['idle_host_cpu_busy_pct'],idle_rtt_ms=selected[0]['idle_rtt_mean_ms'],
                    loaded_rtt_ms=aggregate([r['loaded_rtt_mean_ms'] for r in selected if r['loaded_rtt_mean_ms'] is not None]),
                    loaded_rtt_p95_ms=aggregate([r['loaded_rtt_p95_ms'] for r in selected if r['loaded_rtt_p95_ms'] is not None])))
    (root/'aggregates.json').write_text(json.dumps(grouped,indent=2)+'\n')
    lines=['# Native CPU and matched-capacity results','',f"Status: {'COMPLETE' if complete else 'IN PROGRESS'}. Completed {len(records)} of 160 scheduled transfers.",'',
        'Native Intel Core Ultra 7 165H, 22 logical CPUs, Linux 6.18.34-061834-generic. Whole-host /proc/stat sampled every 0.5 seconds; raw per-core counters and NET_RX/NET_TX softirq counts retained. Busy excludes idle and iowait. CPU includes both gateways, both endpoints, instrumentation and background host activity. It is not VPN-exclusive CPU.','',
        'TCP: five independent 15-second measured transfers in each direction, with 2-second warm-up omitted; CPU and loaded RTT omit the initial 2 seconds approximately. Every capacity/path has a 5-second idle CPU observation and 50-packet idle RTT. UDP: 8 seconds per rate and direction, 1200-byte payload for controlled paths.','',
        'Controlled underlay: TBF on both underlay bridge egress ports, shaping outer transport including tunnel headers. Burst is 1 ms of capacity (minimum 16 KB), queue latency limit 20 ms, accounting overhead 24 bytes and minimum packet unit 64 bytes. This emulates a full-duplex capacity with bounded queues, without added propagation delay. qdisc configuration/counters are retained for every transfer.','',
        'Unrestricted CPU repeats retain default offloads. Controlled comparisons disable TSO/GSO/GRO on all virtual Ethernet interfaces for both paths; therefore controlled-versus-unrestricted differences include offload changes as well as shaping. The original unrestricted evidence remains separate. Mode order alternates by capacity.','',
        '## TCP goodput and whole-host CPU','',
        '| Capacity Mbit/s | Path | Direction | Runs | Received Mbit/s ± SD | Host busy % | Busy CPU equivalents | Softirq CPU equivalents | Idle RTT ms | Loaded RTT ms |',
        '|---|---|---|---:|---:|---:|---:|---:|---:|---:|']
    for g in grouped:
        v=g['goodput'];loaded=g['loaded_rtt_ms']['mean'] if g['loaded_rtt_ms'] else None
        idle=f"{g['idle_rtt_ms']:.3f}" if g['idle_rtt_ms'] is not None else 'N/A'
        load=f'{loaded:.3f}' if loaded is not None else 'N/A'
        lines.append(f"| {g['capacity_mbit'] or 'unrestricted'} | {g['mode']} | {g['direction']} | {v['n']} | {v['mean']:.2f} ± {v['stdev']:.2f} | {g['cpu_busy_pct']['mean']:.2f} | {g['cpu_cores']['mean']:.2f} | {g['softirq_cores']['mean']:.3f} | {idle} | {load} |")
    lines+=['','## CPU detail (five TCP runs per row)','','| Capacity | Path | Direction | Host busy % ± SD | CPU equivalents ± SD | Busiest CPU window % | Peak CPU sample % (mean) | Idle host busy % |','|---|---|---|---:|---:|---:|---:|---:|']
    for g in grouped:
        c=g['cpu_busy_pct'];e=g['cpu_cores']
        idle=f"{g['idle_cpu_busy_pct']:.2f}" if g['idle_cpu_busy_pct'] is not None else 'N/A'
        lines.append(f"| {g['capacity_mbit'] or 'unrestricted'} | {g['mode']} | {g['direction']} | {c['mean']:.2f} ± {c['stdev']:.2f} | {e['mean']:.2f} ± {e['stdev']:.2f} | {g['busiest_core_pct']['mean']:.2f} | {g['peak_sample_core_pct']['mean']:.2f} | {idle} |")
    lines+=['','## Matched-capacity VPN goodput reduction','', '| Capacity Mbit/s | Forward reduction % | Reverse reduction % |','|---:|---:|---:|']
    for cap in [100,500,1000,2500]:
        reductions=[]
        for direction in ['forward','reverse']:
            pair={g['mode']:g for g in grouped if g['capacity_mbit']==cap and g['direction']==direction}
            reductions.append(100*(1-pair['tunnel']['goodput']['mean']/pair['baseline']['goodput']['mean']) if len(pair)==2 else None)
        if any(v is not None for v in reductions):
            lines.append('| '+str(cap)+' | '+' | '.join(f'{v:.2f}' if v is not None else 'pending' for v in reductions)+' |')
    lines+=['','## UDP received throughput, loss and CPU','',
        '| Capacity | Path | Direction | Offered Mbit/s | Payload B | Sent Mbit/s | Received Mbit/s | Loss % | Host busy % | CPU equivalents |','|---|---|---|---:|---:|---:|---:|---:|---:|---:|']
    for r in rows:
        if r['protocol']!='UDP':continue
        loss=f"{r['loss_pct']:.5f}" if r['loss_pct'] is not None else 'N/A (no receiver packets)'
        lines.append(f"| {r['capacity_mbit'] or 'unrestricted'} | {r['mode']} | {r['direction']} | {r['offered_mbit']} | {r['payload_bytes']} | {r['sent_mbps']:.3f} | {r['received_mbps']:.3f} | {loss} | {r['host_cpu_busy_pct']:.2f} | {r['busy_core_equivalents']:.2f} |")
    lines+=['','## UDP targets not reached','','Requested payload rate is not the same as actual offered traffic. These observations have sender throughput below 98% of the target; they cannot establish loss at the full requested load.','','| Point | Requested Mbit/s | Actual sent Mbit/s | Target achieved % |','|---|---:|---:|---:|']
    for r in rows:
        if r['protocol']=='UDP' and r['requested_udp_rate_achieved_pct']<98:
            lines.append(f"| {r['label']} | {r['offered_mbit']} | {r['sent_mbps']:.3f} | {r['requested_udp_rate_achieved_pct']:.2f} |")
    lines+=['','## Limits and interpretation','',
        '- Peak CPU sample usage is the busiest per-core interval at the 0.5-second sampling cadence; window averages can hide scheduler migration between cores. It is not an instantaneous hardware utilization counter.',
        '- A low whole-host percentage can hide one saturated core; per-core summaries and softirq CPU are included. CPU equivalents express busy CPU-seconds per wall second; one equivalent is one fully busy logical CPU, not a physical-core performance normalization.',
        '- At UDP 50%/80%/95%, compare CPU only where measured received goodput is similar. Offered load alone does not prove equal delivered work. For small packets, a low sender rate can limit received goodput.',
        '- iperf3 UDP pacing can consume close to one busy CPU even at low rates. Endpoint process CPU is included in CSV (100% means one CPU), and raw JSON retains endpoint user/system splits. Endpoint CPU is already included in whole-host CPU; do not add the percentages together. Process counters do not isolate all kernel forwarding/crypto work.',
        '- CPU background observations are reported separately. They are not subtracted automatically and do not isolate unrelated activity during transfers.',
        '- Lower loaded RTT at a lower delivered throughput does not establish an intrinsic latency advantage. Interpret the 2.5 Gbit/s VPN RTT alongside its below-capacity goodput.',
        '- RTT under load includes queueing at the specified 20 ms qdiscs. It is specific to this queue configuration, not physical network propagation.',
        '- These are native namespace measurements, not physical-network or Pi measurements. Qdisc limits are software link emulation. No CPU pinning or frequency lock was applied. Five runs provide within-session population SD, not across-session confidence intervals.',
        '- A UDP point with zero received packets has unavailable loss percentage even if iperf reports zero. No failed transfer is silently dropped.',
        '- Original results remain in native-benchmark-results.zip. Build/compiler limitations and source fixes remain documented there; this run reuses the same binaries.',
        '', 'See all-transfers.csv, aggregates.json and run/ for exact parameters, per-transfer JSON, CPU samples, idle/loaded ping, MTU/offload records, qdisc counters and commands.']
    (root/'REPORT.md').write_text('\n'.join(lines)+'\n')
    print('Summarized',len(records),'transfers; complete:',complete)

if __name__=='__main__':main()
