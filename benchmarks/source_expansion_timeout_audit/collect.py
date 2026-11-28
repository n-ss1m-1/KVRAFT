# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import sys,json,subprocess,hashlib,shutil,csv,datetime
R=Path("/home/truth/Code/my_kvraft");O=R/"benchmarks/results/source-expansion-timeout-audit-20261128";T=R/"benchmarks/source_expansion_timeout_audit";old=R/"benchmarks/results/source-expansion-resume-20261128"
sys.path.insert(0,str(R/"benchmarks/source_expansion_resume"))
from paths import hashes
rows=[];last=[]
for i in [1,2,3]:
 v=json.loads((O/f"repro{i}-status.json").read_text());x=v["output"];assert v["exit"]==0 and x["verified"]
 p=subprocess.run([str(O/"build/audit"),"check",str(O/f"repro{i}-db")],capture_output=True,text=True)
 (O/f"repro{i}-extra-reopen.txt").write_text(p.stdout+p.stderr);assert p.returncode==0
 es=[json.loads(s) for s in (O/f"repro{i}-events.jsonl").read_text().splitlines()];tasks=[e for e in es if e["kind"]=="compaction"]
 row=dict(round=i,process_s=v["process_s"],request_s=x["writes"]["wall_ns"]/1e9,flush_s=x["flush_ns"]/1e9,settle_s=x["converge_ns"]/1e9,close_s=x["close_ns"]/1e9,tasks=len(tasks),verified=True)
 rows.append(row)
 for e in tasks[-5:]:
  b=lambda k:sum(f["bytes"] for f in e[k])
  last.append(dict(round=i,task=e["id"],source_level=e["level"],target_level=e["level"]+1,source_bytes=b("source"),target_bytes=b("target"),output_bytes=b("output"),start_ns=e["start_ns"],build_start_ns=e["buildStart_ns"],build_end_ns=e["buildEnd_ns"],published_ns=e["published_ns"],unlock_ns=e["unlock_ns"],task_end_ns=e["end_ns"],after_count=e["after_count"],after_bytes=e["after_bytes"]))
(O/"reproduction-summary.json").write_text(json.dumps(rows,indent=2));(O/"reproduction-last-tasks.json").write_text(json.dumps(last,indent=2))
# Preserve exact timeout evidence and implementation read for the audit.
(O/"evidence").mkdir()
for p in (old/"runs/invalid").glob("matrixm-262144-2-unique*"):shutil.copy2(p,O/"evidence"/p.name)
for name in ["commands.jsonl","resume-state.json","engine-hashes.json","mechanism.patch","observer.patch","environment.txt","configure.txt","build.txt","tests.txt"]:
 shutil.copy2(old/name,O/"evidence"/name)
for name in ["bench.cc","rw.h","totals.h","run.py","base.cc"]:
 shutil.copy2(R/"benchmarks/source_expansion_resume"/name,O/"evidence"/name)
shutil.copytree(old/"mechanism",O/"mechanism-snapshot")
(O/"dmesg-current.txt").write_text(subprocess.run(["dmesg","--ctime"],capture_output=True,text=True).stdout)
before=json.loads((O/"engine-before.json").read_text())
assert hashes(R/"src/lsm")==before
assert subprocess.check_output(["git","diff","--binary"],cwd=R)==(O/"git-before.patch").read_bytes()
# Check original failed DB itself remains byte-for-byte unchanged.
d=old/"db/matrixm-262144-2-unique"
orig=json.loads((O/"original-files.json").read_text())
assert all(hashlib.sha256((d/f["name"]).read_bytes()).hexdigest()==f["sha256"] for f in orig)
(O/"preservation.json").write_text(json.dumps({"main_engine_unchanged":True,"original_git_diff_unchanged":True,"failed_database_unchanged":True,"HEAD":subprocess.check_output(["git","rev-parse","HEAD"],cwd=R,text=True).strip(),"new_bytes":sum(p.stat().st_size for p in O.rglob("*") if p.is_file())},indent=2))
(O/"tool-hashes.json").write_text(json.dumps(hashes(T),indent=2))
shutil.copytree(T,O/"tool-snapshot")
print(json.dumps(rows,indent=2))
print("PRESERVATION PASS")