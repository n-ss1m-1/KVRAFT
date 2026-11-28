# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import subprocess,os,json,time,datetime,hashlib
R=Path("/home/truth/Code/my_kvraft");O=R/"benchmarks/results/source-expansion-timeout-audit-20261128";old=R/"benchmarks/results/source-expansion-resume-20261128"
import sys
i=int(sys.argv[1]) if len(sys.argv)>1 else 1
d=O/f"repro{i}-db";out=O/f"repro{i}"
args=[str(old/"build/bench_mechanism"),"settle",str(d),str(out),"1","0","unique","131072"]
env=dict(os.environ,RW_CONFIG="A",SOURCE_BYTES="262144",EXPANSION="2")
(O/f"repro{i}-command.json").write_text(json.dumps({"argv":args,"env":{k:env[k] for k in ["RW_CONFIG","SOURCE_BYTES","EXPANSION"]},"start":datetime.datetime.now().astimezone().isoformat(),"binary_sha256":hashlib.sha256(Path(args[0]).read_bytes()).hexdigest()},indent=2))
t=time.monotonic();last=None
with out.with_suffix(".stdout").open("w") as so,out.with_suffix(".stderr").open("w") as se,(O/f"repro{i}-monitor.jsonl").open("w") as m:
 p=subprocess.Popen(args,env=env,stdout=so,stderr=se)
 while p.poll() is None:
  row={"elapsed_s":time.monotonic()-t,"pid":p.pid,"wall":datetime.datetime.now().astimezone().isoformat()}
  if d.exists():
   files=list(d.glob("*.sst"));mf=d/"MANIFEST"
   row.update(sst_count=len(files),sst_bytes=sum(f.stat().st_size for f in files if f.exists()))
   if mf.exists():row.update(manifest_mtime_ns=mf.stat().st_mtime_ns,manifest_bytes=mf.stat().st_size)
  if row["elapsed_s"]>105:
   ts=[]
   for task in Path(f"/proc/{p.pid}/task").glob("*"):
    z={"tid":task.name}
    for name in ["wchan","stat","syscall","stack"]:
     try:z[name]=(task/name).read_text()
     except Exception as e:z[name]=str(e)
    ts.append(z)
   row["threads"]=ts
  m.write(json.dumps(row)+"\n");m.flush()
  if row["elapsed_s"]>235:
   (O/f"repro{i}-prekill.txt").write_text(subprocess.run(["ps","-L","-p",str(p.pid),"-o","pid,tid,stat,wchan:32,etime,comm"],capture_output=True,text=True).stdout)
   p.terminate()
   try:p.wait(5)
   except subprocess.TimeoutExpired:p.kill()
   break
  time.sleep(2)
 rc=p.wait()
result={"exit":rc,"process_s":time.monotonic()-t}
try:result["output"]=json.loads(out.with_suffix(".stdout").read_text())
except Exception:pass
(O/f"repro{i}-status.json").write_text(json.dumps(result,indent=2))
print(json.dumps(result)[:1200],flush=True)