from pathlib import Path
import json,csv,collections,statistics as st
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/diagnostics-20261116-followup'
rows=[json.loads(x) for x in (o/'runs.jsonl').read_text().splitlines()];formal=[x for x in rows if x['name'].startswith('formal')]
req=[];tasks=[];cost=[];sources=[]
for v in formal:
 if not v['query']:req.append({k:x for k,x in v.items() if k not in ['command','query']})
 if v['on']!='on':continue
 stats=json.loads((o/(v['name']+'.stats.json')).read_text())
 for c in stats['cells']:cost.append(dict(run=v['name'],**c))
 for t in stats['tasks']:
  ms={m['metric']:m['ns']/1e6 for m in t['metrics']};tasks.append(dict(run=v['name'],wal=v['wal'],limit=v['limit'],threads=v['threads'],file=t['file'],bytes=t['bytes'],origin=t['origin'],before_release_ms=t['before_release_ns']/1e6,after_release_ms=t['after_release_ns']/1e6,build_ms=ms.get('sst_build',0),writer_ms=ms.get('sst_writer_write_sync_rename_dirsync',0),reader_ms=ms.get('reader_init',0),publish_wait_ms=ms.get('publish_wait',0),manifest_ms=ms.get('manifest_save',0),other_publish_ms=ms.get('publish_hold',0)-ms.get('manifest_save',0),cleanup_ms=ms.get('wal_cleanup',0)))
 if v['query']:
  for phase in [7,9,11]:
   cs=[c for c in stats['cells'] if c['phase']==phase and c['op']==3]
   def agg(m,f='count'):return sum(c[f] for c in cs if c['metric']==m)
   sources.append(dict(run=v['name'],phase=phase,found=agg('bloom_pass_found'),not_found=agg('bloom_pass_not_found'),blocks_found=agg('found_table_blocks','units'),blocks_not_found=agg('not_found_table_blocks','units'),cross_extra=agg('same_table_extra_blocks','units'),blocks=agg('blocks'),bytes=agg('block_bytes_with_crc','units'),crc_sample_us=agg('crc','ns')/max(agg('crc','timed_count'),1)/1000))
for name,data in [('requests.csv',req),('tasks.csv',tasks),('costs.csv',cost),('read-sources.csv',sources)]:
 if data:
  with (o/name).open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=list(data[0]));w.writeheader();w.writerows(data)
g=collections.defaultdict(list)
for v in req:g[(v['wal'],v['limit'],v['threads'],v['on'])].append(v)
lines=['FOLLOWUP DIAGNOSTIC RESULTS (not optimization)','Median of 3 independent runs; max latency = maximum across runs.','', 'WAL,threshold,threads,diagnostics,runs,req_op/s,total_op/s,p50_us,p95_us,p99_us,max_us,request_s,flush_s,close_s,rss_KiB,lock_wait_ms,sync_ms,backpressure_ms,automatic_flushes']
for k,vs in sorted(g.items()):
 def med(f):return st.median(v[f] for v in vs)
 extra=[]
 for m in ['first_lock','wal_sync','backpressure']:
  extra.append(st.median(sum(c['ns'] for c in cost if c['run']==v['name'] and c['phase']==1 and c['op']==1 and c['metric']==m)/1e6 for v in vs))
 lines.append(','.join(map(str,[*k,len(vs),*[round(med(f),5) for f in ['request_ops','total_ops','p50_us','p95_us','p99_us']],round(max(v['max_us'] for v in vs),3),*[round(med(f),6) for f in ['request_seconds','flush_seconds','close_seconds','rss_kib']],*[round(x,4) for x in extra],med('automatic_flushes') if k[3]=='on' else 'NA'])))
lines+=['','BACKGROUND TASKS (all origins including final flush; per task mean across each run then median across runs):']
for limit in [65536,1048576]:
 vs=[v for v in req if v['wal']==0 and v['limit']==limit]
 names=['build_ms','writer_ms','reader_ms','publish_wait_ms','manifest_ms','other_publish_ms','cleanup_ms','before_release_ms','after_release_ms']
 lines.append('limit='+str(limit)+' tasks/run='+str([len([t for t in tasks if t['run']==v['name']]) for v in vs])+' bytes/run='+str([sum(t['bytes'] for t in tasks if t['run']==v['name']) for v in vs]))
 for name in names:
  vals=[st.mean(t[name] for t in tasks if t['run']==v['name']) for v in vs];lines.append(name+'='+str(st.median(vals)))
lines+=['','QUERY SOURCE COUNTS:']
for phase in [7,9,11]:
 vs=[v for v in sources if v['phase']==phase]
 if vs:lines.append('phase='+str(phase)+' '+str({k:st.median(v[k] for v in vs) for k in vs[0] if k not in ['run','phase']}))
lines+=['','CRC standalone:']
crc=[]
for p in o.glob('crc-r*.jsonl'):crc.extend(json.loads(x) for x in p.read_text().splitlines())
for size in sorted({v['bytes'] for v in crc}):
 vs=[v for v in crc if v['bytes']==size];lines.append(str(size)+' bytes: '+str({k:st.median(v[k] for v in vs) for k in ['us_per_block','MiB_per_second']}))
(o/'summary.txt').write_text('\n'.join(lines)+'\n');print('\n'.join(lines))
