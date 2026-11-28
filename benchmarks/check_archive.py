#!/usr/bin/env python3
"""Check historical tools without importing scripts or starting workloads."""
import ast
import hashlib
import json
import re
from pathlib import Path


def main():
    root = Path(__file__).resolve().parent
    inventory = json.loads((root / "archive_inventory.json").read_text())
    errors = []
    for entry in inventory["files"]:
        path = root / entry["path"]
        if not path.is_file():
            errors.append(f"missing: {entry['path']}")
            continue
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != entry["sha256"]:
            errors.append(f"hash changed: {entry['path']}")
        if path.suffix == ".py":
            try:
                ast.parse(data.decode("utf-8"), filename=str(path))
            except (SyntaxError, UnicodeError) as error:
                errors.append(str(error))
            if path.name != "check_archive.py" and "KVRAFT_RUN_HISTORICAL_BENCHMARK" not in data.decode():
                errors.append(f"missing execution guard: {entry['path']}")
        if path.suffix in {".cc", ".h"}:
            for include in re.findall(r'^\s*#include\s*"([^"\n]+)"', data.decode(), re.M):
                if include.endswith((".cc", "base.cc")) and not (path.parent / include).is_file():
                    errors.append(f"missing local source include: {path.name}: {include}")
    if errors:
        raise SystemExit("\n".join(errors))
    print(f"PASS: {len(inventory['files'])} archived files; hashes, Python syntax, guards and local source includes")
    print("Historical engine snapshots and performance results were not validated by this check.")


if __name__ == "__main__":
    main()
