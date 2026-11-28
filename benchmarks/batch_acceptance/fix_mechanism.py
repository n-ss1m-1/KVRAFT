# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
p=Path(__file__).with_name('bench.cc');s=p.read_text();a=s.index('#ifdef MECHANISM');b=s.index('#endif',a)+len('#endif')
s=s[:a]+'''#ifdef MECHANISM
#include <sys/stat.h>
static bool iswal(int fd){
 char path[64],target[4096]; std::snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
 auto n=readlink(path,target,sizeof(target));if(n<4)return false;
 return std::string_view(target,n).substr(n-4)==".wal";
}
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" ssize_t __wrap_write(int fd,const void* p,size_t n){
 auto result=__real_write(fd,p,n);
 if(stage>0 && result>0 && iswal(fd)){counters[stage].bytes+=result;++counters[stage].records;}
 return result;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){
 bool wal=stage>0 && iswal(fd);struct stat st{};if(wal && fstat(fd,&st))wal=false;
 int result=__real_fsync(fd);
 if(wal && result==0){if(st.st_size>0)++counters[stage].data_sync;else ++counters[stage].new_sync;}
 return result;
}
#endif'''+s[b:];p.write_text(s)
p=Path(__file__).with_name('CMakeLists.txt');s=p.read_text();s=s[:s.index('target_link_options')]+ 'target_link_options(batch-mechanism PRIVATE "-Wl,--wrap=fsync" "-Wl,--wrap=write")\n';p.write_text(s)
p=Path(__file__).with_name('run.py');s=p.read_text().replace("f'pilot-{wal}-{batch}'","f'pilot-v2-{wal}-{batch}'");p.write_text(s)
