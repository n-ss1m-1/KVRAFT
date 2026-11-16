#!/usr/bin/env python3
"""Serial, process-isolated LSM baseline. No engine or global cache modifications."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import subprocess
import time


def command(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/lsm-bench/lsm-bench"))
    parser.add_argument("--output", type=Path, required=True, help="new directory; never overwrites a prior run")
    parser.add_argument("--operations", type=int, default=20000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--key-bytes", type=int, default=16)
    parser.add_argument("--value-bytes", type=int, default=128)
    parser.add_argument("--seed", type=int, default=20261115)
    parser.add_argument("--reopens", type=int, default=20)
    args = parser.parse_args()
    if min(args.operations, args.repeats, args.key_bytes, args.value_bytes, args.reopens) <= 0:
        parser.error("sizes and counts must be positive")
    if args.key_bytes < len(str(args.operations * 2 - 1)):
        parser.error("key width cannot represent the requested operation count")
    binary = args.binary.resolve(strict=True)
    root = Path(__file__).resolve().parent.parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    data = output / "databases"
    data.mkdir()
    huge = (1 << 63) - 1
    cases = []
    for wal in (0, 1):
        for mode in ("sequential", "random", "overwrite"):
            cases.append((mode, args.operations, wal, 0, huge))
        for ratio in ("mixed95", "mixed50"):
            cases.append((ratio, args.operations, wal, 4, 64 * 1024))
        for threshold in (16 * 1024, 64 * 1024, 256 * 1024):
            cases.append(("threshold", args.operations, wal, 0, threshold))
        cases.append(("flush", args.operations, wal, 0, huge))
    for count in (0, 1, 4, 16):
        cases.append(("query", args.operations, 0, count, huge))
    for n in sorted({max(1, args.operations // 10), args.operations}):
        for count in (0, 4, 16):
            cases.append(("recovery", n, 1, count, huge))
    # Fixed randomized case order limits systematic time/temperature bias.
    schedule = [(repeat, case) for repeat in range(args.repeats) for case in cases]
    random.Random(args.seed).shuffle(schedule)
    tracked = command("git", "-C", str(root), "ls-files").splitlines()
    sources = set(tracked)
    sources.update(str(p.relative_to(root)) for p in (root / "src/lsm").rglob("*") if p.is_file())
    sources.update(str(p.relative_to(root)) for p in (root / "benchmarks").iterdir() if p.is_file())
    hashes = {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sorted(sources) if (root / p).is_file()}
    metadata = {
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "arguments": vars(args) | {"binary": str(binary), "output": str(output)},
        "git_head": command("git", "-C", str(root), "rev-parse", "HEAD"),
        "git_status": command("git", "-C", str(root), "status", "--short"),
        "source_sha256": hashes,
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "platform": platform.platform(), "uname": command("uname", "-a"),
        "cpu": command("lscpu"), "memory": command("free", "-b"),
        "filesystem": command("df", "-T", str(output)),
        "mount": command("findmnt", "-T", str(output)),
        "lsblk": command("lsblk", "-o", "NAME,TYPE,SIZE,FSTYPE,MODEL"),
        "compiler": command("c++", "--version"),
        "cmake_cache": (binary.parent / "CMakeCache.txt").read_text(),
        "cache_policy": "No cache drops. First read follows fixture setup; OS cache uncontrolled. Warm hit pass repeats identical trace.",
        "physical_storage": "WSL virtual ext4 disk; host physical device/cache not identified by guest.",
        "schedule": schedule,
    }
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2, ensure_ascii=False) + "\n")
    results = []
    with (output / "results.jsonl").open("w") as stream:
        for index, (repeat, case) in enumerate(schedule):
            mode, n, wal, count, threshold = case
            case_dir = data / f"{index:04d}_{mode}_w{wal}_t{count}_n{n}"
            cmd = [str(binary), str(case_dir), mode, str(n), str(wal), str(count),
                   str(threshold), str(args.key_bytes), str(args.value_bytes), str(args.seed), str(args.reopens)]
            print(f"[{index+1}/{len(schedule)}] repeat={repeat} {case}", flush=True)
            start = time.monotonic()
            process = subprocess.run(cmd, text=True, capture_output=True)
            (output / f"case_{index:04d}.stderr.txt").write_text(process.stderr)
            (output / f"case_{index:04d}.stdout.jsonl").write_text(process.stdout)
            if process.returncode:
                raise RuntimeError(f"case {index} failed ({process.returncode}): {process.stderr}")
            elapsed = time.monotonic() - start
            for line in process.stdout.splitlines():
                row = json.loads(line)
                row.update(case_index=index, repeat=repeat, mode=mode, configured_operations=n,
                           wal=bool(wal), configured_tables=count, memtable_limit=threshold,
                           key_bytes=args.key_bytes, value_bytes=args.value_bytes, seed=args.seed,
                           process_wall_seconds=elapsed)
                results.append(row)
                stream.write(json.dumps(row) + "\n")
            stream.flush()
    import csv
    with (output / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)
    print(f"Completed {len(schedule)} cases; {len(results)} phase rows in {output}")
    print("Fixture databases retained; recorded disk size is logical file size, not host VHD allocation.")


if __name__ == "__main__":
    main()
