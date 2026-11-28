# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import R,O
import json,csv,statistics as st,hashlib,subprocess,shutil
T=R/'benchmarks/auto_compaction'
vs=[json.loads(p.read_text()) for p in sorted((O/'runs').glob('*.json'))]
assert all(v.get('verified') for v in vs)
med=lambda rows,f:st.median(f(v) for v in rows)
lines=['# 分层自动Compaction性能评估','','Release C++17，正式LSM_DIAGNOSTICS=OFF；开发容量配置，非生产外推。所有轮次含慢轮均保留。','', '## 在线写入（逐组中位）','','|负载|WAL|自动|轮数|逻辑op/s|含Flush op/s|API p50/p95/p99/max ms|Flush ms|close ms|RSS MiB|最终L0/L1/L2文件数|','|---|---:|---:|---:|---:|---:|---|---:|---:|---:|---|']
for work in ['unique','update']:
 for w in [0,1]:
  for a in [0,1]:
   rows=[v for v in vs if v['name'].startswith('r') and v['work']==work and v['wal']==w and v['auto']==a]
   if not rows:continue
   lat='/'.join(f"{med(rows,lambda v:v['writes'][k+'_ns'])/1e6:.3f}" for k in ['p50','p95','p99','max'])
   counts='/'.join(f"{med(rows,lambda v:sum(t['level']==l for t in v['final'])):.0f}" for l in range(3))
   lines.append(f"|{work}|{w}|{a}|{len(rows)}|{med(rows,lambda v:v['writes']['logical_ops_s']):.0f}|{med(rows,lambda v:v['logical_ops']*1e9/v['request_to_flush_ns']):.0f}|{lat}|{med(rows,lambda v:v['flush_ns'])/1e6:.3f}|{med(rows,lambda v:v['close_ns'])/1e6:.3f}|{med(rows,lambda v:v['rss_kib'])/1024:.2f}|{counts}|")
lines+=['','## 收敛机制轮（含额外记录遍历，不是正式性能）','','|负载|自动|轮|L0→L1/L1→L2次数|写入期间/之后发布|Flush次数/字节|Comp输出字节|逻辑编码字节|SST输出放大|背压次数/总ms|Flush后收敛ms|各层文件数|各层字节|记录数|','|---|---:|---:|---|---|---|---:|---:|---:|---|---:|---|---|---:|']
mechanisms=[]
for v in vs:
 if not v['name'].startswith('m'):continue
 tasks=list(csv.DictReader((O/'runs'/(v['name']+'-tasks.csv')).open()));counts=[sum(int(t['level'])==l for t in tasks) for l in [0,1]];during=sum(int(t['published_ns'])<=v['request_end_ns'] for t in tasks);co=sum(int(t['output_bytes']) for t in tasks);m=v['mechanism'];amp=(m['flush_bytes']+co)/v['logical_encoded_bytes'];fc=[sum(t['level']==l for t in v['final']) for l in range(3)];bs=[sum(t['bytes'] for t in v['final'] if t['level']==l) for l in range(3)]
 mechanisms.append(dict(run=v['name'],tasks=counts,during=during,after=len(tasks)-during,flush_bytes=m['flush_bytes'],compaction_bytes=co,logical_bytes=v['logical_encoded_bytes'],amplification=amp,level_files=fc,level_bytes=bs,records=v['storage']['records']))
 lines.append(f"|{v['work']}|{v['auto']}|{v['name']}|{counts}|{during}/{len(tasks)-during}|{m['flush_count']}/{m['flush_bytes']}|{co}|{v['logical_encoded_bytes']}|{amp:.3f}|{m['backpressure_count']}/{m['backpressure_ns']/1e6:.3f}|{v['converge_ns']/1e6:.3f}|{fc}|{bs}|{v['storage']['records']}|")
