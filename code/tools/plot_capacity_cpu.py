#!/usr/bin/env python3
"""Render report figures from saved measurements; requires matplotlib only."""
import argparse
import csv
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('evidence',type=Path)
a=p.parse_args();root=a.evidence
records=json.loads((root/'aggregates.json').read_text())
rows=list(csv.DictReader((root/'all-transfers.csv').open()))
out=root/'figures';out.mkdir(exist_ok=True)
plt.rcParams.update({'font.size':10,'axes.spines.top':False,'axes.spines.right':False,'figure.dpi':160,'savefig.bbox':'tight'})
colors={'baseline':'#38769c','tunnel':'#d06b37'}

def save(fig,name):
    for suffix in ['png','pdf','svg']:
        fig.savefig(out/f'{name}.{suffix}')
    plt.close(fig)

for metric,name,ylabel in [('goodput','capacity-goodput','Received TCP goodput (Mbit/s)'),('cpu_cores','capacity-cpu','Whole-host busy logical-CPU equivalents')]:
    fig,axes=plt.subplots(1,2,figsize=(10.5,4),sharey=True)
    for ax,direction in zip(axes,['forward','reverse']):
        for mode in ['baseline','tunnel']:
            selected=sorted([g for g in records if g['capacity_mbit'] is not None and g['mode']==mode and g['direction']==direction],key=lambda x:x['capacity_mbit'])
            ax.errorbar([g['capacity_mbit'] for g in selected],[g[metric]['mean'] for g in selected],yerr=[g[metric]['stdev'] for g in selected],color=colors[mode],marker='o',capsize=3,label='Direct forwarding' if mode=='baseline' else 'VPN')
        ax.set_title(direction.capitalize());ax.set_xlabel('Shaped outer-link capacity (Mbit/s)');ax.set_xticks([100,500,1000,2500]);ax.grid(alpha=.2);ax.set_ylim(bottom=0)
    axes[0].set_ylabel(ylabel);axes[1].legend()
    fig.suptitle('Matched native namespace paths; TSO/GSO/GRO disabled\nFive TCP runs per point; error bars show population SD',fontsize=11)
    fig.tight_layout();save(fig,name)

fig,axes=plt.subplots(1,2,figsize=(10.5,4),sharey=True)
for ax,direction in zip(axes,['forward','reverse']):
    for mode in ['baseline','tunnel']:
        selected=sorted([g for g in records if g['capacity_mbit'] is not None and g['mode']==mode and g['direction']==direction],key=lambda x:x['capacity_mbit'])
        ax.plot([g['capacity_mbit'] for g in selected],[g['idle_rtt_ms'] for g in selected],marker='o',linestyle='--',color=colors[mode],label=f'{mode}: idle')
        ax.plot([g['capacity_mbit'] for g in selected],[g['loaded_rtt_ms']['mean'] for g in selected],marker='o',color=colors[mode],label=f'{mode}: TCP loaded')
    ax.set_title(direction.capitalize());ax.set_xlabel('Shaped outer-link capacity (Mbit/s)');ax.set_xticks([100,500,1000,2500]);ax.grid(alpha=.2);ax.set_ylim(bottom=0)
axes[0].set_ylabel('Mean ping RTT (ms)');axes[1].legend(fontsize=8)
fig.suptitle('Idle and TCP-loaded RTT; TBF queue latency limit 20 ms',fontsize=11);fig.tight_layout();save(fig,'capacity-rtt')

fig,ax=plt.subplots(figsize=(7,4))
for direction,marker in [('forward','o'),('reverse','s')]:
    caps=[];retention=[]
    for cap in [100,500,1000,2500]:
        pair={g['mode']:g for g in records if g['capacity_mbit']==cap and g['direction']==direction}
        caps.append(cap);retention.append(100*pair['tunnel']['goodput']['mean']/pair['baseline']['goodput']['mean'])
    ax.plot(caps,retention,marker=marker,label=direction.capitalize())
ax.set_xlabel('Shaped outer-link capacity (Mbit/s)');ax.set_ylabel('VPN / direct TCP goodput (%)');ax.set_xticks(caps);ax.set_ylim(0,105);ax.grid(alpha=.2);ax.legend();ax.set_title('Useful-throughput retention over equally constrained paths')
fig.tight_layout();save(fig,'capacity-retention')

sweep=[r for r in rows if r['label'].startswith('unrestricted/packet-sweep/')]
fig,axes=plt.subplots(1,2,figsize=(10.5,4))
xs=[int(r['payload_bytes']) for r in sweep]
axes[0].plot(xs,[float(r['received_mbps']) for r in sweep],marker='o',label='Received');axes[0].plot(xs,[float(r['sent_mbps']) for r in sweep],linestyle='--',label='Sent');axes[0].set_ylabel('UDP payload throughput (Mbit/s)');axes[0].legend()
axes[1].plot(xs,[float(r['busy_core_equivalents']) for r in sweep],marker='o',color=colors['tunnel']);axes[1].set_ylabel('Whole-host busy CPU equivalents')
for ax in axes:
    ax.set_xlabel('UDP payload size (bytes)');ax.grid(alpha=.2);ax.set_ylim(bottom=0)
fig.suptitle('Unrestricted UDP size sweep with CPU instrumentation\n200 Mbit/s offered, 8 seconds per point; default offloads',fontsize=11);fig.tight_layout();save(fig,'packet-size-cpu')
print('Saved figures:',out)
