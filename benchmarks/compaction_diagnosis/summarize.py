# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,statistics,collections,csv,subprocess,hashlib,shutil
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/compaction-diagnosis-20261125';med=statistics.median
runs=[json.loads(f.read_text()) for f in (O/'runs').glob('*.json') if not f.name.endswith('.probe.json')]
L=['# Compaction 定向诊断','', '## 现象复现（正式轮：诊断关闭）','', '|场景|侧|轮数|op/s中位|min–max|p50/p95/p99/max µs中位|','|---|---|---:|---:|---|---|']
for mode in ['hot','impact']:
 for side in [0,1]:
  vs=[v for v in runs if v['name'].startswith('r') and v['kind']=='plain' and v['side']==side and v['mode']==mode];cs=[v['passes'][1] if mode=='hot' else v['requests'] for v in vs];rates=[c['ops_s']*(64 if mode=='impact' else 1) for c in cs];lat='/'.join(f"{med(c[k] for c in cs)/1000:.2f}" for k in ['p50_ns','p95_ns','p99_ns','max_ns']);L.append(f"|{mode}|{side}|{len(vs)}|{med(rates):.0f}|{min(rates):.0f}–{max(rates):.0f}|{lat}|")
L+=['','## 热点机制（独立机制轮）','', 'side0合并前，side1合并后；phase1填充，phase2稳定。计数全量，索引/cache/Get计时每32请求抽样一次（稳定256请求）；同请求下所有候选块均抽样，不按块独立随机。表检查数含Bloom拒绝。时间是抽样平均，不外推精确总耗时。','', '|热点集|侧|每请求表数|Bloom拒绝/请求|放行/请求|缓存命中/未命中|实际加载|扫描记录/请求|索引ns/抽样调用|缓存ns/抽样调用|块内Get ns/抽样调用|稳定Usage B|','|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|']
agg=[]
for v in sorted(runs,key=lambda x:x['name']):
 if v['kind']!='probe' or v['mode']!='hot' or v['name'].startswith('pilot'):continue
 p=json.loads((O/'runs'/(v['name']+'.probe.json')).read_text());cells={c['metric']:c for c in p['cells'] if c['phase']==2};u=lambda m:cells.get(m,{}).get('units',0);n=lambda m:cells.get(m,{}).get('count',0);t=lambda m:cells.get(m,{}).get('ns',0)/max(1,cells.get(m,{}).get('timed',0));usage=u('cache_usage')/max(1,n('cache_usage'));L.append(f"|{v['variant']} ({v['name']})|{v['side']}|{u('tables')/8192:.3f}|{u('bloom_rejects')/8192:.3f}|{(u('bloom_checks')-u('bloom_rejects'))/8192:.3f}|{u('cache_hits')}/{u('cache_misses')}|{u('block_load')}|{u('scanned')/8192:.3f}|{t('index'):.1f}|{t('cache_get'):.1f}|{t('block_get'):.1f}|{usage:.0f}|")
 agg.append({'run':v['name'],'phase':2,'cells':cells})