(O/'mechanism-summary.json').write_text(json.dumps(mechanisms,indent=2))
lines+=['','## 静态查询（收敛轮冻结布局；三轮中位）','','|写入布局|查询|缓存|自动形成布局|轮|容量/Usage MiB|首轮op/s|稳定op/s|稳定p50/p95/p99/max µs|','|---|---|---:|---:|---:|---|---:|---:|---|']
for work in ['unique','update']:
 for load in ['live','missing','deleted','hot']:
  for cache in [0,1]:
   for a in [0,1]:
    rows=[v for v in vs if v['name'].startswith('q') and not v['name'].startswith('query') and f'-{work}-{load}-c{cache}-a{a}' in v['name']]
    if not rows:continue
    lat='/'.join(f"{med(rows,lambda v:v['steady'][k+'_ns'])/1000:.3f}" for k in ['p50','p95','p99','max'])
    lines.append(f"|{work}|{load}|{cache}|{a}|{len(rows)}|{med(rows,lambda v:v['cache_capacity'])/1048576:.2f}/{med(rows,lambda v:v['usage_steady'])/1048576:.2f}|{med(rows,lambda v:v['first']['api_s']):.0f}|{med(rows,lambda v:v['steady']['api_s']):.0f}|{lat}|")
lines+=['','## 并行读写（三轮中位）','','|自动|写op/s|批次p95/p99/max ms|读op/s|读p95/p99/max µs|读实际时长ms|完成读数|Flush ms|close ms|','|---:|---:|---|---:|---|---:|---:|---:|---:|']
for a in [0,1]:
 rows=[v for v in vs if v['name'].startswith('x') and v['auto']==a]
 if not rows:continue
 wl='/'.join(f"{med(rows,lambda v:v['writes'][k+'_ns'])/1e6:.3f}" for k in ['p95','p99','max']);rl='/'.join(f"{med(rows,lambda v:v['reads'][k+'_ns'])/1e3:.3f}" for k in ['p95','p99','max'])
 lines.append(f"|{a}|{med(rows,lambda v:v['writes']['logical_ops_s']):.0f}|{wl}|{med(rows,lambda v:v['reads']['api_s']):.0f}|{rl}|{med(rows,lambda v:v['reads']['wall_ns'])/1e6:.3f}|{med(rows,lambda v:v['reads']['count']):.0f}|{med(rows,lambda v:v['flush_ns'])/1e6:.3f}|{med(rows,lambda v:v['close_ns'])/1e6:.3f}|")
lines+=['','## 逐轮索引','','|运行|验证|写/查询op/s|API max ms|Flush ms|close ms|RSS MiB|','|---|---|---:|---:|---:|---:|---:|']
for v in vs:
 q=v.get('writes',v.get('steady',{}));lines.append(f"|{v['name']}|{v['verified']}|{q.get('logical_ops_s',q.get('api_s',0)):.0f}|{q.get('max_ns',0)/1e6:.3f}|{v.get('flush_ns',0)/1e6:.3f}|{v.get('close_ns',0)/1e6:.3f}|{v['rss_kib']/1024:.2f}|")
(O/'SUMMARY.md').write_text('\n'.join(lines)+'\n')
# Write phase table, based on batch order quartiles: elapsed wall, throughput, percentiles, no extra engine work.
with (O/'write-phases.csv').open('w') as f:
 wr=csv.writer(f);wr.writerow(['run','quartile','batches','logical_ops_s','p95_ns','p99_ns','max_ns'])
 for v in vs:
  if 'writes' not in v:continue
  data=list(csv.DictReader((O/'runs'/(v['name']+'-writes.csv')).open()));size=len(data)//4
  for i in range(4):
   seg=data[i*size:(i+1)*size];lat=sorted(int(x['elapsed_ns']) for x in seg);wall=int(seg[-1]['end_ns'])-int(seg[0]['start_ns']);wr.writerow([v['name'],i,len(seg),len(seg)*64*1e9/wall,lat[int(.95*(len(lat)-1))],lat[int(.99*(len(lat)-1))],max(lat)])
print('runs',len(vs))
if (O/'FINDINGS.md').exists():
 with (O/'SUMMARY.md').open('a') as f:f.write('\n'+(O/'FINDINGS.md').read_text())
