from pathlib import Path
p=Path('/home/truth/Code/my_kvraft/benchmarks/diagnostics/followup_run.py');s=p.read_text().replace("for label,wal,limit,threads,query in order:run(f'formal-r{rep}-{label}','on',wal,limit,threads,query=query)","""for label,wal,limit,threads,query in order:
   switches=['on','off'] if wal else ['on'];random.Random(20261115+rep+threads).shuffle(switches)
   for switch in switches:run(f'formal-r{rep}-{label}-{switch}',switch,wal,limit,threads,query=query)""");p.write_text(s)
