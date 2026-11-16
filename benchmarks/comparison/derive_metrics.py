import csv
import json
from pathlib import Path
root=Path(__file__).resolve().parents[2]
out=root/'benchmarks/results/background-20261116'
rows=[json.loads(s) for s in (out/'results.jsonl').read_text().splitlines()]
for r in rows:
 r['put_ops_per_second']=r['puts']/r['request_seconds']
 r['mutation_ops_per_second']=(r['puts']+r['deletes'])/r['request_seconds']
 r['mutation_ops_per_second_with_flush']=(r['puts']+r['deletes'])/r['total_with_flush_seconds']
with (out/'derived_metrics.csv').open('x',newline='') as f:
 w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
