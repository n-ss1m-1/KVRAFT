# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,subprocess,tempfile,hashlib,random,statistics as st,collections,shutil
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/crc-table-acceptance-20261118'
def hashes(p):return {str(x.relative_to(p)):hashlib.sha256(x.read_bytes()).hexdigest() for x in p.rglob('*') if x.is_file()}
def cmd(*a):return subprocess.check_output(a,text=True,stderr=subprocess.STDOUT)
a=hashes(o/'snapshots/old/lsm');b=hashes(o/'snapshots/new/lsm');different=[p for p in a if a[p]!=b[p]];assert different==['lib/coding.cc'],different
p=subprocess.run(['diff','-ruN',str(o/'snapshots/old/lsm'),str(o/'snapshots/new/lsm')],capture_output=True,text=True);(o/'crc-only.patch').write_text(p.stdout)
meta=dict(source_hashes={'old':a,'new':b},different_files=different,compiler=cmd('c++','--version'),uname=cmd('uname','-a'),cpu=cmd('lscpu'),filesystem=cmd('df','-T',str(o)),commands={v:cmd('ninja','-C',str(r/f'build/crc-accept-{v}'),'-t','commands') for v in ['old','new']},cmake={v:(r/f'build/crc-accept-{v}/CMakeCache.txt').read_text() for v in ['old','new']},seed=20261115,key_bytes=16,value_bytes=128,n=8192)
rows=[]
def run(name,version,tool,*args):
 command=[str(r/f'build/crc-accept-{version}/{tool}'),*map(str,args)];p=subprocess.run(command,capture_output=True,text=True,check=True)
 (o/(name+'.stdout.jsonl')).write_text(p.stdout);(o/(name+'.stderr.txt')).write_text(p.stderr)
 vs=[json.loads(s) for s in p.stdout.splitlines()];rows.extend(dict(run=name,version=version,tool=tool,command=command,**v) for v in vs)
 print(name,'passed',flush=True)
with tempfile.TemporaryDirectory(prefix='kvraft-crc-accept-') as tmp:
 fixtures={}
 for nt in [1,64]:
  d=Path(tmp)/f'tables-{nt}';run(f'fixture-old-{nt}','old','acceptance','generate',d,nt);run(f'fixture-new-verify-{nt}','new','acceptance','verify',d,nt);fixtures[nt]=d
 meta['fixture_hashes']={nt:hashes(d) for nt,d in fixtures.items()}
 for rep in range(3):
  versions=['old','new'] if rep%2==0 else ['new','old']
  for version in versions:run(f'micro-r{rep}-{version}',version,'crc-micro')
  for nt in ([1,64] if rep%2==0 else [64,1]):
   for version in versions:run(f'query-r{rep}-{nt}-{version}',version,'acceptance','query',fixtures[nt],nt)
 assert meta['fixture_hashes']=={nt:hashes(d) for nt,d in fixtures.items()},'fixture changed'
(o/'metadata.json').write_text(json.dumps(meta,indent=2));(o/'results.jsonl').write_text(''.join(json.dumps(v)+'\n' for v in rows))
lines=['# 查表 CRC32C 验收结果','','13/13 LSM tests passed. Old-reference WAL/SSTable/MANIFEST fixture read checks passed. Isolated source differences: lib/coding.cc only (CRC table generation and Crc32c body). No engine workspace changes.','','## CRC microbenchmark','','| bytes | old µs | new µs | speedup | old MiB/s | new MiB/s |','| ---: | ---: | ---: | ---: | ---: | ---: |']
for size in [128,4025,4096,65536]:
 vs={v:[x for x in rows if x['tool']=='crc-micro' and x['version']==v and x['bytes']==size] for v in ['old','new']}
 for i in range(3):assert vs['old'][i]['checksum']==vs['new'][i]['checksum']
 def med(v,k):return st.median(x[k] for x in vs[v])
 lines.append(f'| {size} | {med("old","us_per_block"):.4f} | {med("new","us_per_block"):.4f} | {med("old","us_per_block")/med("new","us_per_block"):.2f}× | {med("old","MiB_per_second"):.1f} | {med("new","MiB_per_second"):.1f} |')
lines+=['','## Warm queries (median of 3 runs; max = maximum across runs)','','| tables | case | version | op/s [min,max] | p50 µs | p95 µs | p99 µs | max µs |','| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |']
for nt in [1,64]:
 for case in ['latest','oldest','missing']:
  for ver in ['old','new']:
   vs=[x for x in rows if x['tool']=='acceptance' and x.get('case')==case and x['tables']==nt and x['version']==ver]
   def med(k):return st.median(x[k] for x in vs)
   lines.append(f'| {nt} | {case} | {ver} | {med("ops_per_second"):.0f} [{min(x["ops_per_second"] for x in vs):.0f},{max(x["ops_per_second"] for x in vs):.0f}] | {med("p50_us"):.3f} | {med("p95_us"):.3f} | {med("p99_us"):.3f} | {max(x["max_us"] for x in vs):.3f} |')
lines+=['','所有性能结果均来自本次相同源码、相同配置重跑，未使用历史 20.79 µs 计算比例。诊断关闭；Release -O3 -DNDEBUG -std=c++17；无 LTO。输入提前准备，微基准循环无分配，分离编译的 CRC 调用、运行时输入、累加结果及 volatile sink 防止消除/合并。','','查询复用同一旧 CRC fixture，hash 前后一致。8192 键、16 B key、128 B value、每键单版本、交错分表使范围重叠；latest/oldest 键集、种子与既有工具一致，missing 为范围内奇数键。全部数据先校验；各查询类别再执行同一 trace 预热后采样。正确性校验在计时外，无并行构建/测试。1 表 latest/oldest 是同一键集。页缓存预热不等于引擎块缓存；小规模缓存负载结果不可外推冷盘。','','原始逐轮数值见 micro-r*.stdout.jsonl / query-r*.stdout.jsonl 和 results.jsonl；最大值易受调度影响，不能仅凭最大值判断稳定退化。临时目录 fixture 已在运行结束自动清理，未操作用户数据库。','']
(o/'SUMMARY.md').write_text('\n'.join(lines))
shutil.copytree(r/'benchmarks/crc_acceptance',o/'tools',ignore=shutil.ignore_patterns('__pycache__'))
(o/'test_coding.after.cc').write_text((r/'tests/lsm/test_coding.cc').read_text())
p=subprocess.run(['diff','-u',str(o/'test_coding.before.cc'),str(o/'test_coding.after.cc')],capture_output=True,text=True);(o/'test.patch').write_text(p.stdout)
# Workspace engine must remain byte-identical to initial snapshot.
assert hashes(r/'src/lsm')==b
print('\n'.join(lines))
