#!/usr/bin/env python3
"""Summarize matched physical Pi measurements and cross-host CPU windows."""
import argparse
import csv
from datetime import datetime
import json
from pathlib import Path
import re
import statistics


def load(path):return json.loads(path.read_text())


def agg(xs):return dict(n=len(xs),mean=statistics.mean(xs),stdev=statistics.pstdev(xs)) if xs else None


def epoch(sample):return datetime.fromisoformat(sample['utc']).timestamp()


def cpu_window(samples,start,end,hz):
    selected=[s for s in samples if start<=epoch(s)<=end]
    if len(selected)<2:raise RuntimeError('CPU window has insufficient samples')
    a,z=selected[0],selected[-1];wall=z['monotonic']-a['monotonic'];cores={};peak=0
    for name,initial in a['cpus'].items():
        delta=[v-u for u,v in zip(initial,z['cpus'][name])];total=sum(delta);busy=total-delta[3]-delta[4]
        cores[name]=dict(busy_pct=100*busy/total if total else 0,busy_cpu_seconds=busy/hz,busy_core_equivalents=busy/hz/wall,softirq_cpu_seconds=delta[6]/hz,user_cpu_seconds=(delta[0]+delta[1])/hz,system_cpu_seconds=delta[2]/hz)
    for first,last in zip(selected,selected[1:]):
        for name,initial in first['cpus'].items():
            if name=='cpu':continue
            d=[v-u for u,v in zip(initial,last['cpus'][name])];total=sum(d)
            if total:peak=max(peak,100*(total-d[3]-d[4])/total)
    all_cpu=cores.pop('cpu');temps=[int(s['telemetry']['temperature_millic'])/1000 for s in selected if s['telemetry'].get('temperature_millic')]
    flags=sorted(set(s['telemetry'].get('throttling','unknown') for s in selected))
    return dict(window_seconds=wall,samples=len(selected),**all_cpu,softirq_core_equivalents=all_cpu['softirq_cpu_seconds']/wall,busiest_core_pct=max(x['busy_pct'] for x in cores.values()),peak_sample_core_busy_pct=peak,temperature_mean_c=statistics.mean(temps),temperature_max_c=max(temps),throttling_flags=flags,per_core=cores)


