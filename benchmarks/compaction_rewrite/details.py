# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
from statistics import median
from collections import defaultdict
import csv
rs=[json.loads(p.read_text()) for p in (O/'runs').glob('*.json')]
lines=['# 机制与补充对照','', '以下机制次数、字节来自独立轮，不与正式请求延迟混用。文件流转逐边见 file-flow.csv；任务范围、源预算实际用量及阶段见 tasks.csv；完整层快照与文件范围见 *-events.jsonl。','', '|配置|负载|层|任务数(主机制三轮合计)|源MiB|目标MiB|输出MiB|目标/源中位|最大比值|最大任务源/目标/输出MiB|','|---|---|---:|---:|---:|---:|---:|---:|---:|---|']
rows=list(csv.DictReader((O/'tasks.csv').open()));g=defaultdict(list)
for x in rows:
 if x['kind']!='compaction':continue
 v=next(v for v in rs if v.get('name')==x['run']);g[v['config'],v['work'],x['source_level']].append(x)
for k,xs in sorted(g.items()):
 b=lambda f:sum(float(x[f]) for x in xs)/1048576
 mx=max(xs,key=lambda x:float(x['output_bytes']))
 lines.append('|'+ '|'.join(map(str,[*k,len(xs),round(b('source_bytes'),2),round(b('target_bytes'),2),round(b('output_bytes'),2),round(median(float(x['ratio_target_source']) for x in xs),2),round(max(float(x['ratio_target_source']) for x in xs),2),'/'.join(str(round(float(mx[f])/1048576,2)) for f in ['source_bytes','target_bytes','output_bytes'])]))+'|')
lines+=['','## 输出按发布时间归属','','|配置|负载|请求中发布MiB|请求后至Flush返回MiB|Flush后追赶MiB|','|---|---|---:|---:|---:|']
g=defaultdict(list)
for x in rows:
 v=next(v for v in rs if v.get('name')==x['run']);g[v['config'],v['work']].append(x)
for k,xs in sorted(g.items()):
 runs=len(set(x['run'] for x in xs));vals=[sum(float(x['output_bytes']) for x in xs if x['phase']==p)/1048576/runs for p in ['write','final_flush','catchup']]
 lines.append('|'+ '|'.join(map(str,[*k,*[round(x,2) for x in vals]]))+'|')
lines+=['','归属按文件发布时刻，跨边界任务仍只有一份输出，不能解释成全部工作都发生在该阶段。原始起止可用于区间交集；后台累计阶段不可相加解释墙钟。','', '## 收敛后查询（相同4096个live请求，两遍，稳定阶段中位）','','|配置|负载|缓存|轮数|op/s|p95 us|p99 us|max us|Usage B|容量 B|','|---|---|---:|---:|---:|---:|---:|---:|---:|---:|']
g=defaultdict(list)
for v in rs:
 if v.get('name','').startswith('q'):g[v['config'],v['auto'] if False else v['name'].split('-')[2],v['name'].split('-')[1],bool(v['cache_capacity'])].append(v)
for (cfg,label,work,cap),vs in sorted(g.items()):
 lines.append('|'+ '|'.join(map(str,[label,work,int(cap),len(vs),round(median(v['steady']['api_s'] for v in vs)),*[round(median(v['steady'][f+'_ns'] for v in vs)/1000,3) for f in ['p95','p99','max']],round(median(v['usage_steady'] for v in vs)),round(median(v['cache_capacity'] for v in vs))]))+'|')
lines+=['','## 候选表（独立轮，两遍共8192请求）','']
for p in sorted((O/'runs').glob('candidate-*.json')):
 v=json.loads(p.read_text());lines.append(f'- {p.stem}: {v["candidate_tables"]/8192:.4f} Reader候选/请求。不是块命中率。')
lines+=['','## 观察与详细诊断开销对照','', '|对照|轮数|请求op/s中位|请求至收敛ms中位|范围ms|','|---|---:|---:|---:|---|']
for prefix in ['overhead','diag']:
 for kind in (['previous','observer'] if prefix=='overhead' else ['observer','mechanism']):
  vs=[v for v in rs if v.get('name','').startswith(prefix) and v.get('kind')==kind]
  if vs:
   ns=[(v['converged_ns']-v['begin_ns'])/1e6 for v in vs] if prefix=='diag' else [v['request_to_flush_ns']/1e6 for v in vs]
   lines.append(f'|{prefix}/{kind}|{len(vs)}|{median(v["writes"]["logical_ops_s"] for v in vs):.0f}|{median(ns):.3f}|{min(ns):.3f}–{max(ns):.3f}|')
lines+=['','overhead是同A配置write模式（不等收敛），previous为上轮同哈希原引擎+friend访问，observer仅增加busy守卫/通知；表中该组使用含Flush时间，并非收敛时间。diag为完整收敛，详细轮有额外文件记录遍历、分配和锁内层快照，会改变时序；开销不能机械扣除。首次/稳定查询及原始逐请求值均保留。']
(O/'DETAILS.md').write_text('\n'.join(lines)+'\n');print('\n'.join(lines),flush=True)