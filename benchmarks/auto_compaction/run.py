# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,json,time,csv
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/auto-compaction-20261127';(O/'runs').mkdir(exist_ok=True);(O/'db').mkdir(exist_ok=True)
def run(name,kind,mode,auto,wal,work,n=131072,db=None,cap=0):
 out=O/'runs'/name;d=db or O/'db'/name
 args=[str(O/'build'/('bench_'+kind)),mode,str(d),str(out)]
 args+=list(map(str,[cap,work,n] if mode=='query' else [auto,wal,work,n]))
 with (O/'commands.jsonl').open('a') as f:f.write(json.dumps(args)+'\n')
 t=time.monotonic()
 try:
  p=subprocess.run(args,capture_output=True,text=True,timeout=180);out.with_suffix('.stdout').write_text(p.stdout);out.with_suffix('.stderr').write_text(p.stderr)
  v=json.loads(p.stdout) if p.returncode==0 else {'failed':p.returncode}
 except subprocess.TimeoutExpired as e:v={'timeout':True};out.with_suffix('.stderr').write_text(str(e))
 v.update(name=name,kind=kind,mode=mode,work=work,auto=auto,wal=wal,process_s=time.monotonic()-t);out.with_suffix('.json').write_text(json.dumps(v,indent=2))
 print(name,round(v['process_s'],2),v.get('writes',{}).get('logical_ops_s',''),flush=True)
 if 'failed' in v or 'timeout' in v:raise RuntimeError(name)
 return v
if __name__=='__main__':
 for work in ['unique','update']:
  run('pilot-'+work,'probe','settle',1,0,work)
  rows=list(csv.DictReader((O/'runs'/('pilot-'+work+'-tasks.csv')).open()));print('tasks by source', {l:sum(int(x['level'])==l for x in rows) for l in [0,1]},flush=True)