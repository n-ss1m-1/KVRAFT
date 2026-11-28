# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,csv,statistics as st,hashlib,subprocess
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/write-tail-diagnosis-20261125'
vals=[json.loads(p.read_text()) for p in sorted((O/'runs').glob('*.json')) if not p.name.endswith('.probe.json')]
lines=['# 异常写入长尾诊断','', '所有时间为steady_clock。100 ms仅作筛选。每批64条，1024批，WAL-off；主引擎与既有双后台快照逐文件哈希一致。正式性能不含probe/pilot。','', '|运行|合并|诊断|max ms|>100ms批数|逻辑op/s|请求ms|Flush ms|Compact ms|close ms|总ms|','|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
allslow=[];detailed=[]
for v in vals:
 data=list(csv.DictReader((O/'runs'/(v['name']+'-timeline.csv')).open()));slow=[(i,int(x['start_ns']),int(x['elapsed_ns'])) for i,x in enumerate(data) if int(x['elapsed_ns'])>100000000];v['slow_batches']=len(slow)
 q=v['requests'];lines.append(f"|{v['name']}|{v['comp']}|{v['kind']=='probe'}|{q['max_ns']/1e6:.3f}|{len(slow)}|{v['logical_ops']/(q['wall_ns']/1e9):.0f}|{q['wall_ns']/1e6:.3f}|{v['flush_ns']/1e6:.3f}|{v['compact_ns']/1e6:.3f}|{v['close_ns']/1e6:.3f}|{v['total_ns']/1e6:.3f}|")
 if v['kind']=='probe':
  probe=json.loads((O/'runs'/(v['name']+'.probe.json')).read_text());events=probe['events']
  for i,a,d in slow:
   start=v['epoch_ns']+a;end=start+d
   overlap=[dict(e,start_ms=(e['start_ns']-v['epoch_ns'])/1e6,end_ms=(e['end_ns']-v['epoch_ns'])/1e6,duration_ms=(e['end_ns']-e['start_ns'])/1e6) for e in events if e['start_ns']<=end and e['end_ns']>=start and e['phase'] in [3,5,6]]
   overlap.sort(key=lambda e:(e['start_ns'],e['end_ns']))
   detailed.append(dict(run=v['name'],comp=v['comp'],batch=i,start_ms=a/1e6,elapsed_ms=d/1e6,events=overlap))
 for i,a,d in slow:allslow.append(dict(run=v['name'],comp=v['comp'],diagnostics=v['kind']=='probe',batch=i,start_ms=a/1e6,elapsed_ms=d/1e6))
(O/'slow-batches.json').write_text(json.dumps(allslow,indent=2));(O/'slow-overlaps.json').write_text(json.dumps(detailed,indent=2))
lines+=['','## 频率与开销（pilot单列于上表，不计以下正式统计）','','|合并|诊断|轮数|异常轮数|异常批数/总批数|逻辑op/s中位|p50/p95/p99/max中位 ms|','|---:|---:|---:|---:|---|---:|---|']
for c in [0,1]:
 for k in ['engine-snapshot','probe']:
  vs=[v for v in vals if v['comp']==c and v['kind']==k and not v['name'].startswith('pilot')]
  if not vs:continue
  med=lambda f:st.median(f(v) for v in vs)
  lines.append(f"|{c}|{k=='probe'}|{len(vs)}|{sum(v['slow_batches']>0 for v in vs)}|{sum(v['slow_batches'] for v in vs)}/{len(vs)*1024}|{med(lambda v:v['logical_ops']*1e9/v['requests']['wall_ns']):.0f}|"+'/'.join(f"{med(lambda v:v['requests'][m+'_ns'])/1e6:.3f}" for m in ['p50','p95','p99','max'])+'|')
lines+=['','## 捕获的诊断慢批次','']
for d in detailed:
 lines += [f"### {d['run']} batch {d['batch']}，{d['start_ms']:.3f}–{d['start_ms']+d['elapsed_ms']:.3f} ms，耗时{d['elapsed_ms']:.3f} ms",'', '|线程槽|阶段|task|file|起ms|止ms|耗时ms|','|---:|---|---:|---:|---:|---:|---:|']
 for e in d['events']:
  if e['duration_ms']>=1 or e['metric'] in ['ImmutableRemoved','Claim','Ready','FileId']:
   lines.append(f"|{e['slot']}|{e['metric']}|{e['task']}|{e['file']}|{e['start_ms']:.3f}|{e['end_ms']:.3f}|{e['duration_ms']:.3f}|")
lines+=['','完整相交事件在slow-overlaps.json，父子与线程区间不能相加。系统调用事件计的是整个调用墙钟时间，未区分内核阻塞、宿主存储和调度。']
(O/'SUMMARY.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines[-30:]))
if (O/'FINDINGS.md').exists():
 with (O/'SUMMARY.md').open('a') as f:f.write('\n'+(O/'FINDINGS.md').read_text())
