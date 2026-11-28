# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
(O/'runs').mkdir(exist_ok=True);(O/'db').mkdir(exist_ok=True)
def run(name,budget=1048576,factor=1,work='unique',scale='A',kind='observer',wal=0,n=None,mode='settle',db=None,cap=0):
 n=n or (524288 if scale=='C' else 131072);out=O/'runs'/name;d=db or O/'db'/name
 args=[str(O/'build'/('bench_'+kind)),mode,str(d),str(out)]+list(map(str,[cap,work,n] if mode=='query' else [1,wal,work,n]))
 env=dict(os.environ,RW_CONFIG=scale,SOURCE_BYTES=str(budget),EXPANSION=str(factor))
 with (O/'commands.jsonl').open('a') as f:f.write(json.dumps({'env':{k:env[k] for k in ['RW_CONFIG','SOURCE_BYTES','EXPANSION']},'argv':args})+'\n')
 t=time.monotonic()
 try:
  p=subprocess.run(args,env=env,capture_output=True,text=True,timeout=240);out.with_suffix('.stdout').write_text(p.stdout);out.with_suffix('.stderr').write_text(p.stderr)
  v=json.loads(p.stdout) if p.returncode==0 else {'failed':p.returncode}
 except subprocess.TimeoutExpired as e:
  v={'timeout':True};out.with_suffix('.stderr').write_text(str(e));out.with_suffix('.stdout').write_bytes(e.stdout or b'')
 v.update(name=name,budget=budget,factor=factor,work=work,scale=scale,kind=kind,wal=wal,n=n,db=str(d),mode=mode,process_s=time.monotonic()-t);out.with_suffix('.json').write_text(json.dumps(v,indent=2));print(name,round(v['process_s'],2),round(v.get('writes',{}).get('logical_ops_s',0)),flush=True)
 if not v.get('verified'):raise RuntimeError('STOP: '+name)
 return v
if __name__=='__main__':
 for b,f in [(262144,4),(1048576,1),(1048576,4),(4194304,1)]:
  for w in ['unique','update']:run(f'pilot-{b}-{f}-{w}',b,f,w,kind='mechanism')