# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

exec((__import__('pathlib').Path(__file__).parent/'paths.py').read_text())
(O/'runs').mkdir(exist_ok=True);(O/'db').mkdir(exist_ok=True)
def run(name,cfg='A',auto=1,work='unique',n=131072,kind='observer',wal=0,mode='settle',db=None,cap=0):
 out=O/'runs'/name;d=db or O/'db'/name
 exe=O/'build'/('bench_'+kind)
 if kind=='previous':exe=R/'benchmarks/results/auto-compaction-20261127/build/bench_engine'
 args=[str(exe),mode,str(d),str(out)]+list(map(str,[cap,work,n] if mode=='query' else [auto,wal,work,n]))
 env=dict(os.environ,RW_CONFIG=cfg)
 with (O/'commands.jsonl').open('a') as f:f.write(json.dumps({'env':{'RW_CONFIG':cfg},'argv':args})+'\n')
 t=time.monotonic()
 try:
  p=subprocess.run(args,env=env,capture_output=True,text=True,timeout=240);out.with_suffix('.stdout').write_text(p.stdout);out.with_suffix('.stderr').write_text(p.stderr)
  v=json.loads(p.stdout) if p.returncode==0 else {'failed':p.returncode}
 except subprocess.TimeoutExpired as e:
  v={'timeout':True};out.with_suffix('.stderr').write_text(str(e));out.with_suffix('.stdout').write_bytes(e.stdout or b'')
 v.update(name=name,config=cfg,auto=auto,wal=wal,work=work,kind=kind,mode=mode,process_s=time.monotonic()-t,n=n,db=str(d));out.with_suffix('.json').write_text(json.dumps(v,indent=2))
 print(name,round(v['process_s'],2),round(v.get('writes',{}).get('logical_ops_s',0)),flush=True)
 if not v.get('verified'):raise RuntimeError(name)
 return v
if __name__=='__main__':
 for cfg,n in [('A',131072),('B',131072),('C',524288)]:
  for work in ['unique','update']:run('pilot-'+cfg+'-'+work,cfg,1,work,n,'mechanism')