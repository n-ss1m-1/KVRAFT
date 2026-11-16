import csv
import datetime
import hashlib
import itertools
import json
from pathlib import Path
import random
import subprocess
import sys
import time

root=Path(__file__).resolve().parents[2]
out=root/'benchmarks/results/background-20261116'
def cmd(*args):return subprocess.check_output(args,text=True,stderr=subprocess.STDOUT).strip()
def hashes(base):return {str(p.relative_to(base)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(base.rglob('*')) if p.is_file()}
if len(sys.argv)>1 and sys.argv[1]=='smoke':
 for engine in ('sync','background'):
  for mode in ('random','mixed50'):
   p=out/f'smoke-{engine}-{mode}'
   subprocess.run([str(root/f'build/compare-{engine}/compare_bench'),str(p),mode,'500','0','4096','20261115',str(p)+'.csv'],check=True)
 sys.exit()
data=out/'databases'
data.mkdir(exist_ok=False)
schedule=[]
for repeat,mode,wal,limit in itertools.product(range(3),('random','mixed50'),(0,1),(16384,65536)):
 engines=['sync','background'];random.Random(20261115+len(schedule)).shuffle(engines)
 for engine in engines:schedule.append(dict(repeat=repeat,mode=mode,wal=wal,threshold=limit,engine=engine))
meta=dict(started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),operations=5000,key_bytes=16,value_bytes=128,seed=20261115,foreground_threads=1,repeats=3,background_threads={'sync':0,'background':1},schedule=schedule,
 uname=cmd('uname','-a'),cpu=cmd('lscpu'),memory=cmd('free','-b'),filesystem=cmd('df','-T',str(out)),mount=cmd('findmnt','-T',str(out)),compiler=cmd('c++','--version'),git_head=cmd('git','-C',str(root),'rev-parse','HEAD'),git_status=cmd('git','-C',str(root),'status','--short'),
 source_hashes={e:hashes(out/f'snapshots/{e}/src/lsm') for e in ('sync','background')},harness_hashes=hashes(root/'benchmarks/comparison'),
 binary_hashes={e:hashlib.sha256((root/f'build/compare-{e}/compare_bench').read_bytes()).hexdigest() for e in ('sync','background')},
 cmake_cache={e:(root/f'build/compare-{e}/CMakeCache.txt').read_text() for e in ('sync','background')})
(out/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
with (out/'results.jsonl').open('x') as log:
 for i,case in enumerate(schedule):
  name=f'{i:03d}-{case["engine"]}-{case["mode"]}-w{case["wal"]}-t{case["threshold"]}'
  args=[str(root/f'build/compare-{case["engine"]}/compare_bench'),str(data/name),case['mode'],'5000',str(case['wal']),str(case['threshold']),'20261115',str(out/f'{name}.latency.csv')]
  print(f'[{i+1}/{len(schedule)}] {name}',flush=True)
  start=time.monotonic();p=subprocess.run(args,capture_output=True,text=True)
  (out/f'{name}.stdout.json').write_text(p.stdout);(out/f'{name}.stderr.txt').write_text(p.stderr)
  if p.returncode:raise RuntimeError((name,p.returncode,p.stderr))
  row=json.loads(p.stdout)|case|dict(index=i,operations=5000,process_seconds=time.monotonic()-start)
  assert row['verified'] and row['sstables_after_close']>=3
  log.write(json.dumps(row)+'\n');log.flush()
rows=[json.loads(line) for line in (out/'results.jsonl').read_text().splitlines()]
with (out/'results.csv').open('x',newline='') as f:
 w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
print(f'Completed {len(rows)} cases',flush=True)
