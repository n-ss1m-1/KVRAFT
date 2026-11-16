import json,collections,statistics,csv,subprocess,hashlib,shutil
from pathlib import Path
r=Path('/home/truth/Code/my_kvraft');out=r/'benchmarks/results/diagnostics-20261116-first'
rows=[json.loads(x) for x in (out/'runs.jsonl').read_text().splitlines()]
summary=[];costs=[]
for row in rows:
 if not row['name'].startswith('close-boundary'):continue
 cells=json.loads((out/(row['name']+'.stats.json')).read_text())['cells']
 for v in row['results']:
  summary.append(dict(run=row['name'],wal=row['wal'],limit=row['limit'],tables=row['tables'],peak_rss_kib=row['peak_rss_kib'],flush_seconds=row['final_flush_seconds'],total_with_flush_seconds=row['total_with_flush_seconds'],close_seconds=row['close_seconds'],**v))
 for c in cells:
  costs.append(dict(run=row['name'],**c,mean_timed_us=c['ns']/max(c.get('timed_count',c['count']),1)/1000))
for name,data in [('request-summary.csv',summary),('stage-costs.csv',costs)]:
 if data:
  with (out/name).open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=list(data[0]));w.writeheader();w.writerows(data)
g=collections.defaultdict(list)
for row in rows:
 if row['name'].startswith('close-boundary'):g[(row['mode'],row['wal'],row['limit'],row['tables'])].append(row)
text=['# 首批诊断结果（未扩展并发）','','各指标取三轮中位数；最大请求停顿取三轮最大。诊断块计时约 1/8 采样，次数为全量。','','| mode | WAL | limit | SST | case | N | op/s | p50 us | p95 us | p99 us | max us | request s | Flush s | total s | close s | VmHWM KiB |','| --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
for k,rs in sorted(g.items()):
 for name in [v['name'] for v in rs[0]['results']]:
  vals=[next(v for v in row['results'] if v['name']==name) for row in rs]
  def med(field):return statistics.median(v[field] for v in vals)
  def outer(field):return statistics.median(v[field] for v in rs)
  text.append(f'| {k[0]} | {k[1]} | {k[2]} | {k[3]} | {name} | {vals[0]["operations"]} | {med("ops_per_second"):.1f} | {med("p50_us"):.3f} | {med("p95_us"):.3f} | {med("p99_us"):.3f} | {max(v["max_us"] for v in vals):.3f} | {med("seconds"):.5f} | {outer("final_flush_seconds"):.5f} | {outer("total_with_flush_seconds"):.5f} | {outer("close_seconds"):.6f} | {outer("peak_rss_kib"):.0f} |')
text+=['','## 代表写入分阶段（全量计时）','','| WAL | threshold | context | metric | calls | total ms | mean us | max us |','| --- | ---: | --- | --- | ---: | ---: | ---: | ---: |']
metrics=['first_lock','backpressure','wal_append','wal_sync','memtable_add','rotate','create_wal','new_wal_sync','directory_sync','sst_build','sst_writer_write_sync_rename_dirsync','publish_wait','publish_hold','manifest_save','wal_cleanup','flush_cv_wait','join']
for k,rs in sorted(g.items()):
 if k[0]!='write':continue
 for phase,op in [(1,1),(1,5),(2,4),(2,5),(5,0)]:
  for m in metrics:
   samples=[]
   for row in rs:
    cs=[c for c in costs if c['run']==row['name'] and c['phase']==phase and c['op']==op and c['metric']==m]
    if cs:samples.append((sum(c['count'] for c in cs),sum(c['ns'] for c in cs),max(c['max_ns'] for c in cs)))
   if samples:
    text.append(f'| {k[1]} | {k[2]} | phase={phase},op={op} | {m} | {statistics.median(v[0] for v in samples):.0f} | {statistics.median(v[1] for v in samples)/1e6:.3f} | {statistics.median(v[1]/v[0] for v in samples)/1000:.3f} | {max(v[2] for v in samples)/1000:.3f} |')
text+=['','## 预热查询每请求及每块成本','','块时间是实际计时样本均值，不是精确累计占比；后台/前台、父子阶段不可相加。','','| SST | case | tables/request | Bloom reject/request | blocks/request | bytes/request | read block us/sample | pread us/call sample | CRC us/sample | validate us/sample | block get us/sample | validated records/block | scanned records/block |','| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
for k,rs in sorted(g.items()):
 if k[0]!='query':continue
 for phase,name in [(7,'latest_warm'),(9,'oldest_warm'),(11,'missing_warm')]:
  samples=[]
  for row in rs:
   cs=[c for c in costs if c['run']==row['name'] and c['phase']==phase and c['op']==3]
   def agg(m,f):return sum(c[f] for c in cs if c['metric']==m)
   n=row['n'];blocks=agg('blocks','count')
   vs=[agg(m,'count')/n for m in ['tables_checked','bloom_rejects','blocks']]+[agg('block_bytes_with_crc','units')/n]
   vs += [agg(m,'ns')/max(agg(m,'timed_count'),1)/1000 for m in ['read_block','pread_all','crc','block_construct_validate','block_get']]
   vs += [agg(m,'units')/max(blocks,1) for m in ['validated_records','scanned_records']]
   samples.append(vs)
  v=[statistics.median(x[i] for x in samples) for i in range(11)]
  text.append('| '+str(k[3])+' | '+name+' | '+' | '.join(f'{x:.3f}' for x in v)+' |')
(out/'SUMMARY.md').write_text('\n'.join(text)+'\n')
print('representative runs',sum(len(v) for v in g.values()))
