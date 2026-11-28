# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,csv,collections,statistics,hashlib,subprocess,shutil
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/dual-background-20261125';med=statistics.median
runs=[json.loads(p.read_text()) for p in (O/'runs').glob('*.json') if not p.name.endswith('.probe.json')]
summary=[]
with (O/'timeline-stages.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['run','stage','requests','p50_ms','p95_ms','p99_ms','max_ms','cross_boundary','overlap_requests'])
 for v in runs:
  if v['scenario']=='compact':continue
  rows=list(csv.DictReader((O/'runs'/(v['name']+'-timeline.csv')).open()));a=v['compact_start_ns'];b=v['compact_end_ns'];stages=collections.defaultdict(list);cross=collections.Counter();overlap=0;threshold={k:0 for k in [10,50,100]}
  for z in rows:
   s=float(z['start_ns']);d=float(z['elapsed_ns']);stage='before' if s<a else 'window' if s<b and v['comp'] else 'after';stages[stage].append(d);cross[stage]+=int(s<a<s+d or s<b<s+d);overlap+=int(v['comp'] and s<b and s+d>a)
   for k in threshold:threshold[k]+=int(d>k*1e6)
  for stage,ds in stages.items():
   ds.sort();pct=lambda p:ds[min(len(ds)-1,int((len(ds)-1)*p))]/1e6;w.writerow([v['name'],stage,len(ds),pct(.5),pct(.95),pct(.99),pct(1),cross[stage],overlap])
  r=v['requests'];summary.append({'run':v['name'],'version':v['version'],'scenario':v['scenario'],'wal':v['wal'],'comp':v['comp'],'op_s':r['ops_s']*(64 if v['scenario']=='write' else 1),'batch_s':r['ops_s'],'p50_ms':r['p50_ns']/1e6,'p95_ms':r['p95_ns']/1e6,'p99_ms':r['p99_ns']/1e6,'max_ms':r['max_ns']/1e6,'over10':threshold[10],'over50':threshold[50],'over100':threshold[100],'overlap_requests':overlap,'request_ms':r['wall_ns']/1e6,'flush_ms':v['flush_ns']/1e6,'close_ms':v['close_ns']/1e6,'total_ms':v['total_ns']/1e6,'compact_ms':v['compact_ns']/1e6,'rss_mib':v['rss_kib']/1024,'tables_after':v['tables_after'],'new_only_tables':v['new_only_tables'],'logical_ops':v['logical_ops'],'total_op_s':v['logical_ops']*1e9/v['total_ns']})
(O/'per-run.json').write_text(json.dumps(summary,indent=2))
if summary:
 with (O/'per-run.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(summary[0]));w.writeheader();w.writerows(summary)
L=['# 串行与双后台对照','', '当前18/18测试；历史17/17适用测试，跳过旧DB Compaction优先级专项。Release C++17 -O3 -DNDEBUG，LSM_DIAGNOSTICS=OFF。正式表格不含pilot或probe。每组独立进程、夹具复制不计时，当前主引擎不变。','', '## 正式性能中位数','', '|场景|WAL|Compact|版本|轮数|逻辑op/s|API/s|API p50/p95/p99/max ms|>10/50/100ms次数中位|Flush/关闭ms|总ms|Compact ms|RSS MiB|','|---|---:|---:|---|---:|---:|---:|---|---|---|---:|---:|---:|']
groups=collections.defaultdict(list)
for v in summary:
 if v['run'].startswith('r'):groups[v['scenario'],v['wal'],v['comp'],v['version']].append(v)
for (mode,wal,comp,version),vs in sorted(groups.items()):
 lat='/'.join(f"{med(v[k] for v in vs):.3f}" for k in ['p50_ms','p95_ms','p99_ms','max_ms']);thr='/'.join(f"{med(v[k] for v in vs):g}" for k in ['over10','over50','over100']);L.append(f"|{mode}|{wal}|{comp}|{version}|{len(vs)}|{med(v['op_s'] for v in vs):.0f}|{med(v['batch_s'] for v in vs):.0f}|{lat}|{thr}|{med(v['flush_ms'] for v in vs):.3f}/{med(v['close_ms'] for v in vs):.3f}|{med(v['total_ms'] for v in vs):.3f}|{med(v['compact_ms'] for v in vs):.3f}|{med(v['rss_mib'] for v in vs):.2f}|")
L+=['','## 无前台合并','', '|版本|轮数|Compact ms|关闭ms|总ms|RSS MiB|输出表数|','|---|---:|---:|---:|---:|---:|---:|']
for version in ['serial','engine-snapshot']:
 vs=[v for v in runs if v['name'].startswith('r') and v['scenario']=='compact' and v['version']==version]
 if vs:L.append(f"|{version}|{len(vs)}|{med(v['api_ns'] for v in vs)/1e6:.3f}|{med(v['close_ns'] for v in vs)/1e6:.3f}|{med(v['total_ns'] for v in vs)/1e6:.3f}|{med(v['rss_kib'] for v in vs)/1024:.2f}|{vs[0]['after']['tables']}|")
L+=['','## 每轮最大值、超阈值和重叠（保留所有慢轮）','', '|run|op/s|max ms|>10/50/100ms次数|重叠请求|Compact ms|刷盘新表/总表|','|---|---:|---:|---|---:|---:|---|']
for v in sorted(summary,key=lambda x:x['run']):L.append(f"|{v['run']}|{v['op_s']:.0f}|{v['max_ms']:.3f}|{v['over10']}/{v['over50']}/{v['over100']}|{v['overlap_requests']}|{v['compact_ms']:.3f}|{v['new_only_tables']}/{v['tables_after']}|")
L+=['','## 独立机制轮','', '下表计时来自隔离诊断，不混入正式性能。后台Flush与Compact使用不同永久线程槽。Backpressure是含唤醒重新取锁的条件等待。Writer包含写入/fsync/rename/目录同步。父子和两后台区间重叠，不能累加当墙钟。','', '|run|背压次数/总/最大ms|首次锁总ms|队列总ms|Comp构建含Writer ms|Comp发布等待/持锁/Save ms|Flush任务数|构建窗口内Flush发布数|','|---|---|---:|---:|---:|---|---:|---:|']
mech=[]
for v in runs:
 if not v['name'].startswith('m'):continue
 p=json.loads((O/'runs'/(v['name']+'.probe.json')).read_text());assert p['events_dropped']==0 and p['overflow']==0
 cells=collections.defaultdict(lambda:{'ns':0,'count':0,'units':0})
 for c in p['cells']:
  for k in ['ns','count','units']:cells[c['metric']][k]+=c[k]
 events=p['events'];bp=[e['end_ns']-e['start_ns'] for e in events if e['metric']=='backpressure_cv'];build=[e for e in events if e['metric']=='merge_and_build_including_writer'];pubs=[e for e in events if e['metric']=='flush_publish_hold'];over=sum(any(a['start_ns']<e['end_ns']<=a['end_ns'] for a in build) for e in pubs);ms=lambda m:cells[m]['ns']/1e6
 L.append(f"|{v['name']}|{len(bp)}/{sum(bp)/1e6:.3f}/{max(bp,default=0)/1e6:.3f}|{ms('first_db_lock'):.3f}|{ms('queue_lock')+ms('queue_cv'):.3f}|{ms('merge_and_build_including_writer'):.3f}|{ms('compact_publish_wait'):.3f}/{ms('compact_publish_hold'):.3f}/{ms('compact_manifest'):.3f}|{cells['flush_task']['count']}|{over}|")
 mech.append({'run':v['name'],'flush_publish_during_build':over,'cells':dict(cells)})
(O/'mechanism-summary.json').write_text(json.dumps(mech,indent=2));(O/'SUMMARY.md').write_text('\n'.join(L)+'\n')
print('summary',len(runs),'runs')
