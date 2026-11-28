# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
from collections import defaultdict,Counter
from statistics import median
import csv
vs=[]
for p in (O/'runs').glob('*.json'):
 if p.name.endswith('-backpressure.json'):continue
 v=json.loads(p.read_text())
 if 'name' in v:vs.append(v)
formal=[]
for v in vs:
 if not v['name'].startswith(('s1','s2','wal')):continue
 cs=[0]*(7 if v['scale']=='C' else 3);bs=cs.copy()
 for t in v['final']:cs[t['level']]+=1;bs[t['level']]+=t['bytes']
 x=dict(run=v['name'],scale=v['scale'],budget=v['budget'],factor=v['factor'],work=v['work'],wal=v['wal'],request_ops_s=v['writes']['logical_ops_s'],flush_ops_s=v['n']*1e9/v['request_to_flush_ns'],flush_ms=v['flush_ns']/1e6,catchup_ms=v['converge_ns']/1e6,total_ms=(v['converged_ns']-v['begin_ns'])/1e6,close_ms=v['close_ns']/1e6,rss_mib=v['rss_kib']/1024,amp=(v['flush_output_bytes']+v['compaction_output_bytes'])/v['logical_encoded_bytes'],flush_count=v['flush_count'],levels=str(cs),level_bytes=str(bs),final_bytes=v['storage']['bytes'],records=v['storage']['records'])
 for q in ['p50','p95','p99','max']:x[q+'_ms']=v['writes'][q+'_ns']/1e6
 formal.append(x)
def csvout(name,rows):
 if rows:
  with (O/name).open('w') as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
csvout('formal-rounds.csv',formal)
g=defaultdict(list)
for x in formal:g[x['scale'],x['wal'],x['budget'],x['factor'],x['work']].append(x)
lines=['# 参数对照汇总','', '正式轮诊断关闭，输出字节为任务/文件边界独占线程累计，覆盖到可靠收敛并join。逐轮见formal-rounds.csv。','', '|规模|WAL|预算KiB|倍率|负载|轮数|输出放大|请求op/s|含Flush op/s|p99 ms|max ms|Flush ms|追赶ms|总收敛ms|RSS MiB|层文件数|','|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|']
for k,xs in sorted(g.items()):
 m=lambda f:median(x[f] for x in xs)
 lines.append('|'+ '|'.join(map(str,[k[0],k[1],k[2]//1024,k[3],k[4],len(xs),round(m('amp'),4),round(m('request_ops_s')),round(m('flush_ops_s')),*[round(m(f),3) for f in ['p99_ms','max_ms','flush_ms','catchup_ms','total_ms']],round(m('rss_mib'),2),xs[0]['levels']]))+'|')
(O/'SUMMARY.md').write_text('\n'.join(lines)+'\n');print('\n'.join(lines),flush=True)
# mechanism complete runs only
rows=[];files=[]
for p in sorted((O/'runs').glob('*-events.jsonl')):
 name=p.name[:-13];pjson=p.with_name(name+'.json')
 if not pjson.exists():continue
 v=json.loads(pjson.read_text());es=[json.loads(x) for x in p.read_text().splitlines()]
 for e in es:
  if e['kind']!='compaction':continue
  b=lambda k:sum(f['bytes'] for f in e[k]);nr=lambda k:sum(f['records'] for f in e[k]);out=nr('output');inp=nr('source')+nr('target')
  row=dict(run=name,scale=v['scale'],budget=v['budget'],factor=v['factor'],work=v['work'],task=e['id'],level=e['level'],base_bytes=e['baseBytes_ns'],final_source_bytes=e['finalBytes_ns'],base_files=e['baseCount_ns'],source_files=len(e['source']),target_files=len(e['target']),candidates=e['candidates_ns'],accepted=e['accepted_ns'],stop=e['stop_reason'],target_bytes=b('target'),total_input_bytes=b('source')+b('target'),output_bytes=b('output'),input_records=inp,output_records=out,dropped_versions=inp-out,selection_hold_ns=e['selected_ns']-e['selectionLocked_ns'],selection_core_ns=e['selectionDone_ns']-e['selectionLocked_ns'],task_ns=e['end_ns']-e['start_ns'],targets_equal=e['base_targets']==e['final_targets'])
  rows.append(row)
  for k in ['source','target','output']:
   for f in e[k]:files.append(dict(run=name,task=e['id'],role=k,**f))
csvout('tasks.csv',rows);csvout('files.csv',files)
mg=defaultdict(list)
for x in rows:
 if not x['run'].startswith('overhead'):mg[x['scale'],x['budget'],x['factor'],x['work']].append(x)
lines=['# 独立机制轮','', '|规模|预算KiB|倍率|负载|任务数|基础源KiB中位|扩展源KiB中位|候选/接受文件|停止原因|目标/源中位|最大总输入MiB|最大输出MiB|选表持锁p50/max us|','|---|---:|---:|---|---:|---:|---:|---|---|---:|---:|---:|---|']
for k,xs in sorted(mg.items()):
 ratios=[x['target_bytes']/x['final_source_bytes'] for x in xs]
 lines.append('|'+ '|'.join(map(str,[k[0],k[1]//1024,k[2],k[3],len(xs),round(median(x['base_bytes'] for x in xs)/1024,2),round(median(x['final_source_bytes'] for x in xs)/1024,2),str([sum(x['candidates'] for x in xs),sum(x['accepted'] for x in xs)]),str(dict(Counter(x['stop'] for x in xs))),round(median(ratios),3),round(max(x['total_input_bytes'] for x in xs)/1048576,2),round(max(x['output_bytes'] for x in xs)/1048576,2),str([round(median(x['selection_hold_ns'] for x in xs)/1000,2),round(max(x['selection_hold_ns'] for x in xs)/1000,2)])]))+'|')
(O/'MECHANISM.md').write_text('\n'.join(lines)+'\n')