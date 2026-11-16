from pathlib import Path
r=Path('/home/truth/Code/my_kvraft')
p=r/'src/lsm/include/lsm/diagnostics.h';s=p.read_text().replace('uint64_t count=0,ns=0,max_ns=0,failed=0,units=0;','uint64_t count=0,ns=0,max_ns=0,failed=0,units=0,timed_count=0;')
s=s.replace('inline thread_local Op operation=Other;','inline thread_local Op operation=Other;\ninline thread_local uint64_t block_sequence=0;\ninline thread_local bool sample_block=true;')
s=s.replace('uint64_t units=0) noexcept {','uint64_t units=0,bool timed=true) noexcept {').replace('++c.count;c.ns+=ns;','++c.count;c.timed_count+=timed;c.ns+=ns;').replace('record(m,0,false,units);','record(m,0,false,units,false);')
a=s.index('struct Timer {');b=s.index('\ntemplate<class F>',a)
s=s[:a]+'''struct Timer {
 Metric m;uint64_t start;int exceptions;bool bad=false,timed;
 Timer(Metric v) noexcept:m(v),start(0),exceptions(std::uncaught_exceptions()),timed(true){
  if(v==ReadBlock)sample_block=(block_sequence++%8)==0;
  if(v==ReadBlock||v==Pread||v==Crc||v==Validate||v==BlockGet)timed=sample_block;
  if(timed)start=now();
 }
 ~Timer(){record(m,timed?now()-start:0,bad||std::uncaught_exceptions()>exceptions,0,timed);}
 void fail()noexcept{bad=true;}
};'''+s[b:]
s=s.replace('<<c.count<<",\\\"ns\\\":"','<<c.count<<",\\\"timed_count\\\":"<<c.timed_count<<",\\\"ns\\\":"')
p.write_text(s)
# preserve full-density outputs and source, then rebuild
import shutil
out=r/'benchmarks/results/diagnostics-20261116-first'
shutil.copy2(p,out/'diagnostics-sampled.h')
p=r/'benchmarks/diagnostics/run.py';s=p.read_text();s=s.replace("elif a.stage=='first':",'''elif a.stage=='sampled-overhead':
 for rep in range(3):
  switches=['off','on'];random.Random(20261115+rep).shuffle(switches)
  for on in switches:run(f'sampled-overhead-r{rep}-query-{on}',on,'query',8192,0,9223372036854775807,16)
elif a.stage=='first':''');p.write_text(s)
