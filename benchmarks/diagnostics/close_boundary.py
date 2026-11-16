from pathlib import Path
r=Path('/home/truth/Code/my_kvraft')
p=r/'benchmarks/diagnostics/diagnose.cc';s=p.read_text();a=s.index(' // Full data validation');b=s.index(' {lsm_diag::Context ctx(lsm_diag::Reopen',a);s=s[:a]+''' // Close immediately after timed work: do not let full validation hide WAL cleanup/join.
 {lsm_diag::Context ctx(lsm_diag::Close,lsm_diag::Other);auto b=Clock::now();db.reset();close_seconds=sec(Clock::now()-b);}
'''+s[b:];s=s.replace('auto ro=opt;ro.enableWal=false;db=','auto ro=opt;ro.enableWal=false;db=');p.write_text(s)
# First batch remains raw; corrected close boundary gets a distinct run prefix.
p=r/'benchmarks/diagnostics/run.py';s=p.read_text().replace("elif a.stage=='representative':","elif a.stage in ('representative','close-boundary'):").replace("f'representative-r{rep}-{mode}-w{wal}-t{nt}'","f'{a.stage}-r{rep}-{mode}-w{wal}-t{nt}'");p.write_text(s)
