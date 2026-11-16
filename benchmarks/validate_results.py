#!/usr/bin/env python3
"""Validate a finished baseline against its recorded schedule."""
import argparse
import collections
import json
import math
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("results", type=Path)
args = parser.parse_args()
meta = json.loads((args.results / "metadata.json").read_text())
rows = [json.loads(s) for s in (args.results / "results.jsonl").read_text().splitlines()]
by_case = collections.defaultdict(list)
for row in rows:
    by_case[row["case_index"]].append(row)
assert set(by_case) == set(range(len(meta["schedule"]))), "missing cases"
for index, (repeat, case) in enumerate(meta["schedule"]):
    mode, n, wal, count, threshold = case
    samples = by_case[index]
    expected_phases = {
        "query": {"get_hit_first_pass", "get_hit_warm", "get_miss_first_pass", "get_miss_warm"},
        "mixed95": {"mixed", "get", "put", "delete"},
        "mixed50": {"mixed", "get", "put", "delete"},
        "recovery": {"open"}, "flush": {"manual_flush", "first_put_after_flush"},
    }.get(mode, {"put"})
    # Very small fixtures can produce no flushes or one empty mixed category.
    phases = {s["phase"] for s in samples}
    if mode == "threshold":
        assert phases <= {"put", "put_triggered_flush"} and "put" in phases
    elif mode.startswith("mixed"):
        assert "mixed" in phases and phases <= expected_phases
    else:
        assert phases == expected_phases, (index, phases)
    for row in samples:
        assert row["repeat"] == repeat and row["configured_operations"] == n
        assert row["wal"] == bool(wal) and row["configured_tables"] == count
        assert row["memtable_limit"] == threshold
        assert row["operations"] > 0
        assert 0 <= row["p50_us"] <= row["p95_us"] <= row["p99_us"] <= row["max_us"]
        assert math.isfinite(row["ops_per_second"]) and row["ops_per_second"] > 0
        if mode in ("query", "recovery"):
            assert row["sstables_end"] == min(count, n)
        if mode == "query":
            assert row["operations"] == n
        if mode in ("recovery", "flush"):
            assert row["operations"] == meta["arguments"]["reopens"]
    if mode.startswith("mixed"):
        sub = sum(s["operations"] for s in samples if s["phase"] != "mixed")
        assert sub == n
print(f"Validated {len(by_case)} completed cases and {len(rows)} phase rows")