def ping(text,after=None,before=None):
    rtts=[]
    for line in text.splitlines():
        m=re.search(r'time[=<]([\d.]+)\s*ms',line);ts=re.search(r'\[([\d.]+)\]',line)
        if m and (after is None or ts and float(ts[1])>=after) and (before is None or ts and float(ts[1])<=before):rtts.append(float(m[1]))
    m=re.search(r'(\d+) packets transmitted, (\d+) received',text)
    ordered=sorted(rtts)
    return dict(transmitted=int(m[1]) if m else None,received=int(m[2]) if m else None,rtt_samples=len(rtts),mean_ms=statistics.mean(rtts) if rtts else None,p95_ms=ordered[min(len(ordered)-1,int(.95*len(ordered)))] if ordered else None,max_ms=max(rtts) if rtts else None)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('evidence',type=Path);a=p.parse_args();root=a.evidence.resolve()
    parameters=load(root/'parameters.json');records=load(root/'results.json');raw=root/'raw';rows=[];detailed=[];idle={};metadata={}
    caches={}
    for mode in ['direct','tunnel']:
        for side in 'ab':
            folder=raw/mode/f'pi-{side}'
            samples=[json.loads(s) for s in (folder/'cpu.jsonl').read_text().splitlines() if s]
            hz=load(folder/'monitor-finished.json')['clock_ticks_per_second'];caches[(mode,side)]=(samples,hz)
            window=load(folder/'idle-window.json');summary=cpu_window(samples,window['start']['epoch'],window['end']['epoch'],hz)
            idle_ping=ping(load(folder/'ping-idle.json')['stdout'])
            idle[(mode,side)]=dict(cpu=summary,ping=idle_ping)
            metadata[(mode,side)]=load(folder/'metadata.json')
    for r in records:
        mode=r['mode'];folder=raw/mode/'pi-a'/r['label'];data=load(folder/'iperf.json')
        omit=2 if r['protocol']=='TCP' else 0
        begin=r['start']['epoch']+omit;end=r['end']['epoch']
        cpus={side:cpu_window(*caches[(mode,side)][:1],begin,end,caches[(mode,side)][1]) for side in 'ab'}
        loaded_path=folder/'ping-loaded.txt';loaded=ping(loaded_path.read_text(),begin,end) if loaded_path.exists() else {}
        ep=data['end'].get('cpu_utilization_percent',{})
        row=dict(mode=mode,label=r['label'],protocol=r['protocol'],direction='reverse' if r['reverse'] else 'forward',offered_mbit=r.get('rate_mbit'),sent_mbps=r['sent_mbps'],received_mbps=r['received_mbps'],loss_pct=r['loss_pct'],received_bytes=r['received_bytes'],received_packets=r['received_packets'],tcp_retransmits=r['retransmits'],udp_jitter_ms=data['end']['sum_received'].get('jitter_ms'),requested_rate_achieved_pct=100*r['sent_mbps']/r['rate_mbit'] if r.get('rate_mbit') else None,loaded_rtt_mean_ms=loaded.get('mean_ms'),loaded_rtt_p95_ms=loaded.get('p95_ms'),loaded_ping_samples=loaded.get('rtt_samples'),loaded_ping_transmitted=loaded.get('transmitted'),loaded_ping_received=loaded.get('received'),idle_rtt_mean_ms=idle[(mode,'a')]['ping']['mean_ms'],iperf_client_cpu_pct_one_core_scale=ep.get('host_total'),iperf_server_cpu_pct_one_core_scale=ep.get('remote_total'))
        for side in 'ab':
            c=cpus[side]
            row.update({f'pi_{side}_cpu_busy_pct':c['busy_pct'],f'pi_{side}_busy_core_equivalents':c['busy_core_equivalents'],f'pi_{side}_softirq_core_equivalents':c['softirq_core_equivalents'],f'pi_{side}_busiest_core_pct':c['busiest_core_pct'],f'pi_{side}_peak_sample_core_pct':c['peak_sample_core_busy_pct'],f'pi_{side}_max_temp_c':c['temperature_max_c'],f'pi_{side}_cpu_window_seconds':c['window_seconds'],f'pi_{side}_cpu_seconds_per_received_gbit':c['busy_core_equivalents']/(r['received_mbps']/1000) if r['received_mbps'] else None})
        rows.append(row);detailed.append(dict(mode=mode,label=r['label'],cpu=cpus,loaded_ping=loaded))
    with (root/'all-transfers.csv').open('w') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    (root/'cpu-per-transfer.json').write_text(json.dumps(detailed,indent=2)+'\n')
    summaries=[]
    for mode in ['direct','tunnel']:
        for direction in ['forward','reverse']:
            selected=[r for r in rows if r['mode']==mode and r['protocol']=='TCP' and r['direction']==direction]
            g=dict(mode=mode,direction=direction,goodput=agg([r['received_mbps'] for r in selected]),loaded_rtt=agg([r['loaded_rtt_mean_ms'] for r in selected]),idle_rtt_ms=idle[(mode,'a')]['ping']['mean_ms'])
            for side in 'ab':
                for key in ['cpu_busy_pct','busy_core_equivalents','softirq_core_equivalents','busiest_core_pct','peak_sample_core_pct','max_temp_c']:
                    g[f'pi_{side}_{key}']=agg([r[f'pi_{side}_{key}'] for r in selected])
            summaries.append(g)
    (root/'aggregates.json').write_text(json.dumps(summaries,indent=2)+'\n')
    (root/'idle-summary.json').write_text(json.dumps({f'{m}-{s}':v for (m,s),v in idle.items()},indent=2)+'\n')
    lines=['# Physical Pi-to-Pi Ethernet benchmark results','',
        'Two Raspberry Pi 5 gateways, native Linux 6.18.34+rpt-rpi-2712 (aarch64, 16 KiB pages), joined by a direct 1 Gbit/s full-duplex Ethernet cable. Wi-Fi carries SSH management only. Traffic originates and terminates on the Pis; CPU includes endpoint generation/receive work plus VPN processing.','',
        'Direct endpoints: 172.31.250.11 ↔ 172.31.250.12. VPN endpoints: 10.77.1.10 ↔ 10.77.2.10, routed through vpn0 and the same Ethernet link. Reserved gateway addresses (.1) are not test endpoints.','',
        'Five TCP runs per direction/configuration, 20 measured seconds each plus 2 omitted warm-up seconds. Single TCP stream; population standard deviation shown. UDP payload 1200 bytes, 10 seconds per point at 500/800/950 Mbit/s requested, both directions. Offloads were left at defaults and recorded per configuration. Direct ran before VPN in one session.','',
        '## TCP goodput, CPU and RTT','',
        '| Path | Direction | TCP received Mbit/s ± SD | Pi A host CPU % | Pi B host CPU % | Idle RTT ms | TCP-loaded RTT ms |',
        '|---|---|---:|---:|---:|---:|---:|']
    for g in summaries:
        v=g['goodput'];lines.append(f"| {g['mode']} | {g['direction']} | {v['mean']:.2f} ± {v['stdev']:.3f} | {g['pi_a_cpu_busy_pct']['mean']:.2f} | {g['pi_b_cpu_busy_pct']['mean']:.2f} | {g['idle_rtt_ms']:.3f} | {g['loaded_rtt']['mean']:.3f} |")
    lines+=['','## VPN useful-throughput reduction','']
    for direction in ['forward','reverse']:
        pair={g['mode']:g['goodput']['mean'] for g in summaries if g['direction']==direction}
        lines.append(f"- {direction.capitalize()}: {100*(1-pair['tunnel']/pair['direct']):.2f}% reduction; VPN retains {100*pair['tunnel']/pair['direct']:.2f}% of direct TCP goodput.")
    lines+=['','## CPU and thermal detail','',
        '| Path | Direction | Pi | Busy CPU equivalents ± SD | Softirq CPU equivalents | Busiest core window % | Peak sample core % | Maximum temperature C |',
        '|---|---|---|---:|---:|---:|---:|---:|']
    for g in summaries:
        for side in 'ab':
            c=g[f'pi_{side}_busy_core_equivalents'];mx=max(r[f'pi_{side}_max_temp_c'] for r in rows if r['mode']==g['mode'] and r['protocol']=='TCP' and r['direction']==g['direction'])
            lines.append(f"| {g['mode']} | {g['direction']} | {side.upper()} | {c['mean']:.2f} ± {c['stdev']:.2f} | {g[f'pi_{side}_softirq_core_equivalents']['mean']:.3f} | {g[f'pi_{side}_busiest_core_pct']['mean']:.2f} | {g[f'pi_{side}_peak_sample_core_pct']['mean']:.2f} | {mx:.1f} |")
    lines+=['','## Idle background','', '| Path | Pi | Host CPU % | Busy CPU equivalents | Idle RTT ms | Ping sent / received |','|---|---|---:|---:|---:|---:|']
    for (mode,side),v in idle.items():
        lines.append(f"| {mode} | {side.upper()} | {v['cpu']['busy_pct']:.2f} | {v['cpu']['busy_core_equivalents']:.3f} | {v['ping']['mean_ms']:.3f} | {v['ping']['transmitted']} / {v['ping']['received']} |")
    lines+=['','## UDP load points','',
        '| Path | Direction | Requested Mbit/s | Actual sent Mbit/s | Received Mbit/s | Loss % | Pi A CPU % | Pi B CPU % | Target reached? |',
        '|---|---|---:|---:|---:|---:|---:|---:|---|']
    for r in rows:
        if r['protocol']!='UDP':continue
        loss=f"{r['loss_pct']:.5f}" if r['loss_pct'] is not None else 'N/A'
        reached='yes (≥98%)' if r['requested_rate_achieved_pct']>=98 else f"no ({r['requested_rate_achieved_pct']:.1f}%)"
        lines.append(f"| {r['mode']} | {r['direction']} | {r['offered_mbit']} | {r['sent_mbps']:.2f} | {r['received_mbps']:.2f} | {loss} | {r['pi_a_cpu_busy_pct']:.2f} | {r['pi_b_cpu_busy_pct']:.2f} | {reached} |")
    lines+=['','## TCP retransmissions and ping delivery','','| Path | TCP direction | Total retransmissions (five runs) | Loaded ping sent / received (includes warm-up) |','|---|---|---:|---:|']
    for mode in ['direct','tunnel']:
        for direction in ['forward','reverse']:
            rr=[r for r in rows if r['mode']==mode and r['protocol']=='TCP' and r['direction']==direction]
            lines.append(f"| {mode} | {direction} | {sum(r['tcp_retransmits'] or 0 for r in rr)} | {sum(r['loaded_ping_transmitted'] or 0 for r in rr)} / {sum(r['loaded_ping_received'] or 0 for r in rr)} |")
    lines+=['','TCP retransmissions are recovered by TCP; the count is not an application-data loss percentage. All raw counts remain in the JSON/CSV.','','## Method and limits','',
        '- Loaded ping probes originate on Pi A for both TCP traffic directions; idle probes were recorded from both Pis. RTT statistics omit the TCP warm-up approximately; full-transfer ping delivery counts include it.',
        '- CPU counters on both Pis were sampled every 0.5 seconds for the entire phase. Per-transfer windows use synchronized UTC timestamps; TCP windows exclude approximately the first 2 seconds. Boundary alignment has sampling/SSH/iperf startup uncertainty. Raw timestamps/counters are retained.',
        '- Host busy % spans four logical CPUs on each Pi; one busy CPU equivalent means one busy CPU-second per wall second, without normalizing performance across architectures or operating frequencies. Busy excludes idle and iowait; guest fields are not double-counted. Per-core and softirq counters are included. Background is reported separately, not silently subtracted.',
        '- Whole-host CPU includes generators, receive processing, controllers, kernel crypto/forwarding, instrumentation and background activity. Endpoint process CPU percentages in CSV use one-core scale and are already included in host CPU; do not add them.',
        '- CPU frequencies/governors, thermal readings and throttling were recorded. Temperature sampling is every 0.5 seconds; throttling flags every 5 seconds and at each client completion.',
        '- UDP target rates are payload rates. Encapsulation/headers consume physical link capacity too. If actual sender rate misses its target, low measured loss cannot establish support for that requested load.',
        '- TCP retransmissions are retained in CSV and original JSON; they are not an application-data loss percentage. Idle/loaded ping loss must be read separately.',
        '- One ordered session with five TCP observations per direction: SD is within-session spread, not across-session confidence intervals. No CPU pinning or frequency lock was applied.',
        '- Lower loaded RTT while delivering less traffic does not prove a latency advantage at equal delivered work. Pi-local endpoints also do not exercise LAN-host forwarding in the same way as the separate laptop demonstration.',
        '', 'See validation.json, parameters.json, commands.json, all-transfers.csv, cpu-per-transfer.json, and raw/ for independent verification. No private keys or passwords are included.']
    (root/'REPORT.md').write_text('\n'.join(lines)+'\n')
    print('Summarized',len(rows),'physical transfers')

if __name__=='__main__':main()
