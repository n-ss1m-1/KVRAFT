# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
import struct
rows=[]
for p in sorted((O/'runs').glob('q*.json')):
 v=json.loads(p.read_text());charge=0;blocks=0;d=Path(v['db'])
 base=json.loads((O/'runs'/(d.name+'.json')).read_text())
 for meta in base['final']:
  data=(d/f"{meta['file']:06d}.sst").read_bytes();footer=data[-48:-4];idx,sz=struct.unpack_from('<QQ',footer,28);b=data[idx:idx+sz];count=struct.unpack_from('<I',b)[0];pos=4
  for i in range(count):
   n=struct.unpack_from('<I',b,pos)[0];pos+=4+n+8+1+8;size=struct.unpack_from('<I',b,pos)[0];pos+=4;charge+=size;blocks+=1
 if v['cache_capacity']:assert v['cache_capacity']>=charge and v['usage_steady']<=charge
 rows.append(dict(run=p.stem,blocks=blocks,all_block_charge_bytes=charge,capacity=v['cache_capacity'],usage=v['usage_steady']))
(O/'cache-worksets.json').write_text(json.dumps(rows,indent=2));print('cache charge checks',len(rows))