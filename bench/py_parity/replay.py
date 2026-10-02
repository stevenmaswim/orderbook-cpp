"""Replay py_parity.bin through the Python engine and time it.

This is the apples-to-apples Python number: the same ops the C++ books replay,
decoded into call arguments BEFORE timing starts (the C++ side also decodes
the trace before timing), each replay on a fresh MatchingEngine (with its
RLock, as in the Python repo's own benchmark). Prints one JSON line.

The Python repo's own number (benchmarks/RESULTS.md, run_benchmark.py) also
times demo.trader's random-number generation, so it is NOT the same quantity.

Usage:
    PYTHONDONTWRITEBYTECODE=1 ../Order_book_project/.venv/bin/python3.11 \
        bench/py_parity/replay.py --py-repo ../Order_book_project
"""
from __future__ import annotations

import argparse
import json
import statistics
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECORD = struct.Struct("<BBB5xQqq")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--py-repo", default=str(HERE.parents[2] / "Order_book_project"))
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--reps", type=int, default=3, help="replays per run, summed")
    args = ap.parse_args()
    sys.path.insert(0, str(Path(args.py_repo).resolve()))
    from orderbook import MatchingEngine, OrderType, Side  # noqa: E402

    data = (HERE / "py_parity.bin").read_bytes()
    assert data[:8] == b"OBTRACE1"
    (count,) = struct.unpack_from("<Q", data, 8)
    calls = []  # (is_submit, args...)
    for i in range(count):
        kind, side, typ, oid, price, qty = RECORD.unpack_from(data, 16 + 32 * i)
        if kind == 0:
            s = Side.BUY if side == 0 else Side.SELL
            if typ == 0:
                calls.append((True, s, OrderType.LIMIT, qty, price / 100))
            else:
                calls.append((True, s, OrderType.MARKET, qty, None))
        else:
            calls.append((False, oid))
    submits = sum(1 for c in calls if c[0])

    def replay() -> tuple[float, MatchingEngine]:
        engine = MatchingEngine()
        submit, cancel = engine.submit, engine.cancel
        t0 = time.perf_counter()
        for c in calls:
            if c[0]:
                submit(c[1], c[2], c[3], price=c[4])
            else:
                cancel(c[1])
        return time.perf_counter() - t0, engine

    replay()  # warmup, discarded
    orders_per_sec = []
    engine = None
    for _ in range(args.runs):
        total = 0.0
        for _ in range(args.reps):
            dt, engine = replay()
            total += dt
        orders_per_sec.append(submits * args.reps / total)

    print(json.dumps({
        "mode": "throughput", "book": "python", "workload": "py_parity",
        "python": sys.version.split()[0], "reps_per_run": args.reps,
        "orders_per_sec_runs": [round(x) for x in orders_per_sec],
        "orders_per_sec_median": round(statistics.median(orders_per_sec)),
        "trades": len(engine.trades()),
        "traded_qty": sum(t.quantity for t in engine.trades()),
    }))


if __name__ == "__main__":
    main()
