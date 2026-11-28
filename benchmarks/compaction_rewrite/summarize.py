# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import *
from collections import defaultdict,Counter
from statistics import median
rows=[];flows=[];summ=[]
for p in sorted((O/'runs').glob('*-events.jsonl')):
 if p.name.startswith('diag'):continue
 name=p.name[:-13];v=json.loads((O/'runs'/(name+'.json')).read_text());es=[json.loads(x) for x in p.read_text().splitlines()];known={};consumed=set();producer={};depth={};by=defaultdict(int);zero=0;fl=0;outsum=0
 for e in sorted(es,key=lambda e:e['published_ns']):
  inp=e['source']+e['target'];out=e['output'];ir=sum(f['records'] for f in inp);orr=sum(f['records'] for f in out)
  if e['kind']=='compaction':
   assert ir>=orr
   assert e['start_ns']<=e['selected_ns']<=e['buildStart_ns']<=e['buildEnd_ns']<=e['wait_ns']<=e['locked_ns']<=e['published_ns']<=e['unlock_ns']<=e['end_ns']
   for f in inp:
    assert f['id'] in known and f['id'] not in consumed and known[f['id']]==f,(name,f)
    consumed.add(f['id'])
   d=1+max(depth[f['id']] for f in inp)
  else:fl+=1;d=0
  ib=sum(f['bytes'] for f in inp);sb=sum(f['bytes'] for f in e['source']);tb=sum(f['bytes'] for f in e['target']);ob=sum(f['bytes'] for f in out);outsum+=ob;by[e['level']+1 if e['kind']=='compaction' else 0]+=ob
  if e['kind']=='compaction' and ir==orr:zero+=ob
  phase='write' if e['published_ns']<=v['request_end_ns'] else 'final_flush' if e['published_ns']<=v['flush_end_ns'] else 'catchup'
  row=dict(run=name,kind=e['kind'],task=e['id'],automatic=e['manual_request']==0,source_level=e['level'],target_level=e['level']+1,source_budget=e['source_budget'],source_bytes=sb,target_bytes=tb,output_bytes=ob,ratio_target_source=tb/sb if sb else 0,input_records=ir,output_records=orr,retained_values=sum(f['records']-f['tombstones'] for f in out),retained_tombstones=sum(f['tombstones'] for f in out),dropped_versions=ir-orr if inp else 0,phase=phase,range_smallest=min(f['smallest'] for f in inp or out),range_largest=max(f['largest'] for f in inp or out),lineage_depth=d,selection_ns=e['selected_ns']-e['start_ns'],build_ns=e['buildEnd_ns']-e['buildStart_ns'],writer_ns=sum(b-a for a,b in e['writer_intervals']),publish_wait_ns=e['locked_ns']-e['wait_ns'],publish_hold_ns=e['unlock_ns']-e['locked_ns'],cleanup_ns=e['end_ns']-e['unlock_ns'])
  rows.append(row)
  for f in out:
   assert f['id'] not in known
   known[f['id']]=f;producer[f['id']]=(e['kind'],e['id']);depth[f['id']]=d
  for f in inp:
   flows.append(dict(run=name,file=f['id'],producer_kind=producer[f['id']][0],producer_task=producer[f['id']][1],consumer_task=e['id'],bytes=f['bytes'],level=f['level'],depth=depth[f['id']],consumer_output_files=';'.join(str(x['id']) for x in out)))
 assert set(known)-consumed=={f['file'] for f in v['final']}
 assert sum(known[i]['bytes'] for i in set(known)-consumed)==v['storage']['bytes']
 summ.append(dict(run=name,config=v['config'],work=v['work'],tasks=sum(e['kind']=='compaction' for e in es),flushes=fl,logical_encoded_bytes=v['logical_encoded_bytes'],sst_output_bytes=outsum,amp=outsum/v['logical_encoded_bytes'],zero_drop_output_bytes=zero,max_lineage_depth=max(depth.values()),output_by_level=dict(by),task_counts=dict(Counter(e['level'] for e in es if e['kind']=='compaction'))))
def csvout(name,items):
 if not items:return
 with (O/name).open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(items[0]));w.writeheader();w.writerows(items)
import csv
csvout('tasks.csv',rows);csvout('file-flow.csv',flows);(O/'mechanism-summary.json').write_text(json.dumps(summ,indent=2))
formal=[]
for p in sorted((O/'runs').glob('*.json')):
 v=json.loads(p.read_text());name=v.get('name','')
 if name.startswith(('f','w')) and 'writes' in v:
  counts=[0]* (7 if v['config']=='C' else 3);bs=counts.copy()
  for t in v['final']:counts[t['level']]+=1;bs[t['level']]+=t['bytes']
  x=dict(run=name,config=v['config'],work=v['work'],auto=v['auto'],wal=v['wal'],n=v['n'],request_ops_s=v['writes']['logical_ops_s'],including_flush_ops_s=v['n']*1e9/v['request_to_flush_ns'],request_ms=v['writes']['wall_ns']/1e6,flush_ms=v['flush_ns']/1e6,catchup_ms=v['converge_ns']/1e6,total_converged_ms=(v['converged_ns']-v['begin_ns'])/1e6,close_ms=v['close_ns']/1e6,rss_mib=v['rss_kib']/1024,final_bytes=v['storage']['bytes'],final_records=v['storage']['records'],level_counts=str(counts),level_bytes=str(bs))
  for q in ['p50','p95','p99','max']:x[q+'_ms']=v['writes'][q+'_ns']/1e6
  formal.append(x)
csvout('formal-rounds.csv',formal)
groups=defaultdict(list)
for x in formal:groups[(x['config'],x['auto'],x['wal'],x['work'])].append(x)
text=['# 正式结果：逐轮见 formal-rounds.csv','', '|配置|自动|WAL|负载|轮数|请求op/s|含Flush op/s|p99批次ms|max批次ms|Flush ms|追赶ms|总收敛ms|关闭ms|RSS MiB|最终文件数|','|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|']
for k,g in sorted(groups.items()):
 med=lambda f:median(x[f] for x in g)
 text.append('|'+ '|'.join(map(str,[*k,len(g),round(med('request_ops_s')),round(med('including_flush_ops_s')),round(med('p99_ms'),3),round(med('max_ms'),3),round(med('flush_ms'),3),round(med('catchup_ms'),3),round(med('total_converged_ms'),3),round(med('close_ms'),3),round(med('rss_mib'),2),g[0]['level_counts']]))+'|')
text+=['','## 独立机制轮（不能以这些耗时替代正式性能）','','|配置|负载|轮数|输出放大中位|范围|任务数中位|零清理输出MiB中位|最大文件流转深度|','|---|---|---:|---:|---|---:|---:|---:|']
mg=defaultdict(list)
for x in summ:mg[x['config'],x['work']].append(x)
for k,g in sorted(mg.items()):text.append('|'+ '|'.join(map(str,[*k,len(g),round(median(x['amp'] for x in g),4),str([round(min(x['amp'] for x in g),4),round(max(x['amp'] for x in g),4)]),median(x['tasks'] for x in g),round(median(x['zero_drop_output_bytes'] for x in g)/1048576,2),max(x['max_lineage_depth'] for x in g)]))+'|')
(O/'SUMMARY.md').write_text('\n'.join(text)+'\n')
print('\n'.join(text),flush=True)