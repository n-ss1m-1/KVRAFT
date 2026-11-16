from pathlib import Path
import struct,collections,json,subprocess,hashlib,shutil,statistics as st
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/diagnostics-20261116-followup'
# Read SST indexes off-line: no benchmark timing or engine changes.
sizes=collections.Counter()
for p in (o/'databases/formal-r0-query-on').glob('*.sst'):
 data=p.read_bytes();footer=data[-48:-4];index_offset,index_size=struct.unpack_from('<QQ',footer,28)
 idx=data[index_offset:index_offset+index_size];num=struct.unpack_from('<I',idx)[0];pos=4
 for i in range(num):
  klen=struct.unpack_from('<I',idx,pos)[0];pos+=4+klen+8+1+8
  size=struct.unpack_from('<I',idx,pos)[0];pos+=4;sizes[size]+=1
(o/'actual-block-sizes.json').write_text(json.dumps(dict(sorted(sizes.items())),indent=2))
rows=[json.loads(x) for x in (o/'runs.jsonl').read_text().splitlines()];assert len([v for v in rows if v['name'].startswith('formal')])==27
shutil.copytree(r/'src/lsm',o/'source-after/lsm');shutil.copytree(r/'benchmarks/diagnostics',o/'harness-after',ignore=shutil.ignore_patterns('__pycache__'))
p=subprocess.run(['diff','-ruN',str(o/'source-before/lsm'),str(o/'source-after/lsm')],capture_output=True,text=True);(o/'source-diff.patch').write_text(p.stdout)
def cmd(*args):return subprocess.check_output(args,text=True,stderr=subprocess.STDOUT)
def hashes(p):return {str(x.relative_to(p)):hashlib.sha256(x.read_bytes()).hexdigest() for x in p.rglob('*') if x.is_file()}
meta=dict(source_hashes=hashes(o/'source-after'),harness_hashes=hashes(o/'harness-after'),git=cmd('git','-C',str(r),'status','--short'),uname=cmd('uname','-a'),cpu=cmd('lscpu'),filesystem=cmd('df','-T',str(o)),compiler=cmd('c++','--version'),cmake={v:(r/f'build/diag-{v}/CMakeCache.txt').read_text() for v in ['on','off']},build_commands={v:cmd('ninja','-C',str(r/f'build/diag-{v}'),'-t','commands') for v in ['on','off']},seed=20261115,key_bytes=16,value_bytes=128,total_write_requests=16384,query_requests=8192,block_sizes=dict(sizes))
(o/'metadata.json').write_text(json.dumps(meta,indent=2))
over=cmd('python3',str(r/'benchmarks/diagnostics/followup_overhead.py'));(o/'overhead.txt').write_text(over)
print('actual block sizes',sizes)
for wal,limit,threads in [(0,65536,1),(0,1048576,1),(1,1048576,1),(1,1048576,2),(1,1048576,4)]:
 vs=[v for v in rows if v['name'].startswith('formal') and not v['query'] and v['on']=='on' and (v['wal'],v['limit'],v['threads'])==(wal,limit,threads)]
 print('configuration',wal,limit,threads,'request range', [round(v['request_ops'],2) for v in vs])
 if wal==0:
  for metric in ['sst_build','sst_writer_write_sync_rename_dirsync','reader_init','publish_wait','manifest_save','publish_hold']:
   sums=[]
   for v in vs:
    ss=json.loads((o/(v['name']+'.stats.json')).read_text());sums.append(sum(m['ns'] for t in ss['tasks'] for m in t['metrics'] if m['metric']==metric)/1e6)
   print(metric,st.median(sums))
