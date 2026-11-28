# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
p=Path(__file__).with_name('bench.cc')
s=p.read_text()
s=s.replace('auto submitted=Clock::now();stage=2; db->Flush();auto flushed=Clock::now();','''auto submitted=Clock::now();
 if(mode=="recovery"){
   stage=3; db.reset(); stage=4; opt.enableWal=true;
   {DB reopened(dir,opt);verify(reopened,oracle);}
   std::cout<<"{\\"status\\":\\"ok\\",\\"recovery_verified\\":true,\\"n\\":"<<n<<",\\"batch\\":"<<batch<<"}\\n";return 0;
 }
 stage=2; db->Flush();auto flushed=Clock::now();''')
s=s.replace('auto closed=Clock::now();stage=4;','auto closed=Clock::now();rusage usage{};getrusage(RUSAGE_SELF,&usage);stage=4;')
s=s.replace(' rusage usage{};getrusage(RUSAGE_SELF,&usage);',' ')
# replacement above also touched new line? no leading space immediately preceding rusage in insertion.
s=s.replace(' if(mode=="recovery")throw std::runtime_error("use separate recovery executable mode");','')
p.write_text(s)
