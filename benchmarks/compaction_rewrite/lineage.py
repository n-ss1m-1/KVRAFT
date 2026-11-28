# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
import csv
from collections import defaultdict
from statistics import median
vs={p.stem:json.loads(p.read_text()) for p in (O/'runs').glob('*.json')}
by=defaultdict(list)
for p in (O/'runs').glob('*-events.jsonl'):
 if p.name.startswith('diag'):continue
 name=p.name[:-13];v=vs[name]
 for e in map(json.loads,p.read_text().splitlines()):
  if e['kind']!='compaction':continue
  def b(k):return sum(f['bytes'] for f in e[k])
  src=e['source'];lo=min(int(f['smallest'],16) for f in src);hi=max(int(f['largest'],16) for f in src);space=v['n'] if v['work']=='unique' else v['n']//4
  inp=src+e['target'];dropped=sum(f['records'] for f in inp)-sum(f['records'] for f in e['output'])
  # Exact interval intersections, not sum of thread times. Stage 'build' includes nested Writer intervals.
  cuts=[(v['begin_ns'],v['request_end_ns']),(v['request_end_ns'],v['flush_end_ns']),(v['flush_end_ns'],v['converged_ns'])]
  overlap=lambda a,z,x,y:max(0,min(z,y)-max(a,x))
  row={'run':name,'config':v['config'],'work':v['work'],'task':e['id'],'level':e['level'],'source_span_fraction':(hi-lo+2)/(space*2),'source_count':len(src),'target_count':len(e['target']),'output_count':len(e['output']),'source_bytes':b('source'),'target_bytes':b('target'),'output_bytes':b('output'),'dropped_versions':dropped,'budget_fraction':b('source')/e['source_budget'],'input_records':sum(f['records'] for f in inp),'output_records':sum(f['records'] for f in e['output'])}
  for label,(a,z) in zip(['write','flush','catchup'],cuts):
   row[label+'_task_overlap_ns']=overlap(e['start_ns'],e['end_ns'],a,z)
   row[label+'_build_overlap_ns']=overlap(e['buildStart_ns'],e['buildEnd_ns'],a,z)
   row[label+'_writer_overlap_ns']=sum(overlap(x,y,a,z) for x,y in e['writer_intervals'])
  by[v['config'],v['work'],e['level']].append(row)
items=[r for g in by.values() for r in g]
with (O/'task-overlap.csv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=list(items[0]));w.writeheader();w.writerows(items)
text=['# 范围、零清理任务与跨边界工作','','|配置|负载|源层|源key范围跨度比例中位|源预算使用比例中位|零清理任务输出占比|输入记录/输出记录|','|---|---|---:|---:|---:|---:|---|']
for k,g in sorted(by.items()):
 out=sum(x['output_bytes'] for x in g);zero=sum(x['output_bytes'] for x in g if x['dropped_versions']==0)
 text.append('|'+ '|'.join(map(str,[*k,round(median(x['source_span_fraction'] for x in g),4),round(median(x['budget_fraction'] for x in g),4),round(zero/out,4),str([sum(x['input_records'] for x in g),sum(x['output_records'] for x in g)])]))+'|')
text+=['','范围跨度是十六进制key数轴跨度/该组逻辑空间跨度，不是重复记录比例。task-overlap.csv按同进程时间轴交集列出每阶段的任务/构建/Writer区间，父子嵌套，不相加解释墙钟；写阶段之前没有初始数据工作。','', '## 具体文件流转链示例（每组第一机制轮）','']
for cfg in ['A','B','C']:
 name='pilot-'+cfg+'-unique';events=[json.loads(x) for x in (O/'runs'/(name+'-events.jsonl')).read_text().splitlines()];prod={};dep={};chain={}
 for e in sorted(events,key=lambda e:e['published_ns']):
  ins=e['source']+e['target'];parent=max(ins,key=lambda f:dep[f['id']]) if ins else None
  for f in e['output']:
   dep[f['id']]=1+dep[parent['id']] if parent else 0
   chain[f['id']]=(chain[parent['id']]+[(e['id'],f['id'])]) if parent else [(0,f['id'])]
 end=max(dep,key=dep.get);seq=chain[end]
 text.append(f'- {name}: '+ ' → '.join(('Flush' if t==0 else '任务'+str(t))+':文件'+str(f) for t,f in seq)+f'（{dep[end]}条消费依赖边）。')
text+=['','每条边证明前一文件被该任务消费、任务生成后一文件；任务可能生成多文件，无法据此认定同一条具体记录经过所有边。唯一键无旧版本丢弃时，各任务输入记录全部保留到输出集合，这是重复整理而非旧版本清理的额外证据。']
(O/'LINEAGE.md').write_text('\n'.join(text)+'\n')
print('\n'.join(text),flush=True)