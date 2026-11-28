# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,struct,hashlib,subprocess,shutil
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/compaction-diagnosis-20261125'
def entries(b):
 p=0
 while p<len(b):
  seq,typ,n=struct.unpack_from('<QBI',b,p);key=b[p+13:p+13+n].decode();vl=struct.unpack_from('<I',b,p+13+n)[0];value=b[p+17+n:p+17+n+vl];yield key,(seq,typ,value);p+=17+n+vl
 assert p==len(b)
def oracle(d):
 b=(d/'oracle.bin').read_bytes();p=0;m={}
 while p<len(b):
  n=struct.unpack_from('<I',b,p)[0];p+=4;m.update(entries(b[p:p+n]));p+=n
 return m
def latest(d):
 m={}
 for f in d.glob('*.sst'):
  b=f.read_bytes();off,size=struct.unpack_from('<QQ',b[-48:-4],28);idx=b[off:off+size];n=struct.unpack_from('<I',idx)[0];p=4
  for i in range(n):
   ln=struct.unpack_from('<I',idx,p)[0];p+=4+ln+8+1;offset=struct.unpack_from('<Q',idx,p)[0];p+=8;size=struct.unpack_from('<I',idx,p)[0];p+=4
   for key,v in entries(b[offset:offset+size]):
    if key not in m or v[0]>m[key][0]:m[key]=v
 return m
checks=[]
for f in sorted((O/'runs').glob('*.json')):
 if f.name.endswith('.probe.json') or f.name.endswith('-slow-overlap.json'):continue
 v=json.loads(f.read_text());d=O/'copies'/v['name'];want=oracle(d)
 if v['mode']=='impact':
  seq=262144
  for i in range(16384):
   k=f'{(1000000+i)*2:016x}';val=bytes(33+(i*13+7*7+j*17)%90 for j in range(128));want[k]=(seq+i+1,1,val)
 have=latest(d);assert have==want,(v['name'],'latest physical/original sequence mismatch');checks.append({'name':v['name'],'latest_keys':len(have),'latest_tombstones':sum(v[1]==2 for v in have.values()),'sequence_value_type_verified':True})
(O/'physical-validation.json').write_text(json.dumps(checks,indent=2))
print('physical latest record/sequence checks',len(checks))
