from pathlib import Path
import json,csv,collections,statistics,subprocess,hashlib,shutil
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/diagnostics-20261116-first'
rows=[json.loads(x) for x in (o/'runs.jsonl').read_text().splitlines()]
text=['# 统计开销对照','','下表是同配置相邻 on/off 三轮配对变化（百分比）。块采样后请求延迟仍完整记录；未机械扣除时钟成本。','','| density | case | throughput pair % | p99 pair % |','| --- | --- | --- | --- |']
for prefix in ['overhead','sampled-overhead']:
 groups=collections.defaultdict(dict)
 for row in rows:
  if not row['name'].startswith(prefix+'-'):continue
  rep=row['name'].split('-')[1 if prefix=='overhead' else 2]
  for v in row['results']:groups[(row['mode'],row['wal'],v['name'])].setdefault(rep,{})[row['diagnostics']]=v
 for k,reps in groups.items():
  rates=[100*(v['on']['ops_per_second']/v['off']['ops_per_second']-1) for v in reps.values()]
  tails=[100*(v['on']['p99_us']/v['off']['p99_us']-1) for v in reps.values()]
  text.append('| '+prefix+' | '+str(k)+' | '+', '.join(f'{v:+.2f}' for v in rates)+' | '+', '.join(f'{v:+.2f}' for v in tails)+' |')
text+=['','全密度出现 missing_first p99 三轮 +10%–13%，因此暂停正式采样，改为约 1/8 块计时并重测。采样后吞吐配对范围 −4.4%～+1.4%，仍有个别尾延迟波动（oldest_warm 一轮 +11.75%），不足以把几微秒尾延迟变化解释成引擎本身差异。','无 WAL 写入 p99 本身对少量刷盘边界很敏感，配对波动很大；所有原始值保留。WAL 开启组吞吐配对变化 −1.37%、+1.24%、+0.69%。','']
(o/'OVERHEAD.md').write_text('\n'.join(text))
# Frozen sources and exact delta against the initial dirty worktree.
shutil.copytree(r/'src/lsm',o/'source-after/lsm')
shutil.copy2(r/'CMakeLists.txt',o/'source-after/CMakeLists.txt')
shutil.copytree(r/'benchmarks/diagnostics',o/'harness',ignore=shutil.ignore_patterns('__pycache__'))
p=subprocess.run(['diff','-ruN',str(o/'source-before'),str(o/'source-after')],capture_output=True,text=True)
(o/'source-diff.patch').write_text(p.stdout)
def cmd(*a):return subprocess.check_output(a,text=True,stderr=subprocess.STDOUT)
def hashes(p):return {str(x.relative_to(p)):hashlib.sha256(x.read_bytes()).hexdigest() for x in p.rglob('*') if x.is_file()}
meta=dict(key_bytes=16,value_bytes=128,seed=20261115,foreground_threads=1,background_threads=1,selected_write_n=16384,query_n=8192,
 phases=['setup','request','final_flush','verify','reopen','close','latest_first','latest_warm','oldest_first','oldest_warm','missing_first','missing_warm'],ops=['other','put','delete','get','flush','background'],
 source_before_hashes=hashes(o/'source-before'),source_after_hashes=hashes(o/'source-after'),harness_hashes=hashes(o/'harness'),
 uname=cmd('uname','-a'),cpu=cmd('lscpu'),filesystem=cmd('df','-T',str(o)),compiler=cmd('c++','--version'),git_status=cmd('git','-C',str(r),'status','--short'),
 cmake_cache={v:(r/f'build/diag-{v}/CMakeCache.txt').read_text() for v in ['on','off']},binary_hashes={v:hashlib.sha256((r/f'build/diag-{v}/diagnose').read_bytes()).hexdigest() for v in ['on','off']},
 note='Full-density overhead uses archived diagnostics-full-density.h; sampled outputs have timed_count. close-boundary is final representative data; earlier representative runs retained with validation-before-close limitation.')
(o/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
assert len([x for x in rows if x['name'].startswith('close-boundary')])==12
for row in rows:
 assert row['success']
 if row['diagnostics']=='on':
  stats=json.loads((o/(row['name']+'.stats.json')).read_text());assert not stats['dropped_threads']
  assert all(not c['failed'] for c in stats['cells'])
print('Archived',len(rows),'successful runs; final 12 representative runs checked')
