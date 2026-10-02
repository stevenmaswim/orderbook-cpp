"""Record the Python engine's benchmark workload as a binary op trace.

Runs demo.trader from the Python engine repo UNCHANGED, in the same
single-threaded mode as its benchmarks/run_benchmark.py (seeds 42, 43, 44 one
after another), but hands it a recording proxy instead of the engine. Every
submit and cancel goes to the real Python MatchingEngine AND into the trace, so
the trace is exactly the op stream the Python benchmark executed.

Also writes the Python engine's end state (trades, volume, resting book) to
py_parity_expected.json. The C++ replay must reproduce it exactly: a free
cross-language correctness check.

Usage (Python 3.11 from the Python repo's venv, stdlib only):
    PYTHONDONTWRITEBYTECODE=1 ../Order_book_project/.venv/bin/python3.11 \
        bench/py_parity/export_trace.py --py-repo ../Order_book_project
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAGIC = b"OBTRACE1"
RECORD = struct.Struct("<BBB5xQqq")  # must match bench/trace.hpp TraceRecord
SUBMIT, CANCEL = 0, 1
BUY, SELL = 0, 1
LIMIT, MARKET = 0, 1


def to_ticks(price: float) -> int:
    # demo.py draws randint(9950, 10050) / 100, so x100 and round recovers the
    # exact integer it started from.
    return round(price * 100)


class RecordingEngine:
    """Looks like MatchingEngine to demo.trader; records every call."""

    def __init__(self, engine, Side, OrderType) -> None:
        self.engine = engine
        self.Side = Side
        self.OrderType = OrderType
        self.ops: list[tuple] = []

    def submit(self, side, order_type, quantity, price=None):
        order, trades = self.engine.submit(side, order_type, quantity, price=price)
        self.ops.append((
            SUBMIT,
            BUY if side is self.Side.BUY else SELL,
            LIMIT if order_type is self.OrderType.LIMIT else MARKET,
            order.order_id,  # the Python engine assigns ids 1, 2, 3, ...
            0 if price is None else to_ticks(price),
            quantity,
        ))
        return order, trades

    def cancel(self, order_id):
        self.ops.append((CANCEL, 0, 0, order_id, 0, 0))
        return self.engine.cancel(order_id)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--py-repo", default=str(HERE.parents[2] / "Order_book_project"))
    args = ap.parse_args()
    py_repo = Path(args.py_repo).resolve()
    sys.path.insert(0, str(py_repo))

    import demo  # noqa: E402  (the Python repo's own workload code)
    from orderbook import MatchingEngine, OrderType, Side  # noqa: E402

    engine = MatchingEngine()
    rec = RecordingEngine(engine, Side, OrderType)
    barrier = threading.Barrier(1)  # passes immediately, as in run_benchmark.py
    results: list[dict] = [{} for _ in range(demo.NUM_THREADS)]
    for i in range(demo.NUM_THREADS):
        demo.trader(rec, demo.BASE_SEED + i, barrier, results, i)

    trace = bytearray(MAGIC + struct.pack("<Q", len(rec.ops)))
    for op in rec.ops:
        trace += RECORD.pack(*op)
    out = HERE / "py_parity.bin"
    out.write_bytes(bytes(trace))

    trades = engine.trades()
    depth = engine._book.depth  # {Side: {price: qty}}, the engine's own view
    expected = {
        "source_repo_commit": subprocess.check_output(
            ["git", "-C", str(py_repo), "rev-parse", "--short", "HEAD"], text=True).strip(),
        "python": sys.version.split()[0],
        "ops": len(rec.ops),
        "submits": sum(1 for o in rec.ops if o[0] == SUBMIT),
        "cancels": sum(1 for o in rec.ops if o[0] == CANCEL),
        "trades": len(trades),
        "traded_qty": sum(t.quantity for t in trades),
        "resting_orders": sum(1 for o in engine.orders() if o.is_active),
        "bids": sorted(([to_ticks(p), q] for p, q in depth[Side.BUY].items()), reverse=True),
        "asks": sorted([to_ticks(p), q] for p, q in depth[Side.SELL].items()),
        "trace_sha256": hashlib.sha256(trace).hexdigest(),
    }
    (HERE / "py_parity_expected.json").write_text(json.dumps(expected, indent=1) + "\n")
    print(f"wrote {out.name}: {expected['ops']} ops ({expected['submits']} submits, "
          f"{expected['cancels']} cancels), sha256 {expected['trace_sha256'][:16]}...")


if __name__ == "__main__":
    main()
