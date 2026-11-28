# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,statistics,collections,csv,hashlib,subprocess,shutil
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/compaction-acceptance-20261125';runs=[json.loads(p.read_text()) for p in (O/'runs').glob('*.json')];med=statistics.median
lines=['# 手动全量 Compaction 验收','', '18/18 LSM 测试通过。四类夹具各 65536 逻辑键；种子 20261115，16 B key、128 B value，Release -O3 -DNDEBUG、C++17、诊断 OFF、WSL ext4。未改引擎、未提交。主体三轮；multi-64查询及拆分追加两轮，相关行为五轮中位数，逐轮值在 runs/*.json；逐请求记录在 CSV。','', '## 测量边界','', '夹具：unique 每键只写一次，随机打乱后分入16/64张表；multi 每键四个版本，每轮重新打乱分片，最后版本 i%5==0 为删除，共13108个删除键。每表覆盖广泛重叠范围。每张表显式 Flush，创建时阈值1 GiB以固定表数；性能轮 memTableSizeLimit=1 MiB，WAL off，默认 Group Commit。各轮独立复制目录，复制/准备/校验不计时。','', '合并成本：API含排队构建发布；返回立即销毁，另记清理/join。RSS在关闭后、最终校验前读取，含oracle、程序、输入Reader等，不是Builder精确内存。成本基线也加载oracle并扫描输入，保持相同前置条件。所有合并轮检查物理最新记录唯一、sequence、删除标记、FlushedSequence、旧文件清理，以及禁用WAL重开全量查询。','', '查询：独立无缓存Reader遍历预热全部输入文件，关闭后新DB/新缓存；每类查询均独立DB。pass0首次trace、pass1重复稳定trace，各8192请求。操作系统预热、引擎缓存初始为空，不清系统缓存。Get结果留存后在计时外校验。容量40531456 B，高于所有输入块计费总和40527360 B；见 capacities.json。固定热点为排序后前32个存活键；missing为偶数编码键的间隙，范围内不存在。足量不表示所有块已在有限trace中填充，DB无Usage公开接口，本轮不声称实际命中率。','', '并发：单前台读线程131072次，或单写线程256次Write×64条=16384条独立新key，WAL off。预构造batch不计时。完成512次读/4次写后发Compact；控制组用成本轮API中位数标记窗口，工作量相同。窗口是公共调用区间，不是精确后台区间，不能强算因果百分比。写Flush在worker完成后执行，关闭必须等待所有前台调用结束。read-v1含oracle比较，保留但不用；read-v2仅Get在API计时内，结果留存计时外校验。','', '## 空间与记录','', '|夹具|输入表→输出表|记录数前→后|删除记录前→后|文件MiB前→后|记录编码MiB前→后|非记录文件开销MiB前→后|','|---|---|---|---|---|---|---|']
for label in ['unique-16','unique-64','multi-16','multi-64']:
 a=next(v for v in runs if v.get('kind')=='cost' and v['fixture']==label and v['run']);b,c=a['before'],a['after']
 lines.append(f"|{label}|{b['tables']}→{c['tables']}|{b['records']}→{c['records']}|{b['tombstones']}→{c['tombstones']}|{b['bytes']/2**20:.3f}→{c['bytes']/2**20:.3f}|{b['encoded']/2**20:.3f}→{c['encoded']/2**20:.3f}|{(b['bytes']-b['encoded'])/2**20:.3f}→{(c['bytes']-c['encoded'])/2**20:.3f}|")
lines+=['','## 合并耗时与内存','', '|夹具/目标|Compact ms|关闭 ms|总 ms|合并进程峰值MiB|不合并峰值MiB|','|---|---:|---:|---:|---:|---:|']
for label in ['unique-16','unique-64','multi-16','multi-64']:
 vs=[v for v in runs if v.get('kind')=='cost' and v['fixture']==label and v['run']];bs=[v for v in runs if v.get('kind')=='cost' and v['fixture']==label and not v['run']]
 lines.append(f"|{label}/4MiB|{med(v['api_ns'] for v in vs)/1e6:.3f}|{med(v['close_ns'] for v in vs)/1e6:.3f}|{med(v['total_ns'] for v in vs)/1e6:.3f}|{med(v['rss_kib'] for v in vs)/1024:.2f}|{med(v['rss_kib'] for v in bs)/1024:.2f}|")
vs=[v for v in runs if v.get('kind')=='split'];lines.append(f"|multi-64/1MiB ({vs[0]['after']['tables']}表)|{med(v['api_ns'] for v in vs)/1e6:.3f}|{med(v['close_ns'] for v in vs)/1e6:.3f}|{med(v['total_ns'] for v in vs)/1e6:.3f}|{med(v['rss_kib'] for v in vs)/1024:.2f}|—|")
lines+=['','## 查询：中位数（主体三轮，multi-64五轮；括号为吞吐min–max）','', '|夹具|缓存B|查询/pass|前op/s|min–max|后op/s|min–max|吞吐比后/前|前p50/p95/p99/max µs|后p50/p95/p99/max µs|','|---|---:|---|---:|---|---:|---|---:|---|---|']
q=collections.defaultdict(list)
for v in runs:
 if v.get('kind')=='query':
  for c in v['cases']:q[(v['fixture'],v['capacity'],c['case'],v['side'])].append(c)
