# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from run import R,O
import json,struct,csv,statistics as st
vs=[json.loads(p.read_text()) for p in (O/'runs').glob('*.json')]
rows=[]
for p in sorted((O/'db').glob('m*')):
 v=json.loads((O/'runs'/(p.name+'.json')).read_text());charge=0;blocks=0
 for meta in v['final']:
  f=p/f"{meta['file']:06d}.sst";data=f.read_bytes();footer=data[-48:-4];idx,sz=struct.unpack_from('<QQ',footer,28);b=data[idx:idx+sz];count=struct.unpack_from('<I',b)[0];pos=4
  for i in range(count):
   n=struct.unpack_from('<I',b,pos)[0];pos+=4+n+8+1+8;size=struct.unpack_from('<I',b,pos)[0];pos+=4;charge+=size;blocks+=1
 rows.append(dict(run=p.name,data_block_charge_bytes=charge,blocks=blocks,capacity_upper_bound=sum(t['bytes'] for t in v['final'])))
(O/'cache-worksets.json').write_text(json.dumps(rows,indent=2))
lines=['# 补充：轻量收敛与缓存依据','','|负载|自动|轮数|请求op/s中位|Flush后收敛ms中位|范围ms|关闭ms|最终各层文件数|','|---|---:|---:|---:|---:|---|---:|---|']
for w in ['unique','update']:
 for a in [0,1]:
  group=[v for v in vs if v['kind']=='observer' and v['work']==w and v['auto']==a]
  if not group:continue
  med=lambda f:st.median(f(v) for v in group)
  lines.append(f"|{w}|{a}|{len(group)}|{med(lambda v:v['writes']['logical_ops_s']):.0f}|{med(lambda v:v['converge_ns'])/1e6:.3f}|{min(v['converge_ns'] for v in group)/1e6:.3f}–{max(v['converge_ns'] for v in group)/1e6:.3f}|{med(lambda v:v['close_ns'])/1e6:.3f}|"+str([med(lambda v:sum(t['level']==l for t in v['final'])) for l in range(3)])+'|')
lines+=['','详细机制轮额外遍历记录，轻量observer不遍历，仅在领取与完成时维护busy并通知。因此以上收敛性能更接近原执行路径，但仍是隔离观察版、不同独立轮次，不机械从详细机制时间扣减。','', '|冻结布局|数据块数|全部块charge字节|足量容量字节|','|---|---:|---:|---:|']
for r in rows:lines.append(f"|{r['run']}|{r['blocks']}|{r['data_block_charge_bytes']}|{r['capacity_upper_bound']}|")
lines+=['','容量>全部块charge（SST索引逐项汇总），故可容纳所有访问块；Usage为实际加载块charge。没有按OS缓存或Bloom拒绝估算命中率。','','## 查询机制（独立轮，live两轮共8192请求）','','|布局自动|缓存|Reader候选总数|每请求候选数|','|---:|---:|---:|---:|']
for v in sorted(vs,key=lambda x:x['name']):
 if v['name'].startswith('query-mechanism'):lines.append(f"|{v['auto']}|{v['cache_capacity']>0}|{v['candidate_tables']}|{v['candidate_tables']/8192:.3f}|")
(O/'SUPPLEMENT.md').write_text('\n'.join(lines)+'\n');print('\n'.join(lines[:12]))