(O/'hot-mechanism.json').write_text(json.dumps(agg,indent=2))
L+=['','## 写入时间线（机制轮，不能代替正式性能）','', '|轮|最大批次ms|队列等待总ms|首次DB锁总ms|背压次数/总ms|轮换数|Add总ms|合并构建含Writer ms|输出Writer ms|发布等待/持锁/Save ms|清理ms|刷盘任务数|','|---|---:|---:|---:|---|---:|---:|---:|---:|---|---:|---:|']
with (O/'write-events.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['run','slot','phase','metric','start_steady_ns','end_steady_ns','duration_ns'])
 for v in sorted(runs,key=lambda x:x['name']):
  if v['kind']!='probe' or v['mode']!='impact':continue
  p=json.loads((O/'runs'/(v['name']+'.probe.json')).read_text());cs=collections.defaultdict(lambda:{'ns':0,'count':0,'units':0})
  for c in p['cells']:
   if c['phase'] not in [3,6]:continue
   for k in ['ns','count','units']:cs[c['metric']][k]+=c[k]
  ms=lambda m:cs[m]['ns']/1e6
  for e in p['events']:w.writerow([v['name'],e['slot'],e['phase'],e['metric'],e['start_ns'],e['end_ns'],e['end_ns']-e['start_ns']])
  L.append(f"|{v['name']}|{v['requests']['max_ns']/1e6:.3f}|{ms('queue_cv')+ms('queue_lock'):.3f}|{ms('first_db_lock'):.3f}|{cs['backpressure_cv']['count']}/{ms('backpressure_cv'):.3f}|{cs['rotate']['count']}|{ms('memtable_add'):.3f}|{ms('merge_and_build_including_writer'):.3f}|{ms('output_write_sync_rename_dirsync'):.3f}|{ms('compact_publish_wait'):.3f}/{ms('compact_publish_hold'):.3f}/{ms('compact_manifest'):.3f}|{ms('old_file_cleanup'):.3f}|{cs['flush_task']['count']}|")
  if v['side']==1:
   es=p['events'];apis=[e for e in es if e['metric']=='write_api' and e['phase']==3];slow=max(apis,key=lambda e:e['end_ns']-e['start_ns']);over=[]
   for e in es:
    overlap=max(0,min(e['end_ns'],slow['end_ns'])-max(e['start_ns'],slow['start_ns']))
    if overlap and e['metric']!='write_api':over.append({**e,'overlap_ns':overlap})
   (O/'runs'/(v['name']+'-slow-overlap.json')).write_text(json.dumps({'slow_api':slow,'overlapping_events':over},indent=2))
L+=['','## 诊断开销配对（同轮、同配置）','', '|轮/场景/侧|off op/s|on op/s|on/off|off最大ms|on最大ms|','|---|---:|---:|---:|---:|---:|']
for v in sorted(runs,key=lambda x:x['name']):
 if not v['name'].startswith(('r0-','r1-','r2-')) or v['kind']!='plain':continue
 name=v['name'].replace('-plain','-probe');p=next(x for x in runs if x['name']==name);a=v['passes'][1] if v['mode']=='hot' else v['requests'];b=p['passes'][1] if p['mode']=='hot' else p['requests'];L.append(f"|{v['name']}|{a['ops_s']:.0f}|{b['ops_s']:.0f}|{b['ops_s']/a['ops_s']:.3f}|{a['max_ns']/1e6:.3f}|{b['max_ns']/1e6:.3f}|")
L+=['','## 原始逐轮摘要','', '|run|稳定/请求op/s|最大µs|','|---|---:|---:|']
for v in sorted(runs,key=lambda x:x['name']):
 if v['mode'] not in ['hot','impact']:continue
 c=v['passes'][1] if v['mode']=='hot' else v['requests'];L.append(f"|{v['name']}|{c['ops_s']:.0f}|{c['max_ns']/1000:.2f}|")
(O/'SUMMARY.md').write_text('\n'.join(L)+'\n')
# Exact patch of independent variant, including whitespace normalization.
p=subprocess.run(['diff','-ruN',str(O/'original'),str(O/'instrumented')],stdout=subprocess.PIPE);(O/'instrumentation.patch').write_bytes(p.stdout)
assert subprocess.check_output(['git','diff','--binary'],cwd=R)==(O/'worktree-before.patch').read_bytes()
h=json.loads((O/'hashes.json').read_text());assert all(hashlib.sha256((R/p).read_bytes()).hexdigest()==v for p,v in h.items());(O/'preservation.json').write_text(json.dumps({'engine_hashes_unchanged':True,'original_tracked_patch_unchanged':True},indent=2))
shutil.copytree(R/'benchmarks/compaction_diagnosis',O/'tool-snapshot',dirs_exist_ok=True)
print('summary ready')
