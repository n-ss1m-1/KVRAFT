# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import struct,json,hashlib
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/compaction-diagnosis-20261125';H=R/'benchmarks/results/compaction-acceptance-20261125'
def entries(b):
 p=0;res=[]
 while p<len(b):
  seq,typ,ln=struct.unpack_from('<QBI',b,p);k=b[p+13:p+13+ln].decode();vl=struct.unpack_from('<I',b,p+13+ln)[0];p+=17+ln+vl;res.append((k,seq,typ))
 assert p==len(b);return res
rows=[]
# Map all key versions to physical blocks; footer/index parsed with exact on-disk v2 layout.
for side,d in [('before',H/'fixtures/multi-64'),('after',H/'copies/cost-r0-multi-64-c1')]:
 for f in sorted(d.glob('*.sst')):
  b=f.read_bytes();off,size=struct.unpack_from('<QQ',b[-48:-4],28);idx=b[off:off+size];n=struct.unpack_from('<I',idx)[0];p=4
  for j in range(n):
   ln=struct.unpack_from('<I',idx,p)[0];p+=4+ln+8+1;offset=struct.unpack_from('<Q',idx,p)[0];p+=8;sz=struct.unpack_from('<I',idx,p)[0];p+=4
   es=entries(b[offset:offset+sz])
   for pos,(key,seq,typ) in enumerate(es):rows.append({'side':side,'file':f.name,'block_index':j,'offset':offset,'bytes':sz,'block_records':len(es),'position_1based':pos+1,'key':key,'sequence':seq,'type':typ})
latest={}
for r in rows:
 k=(r['side'],r['key'])
 if k not in latest or r['sequence']>latest[k]['sequence']:latest[k]=r
traces={}
for file in (O/'runs').glob('*-trace.txt'):
 name=file.name.removesuffix('-trace.txt');data=file.read_bytes();traces[name]={'sha256':hashlib.sha256(data).hexdigest(),'requests':len(data.splitlines()),'unique':len(set(data.splitlines()))}
selected=[]
for side in ['before','after']:
 for k in sorted(set((O/'runs/r0-hot-s0-plain-trace.txt').read_text().splitlines())):
  selected.append(latest[side,k]);assert latest[side,k]['type']==1
(O/'hot-key-positions.json').write_text(json.dumps(selected,indent=2));(O/'trace-hashes.json').write_text(json.dumps(traces,indent=2))
print('block map ready; records',len(rows))
