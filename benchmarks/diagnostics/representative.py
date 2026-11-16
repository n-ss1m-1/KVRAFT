from pathlib import Path
p=Path('/home/truth/Code/my_kvraft/benchmarks/diagnostics/run.py')
s=p.read_text().replace("elif a.stage=='first':",'''elif a.stage=='representative':
 n=json.loads((out/'selected-n.json').read_text())['n']
 cases=[('write',n,1,1048576,0),('write',n,0,65536,0),('query',8192,0,9223372036854775807,1),('query',8192,0,9223372036854775807,64)]
 for rep in range(3):
  order=cases.copy();random.Random(20261115+rep).shuffle(order)
  for mode,n,wal,limit,nt in order:run(f'representative-r{rep}-{mode}-w{wal}-t{nt}','on',mode,n,wal,limit,nt)
elif a.stage=='first':''')
p.write_text(s)
