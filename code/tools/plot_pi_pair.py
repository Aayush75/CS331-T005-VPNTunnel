#!/usr/bin/env python3
"""Render physical Pi benchmark report figures from retained CSV/aggregates."""
import argparse,csv,json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
p=argparse.ArgumentParser(description=__doc__);p.add_argument('evidence',type=Path);a=p.parse_args();root=a.evidence
summary=json.loads((root/'aggregates.json').read_text());rows=list(csv.DictReader((root/'all-transfers.csv').open()))
out=root/'figures';out.mkdir(exist_ok=True)
plt.rcParams.update({'font.size':10,'axes.spines.top':False,'axes.spines.right':False,'figure.dpi':160,'savefig.bbox':'tight'})
colors={'direct':'#38769c','tunnel':'#d06b37'}
def save(fig,name):
 for suffix in ['png','pdf','svg']:fig.savefig(out/f'{name}.{suffix}')
 plt.close(fig)
fig,ax=plt.subplots(figsize=(7,4));width=.34
for j,mode in enumerate(['direct','tunnel']):
 ss=[next(g for g in summary if g['mode']==mode and g['direction']==d) for d in ['forward','reverse']]
 ax.bar([i+(j-.5)*width for i in range(2)],[g['goodput']['mean'] for g in ss],width,yerr=[g['goodput']['stdev'] for g in ss],capsize=4,label='Direct Ethernet' if mode=='direct' else 'VPN over Ethernet',color=colors[mode])
ax.set_xticks([0,1],['A → B','B → A']);ax.set_ylabel('Received TCP goodput (Mbit/s)');ax.set_title('Native Pi-to-Pi: same 1 Gbit/s Ethernet cable\nFive 20-second runs; error bars show population SD');ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True);ax.legend();fig.tight_layout();save(fig,'pi-tcp-goodput')
fig,ax=plt.subplots(figsize=(9,4));groups=[(s,d) for s in 'ab' for d in ['forward','reverse']]
for j,mode in enumerate(['direct','tunnel']):
 gs=[next(g for g in summary if g['mode']==mode and g['direction']==d) for s,d in groups]
 vals=[g[f'pi_{s}_cpu_busy_pct'] for g,(s,d) in zip(gs,groups)]
 ax.bar([i+(j-.5)*width for i in range(4)],[v['mean'] for v in vals],width,yerr=[v['stdev'] for v in vals],capsize=4,label=mode,color=colors[mode])
ax.set_xticks(range(4),[f'Pi {s.upper()}\n{d}' for s,d in groups]);ax.set_ylabel('Whole-Pi busy CPU (%)');ax.set_title('CPU during TCP transfers: four logical CPUs per Pi\nIncludes endpoint work and VPN processing; error bars show SD');ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True);ax.legend();fig.tight_layout();save(fig,'pi-tcp-cpu')
fig,axes=plt.subplots(1,2,figsize=(10,4),sharey=True)
for ax,direction in zip(axes,['forward','reverse']):
 for mode in ['direct','tunnel']:
  rr=sorted([r for r in rows if r['protocol']=='UDP' and r['mode']==mode and r['direction']==direction],key=lambda r:float(r['offered_mbit']))
  xs=[float(r['offered_mbit']) for r in rr]
  ax.plot(xs,[float(r['received_mbps']) for r in rr],marker='o',label=f'{mode}: received',color=colors[mode]);ax.plot(xs,[float(r['sent_mbps']) for r in rr],linestyle='--',color=colors[mode],label=f'{mode}: sent')
 ax.set_title(direction.capitalize());ax.set_xlabel('Requested UDP payload rate (Mbit/s)');ax.set_xticks([500,800,950]);ax.grid(alpha=.2);ax.set_ylim(bottom=0)
axes[0].set_ylabel('Actual payload throughput (Mbit/s)');axes[1].legend(fontsize=8);fig.suptitle('Physical Pi UDP: 1200-byte payloads, 10 seconds per point',fontsize=11);fig.tight_layout();save(fig,'pi-udp-goodput')
fig,ax=plt.subplots(figsize=(8,4));labels=[];idle=[];loaded=[]
for mode in ['direct','tunnel']:
 for direction in ['forward','reverse']:
  g=next(g for g in summary if g['mode']==mode and g['direction']==direction)
  labels.append(mode+'\n'+direction);idle.append(g['idle_rtt_ms']);loaded.append(g['loaded_rtt']['mean'])
ax.bar([i-width/2 for i in range(4)],idle,width,label='Idle');ax.bar([i+width/2 for i in range(4)],loaded,width,label='During TCP');ax.set_xticks(range(4),labels);ax.set_ylabel('Mean ping RTT (ms)');ax.set_title('Idle versus TCP-loaded RTT over the physical cable');ax.grid(axis='y',alpha=.2);ax.set_axisbelow(True);ax.legend();fig.tight_layout();save(fig,'pi-rtt')
print('Saved Pi figures:',out)
