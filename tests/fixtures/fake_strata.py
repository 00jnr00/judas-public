#!/usr/bin/env python3
"""Stands in for `strata --serve` in tests/strata_tokenizer_check.cpp: same line protocol, canned answer."""
import sys
print("INFO engine=fake", flush=True)
print("READY 4096 stop", flush=True)
for line in sys.stdin:
    line = line.strip()
    if line == "QUIT":
        break
    if line == "STOP":
        continue
    if not line.startswith("GEN "):
        print("ERR expected GEN", flush=True)
        continue
    ids = line.rsplit(" ", 1)[1].split(",")
    print(f"RESUME {len(ids) // 2}", flush=True)
    print(f"PP {len(ids)} {len(ids)} 10.0 1000.0", flush=True)
    for t in (9419, 11, 1814, 0, 248046):   # "Hello, world!" then <|im_end|>
        print(f"T {t}", flush=True)
    print(f"DONE 5 {len(ids)} 10 20 stop 0 0 {len(ids) // 2}", flush=True)