for label,cap,case,side in sorted(q):
 if side!='before':continue
 a=q[label,cap,case,'before'];b=q[label,cap,case,'after']
 lat=lambda z:'/'.join(f"{med(v[k] for v in z)/1e3:.2f}" for k in ['p50_ns','p95_ns','p99_ns','max_ns'])
 throughput=lambda z:med(v['ops_s'] for v in z)
 span=lambda z:f"{min(v['ops_s'] for v in z):.0f}–{max(v['ops_s'] for v in z):.0f}"
 lines.append(f"|{label}|{cap}|{case}|{throughput(a):.0f}|{span(a)}|{throughput(b):.0f}|{span(b)}|{throughput(b)/throughput(a):.2f}×|{lat(a)}|{lat(b)}|")
lines+=['','## 合并期间影响（三轮中位数）','', '|前台|合并|请求op/s|p50/p95/p99/max µs|Flush ms|关闭ms|总ms|调用窗口ms|','|---|---:|---:|---|---:|---:|---:|---:|']
for writing in [0,1]:
 for run in [0,1]:
  vs=[v for v in runs if v.get('kind')==('impact' if writing else 'impact-v2') and v['writing']==writing and v['run']==run]
  if not vs:continue
  lat='/'.join(f"{med(v['requests'][k] for v in vs)/1e3:.2f}" for k in ['p50_ns','p95_ns','p99_ns','max_ns'])
  rate=med(v['requests']['ops_s'] for v in vs)*(64 if writing else 1)
  lines.append(f"|{'写逻辑操作' if writing else '读'}|{run}|{rate:.0f}|{lat}|{med(v['flush_ns'] for v in vs)/1e6:.2f}|{med(v['close_ns'] for v in vs)/1e6:.2f}|{med(v['total_ns'] for v in vs)/1e6:.2f}|{med(v['compact_window_ns'] for v in vs)/1e6:.2f}|")
# Window samples grouped by request start; crossing operations remain identified by timeline.
lines+=['','窗口分段按请求开始时间归类；跨窗口请求保留在原时间线，见 impact-windows.csv。控制窗口为人为标记，不表示后台执行。']
with (O/'impact-windows.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['run','phase','count','p50_us','p95_us','p99_us','max_us','crosses_boundary'])
 for v in runs:
  if v.get('kind') not in ['impact-v2','impact'] or (v.get('kind')=='impact' and not v['writing']):continue
  a=v['window_start_ns'];b=v['window_end_ns'];ph=collections.defaultdict(list);cross=collections.Counter()
  with (O/'runs'/(v['name']+'-timeline.csv')).open() as src:
   for z in csv.DictReader(src):
    t=float(z['start_ns']);d=float(z['elapsed_ns']);p='before' if t<a else 'window' if t<b else 'after';ph[p].append(d);cross[p]+=int(t<a<t+d or t<b<t+d)
  for p in ['before','window','after']:
   z=sorted(ph[p]);pct=lambda x:z[min(len(z)-1,int(x*(len(z)-1)))]/1000 if z else 0
   w.writerow([v['name'],p,len(z),pct(.5),pct(.95),pct(.99),pct(1),cross[p]])
lines+=['','## 三轮原始摘要','', '|run|API/请求ms|Flushms|关闭ms|RSS KiB|','|---|---:|---:|---:|---:|']
for v in sorted(runs,key=lambda v:v['name']):
 if v.get('kind') in ['cost','split','impact','impact-v2']:
  ns=v.get('api_ns',v.get('requests',{}).get('wall_ns',0));lines.append(f"|{v['name']}|{ns/1e6:.3f}|{v.get('flush_ns',0)/1e6:.3f}|{v['close_ns']/1e6:.3f}|{v['rss_kib']}|")
(O/'SUMMARY.md').write_text('\n'.join(lines)+'\n')
# Preserve tracked changes exactly; engine includes existing untracked compaction files.
hashes=json.loads((O/'engine-hashes.json').read_text());now={str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((R/'src/lsm').rglob('*')) if p.is_file()};need=hashes==now
patch=subprocess.check_output(['git','diff','--binary'],cwd=R);same=patch==(O/'worktree-before.patch').read_bytes()
(O/'preservation.json').write_text(json.dumps({'engine_hashes_identical':need,'tracked_diff_identical':same},indent=2));assert need and same
(O/'git-status-after.txt').write_bytes(subprocess.check_output(['git','status','--short'],cwd=R))
shutil.copytree(R/'benchmarks/compaction_acceptance',O/'tool-snapshot',dirs_exist_ok=True)
(O/'tool-hashes.json').write_text(json.dumps({str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((R/'benchmarks/compaction_acceptance').glob('*')) if p.is_file()},indent=2))
with (O/'tools.patch').open('wb') as f:
 for p in sorted((R/'benchmarks/compaction_acceptance').glob('*')):
  if p.is_file():f.write(subprocess.run(['git','diff','--no-index','--','/dev/null',str(p)],stdout=subprocess.PIPE).stdout)
print('Summary written; source preservation',need,same)
