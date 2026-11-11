"""Demo without hardware: sends the real recorded dataset to the Flask bridge
in exactly the JSON format the ESP32 firmware uses.

    python app.py                                   # terminal 1: start the bridge
    python replay_client.py --limit 100             # terminal 2: send 100 readings
    curl http://localhost:5000/api/devices/replay-01

Data: UCI/Kaggle "Occupancy Detection" (CC BY 4.0), see ../data/README.md.
"""

from __future__ import annotations

import argparse
import csv
import time
from pathlib import Path

DEFAULT_CSV = Path(__file__).resolve().parent.parent / "data" / "office_room_2015.csv"


def load_rows(path: Path = DEFAULT_CSV, limit: int | None = None) -> list[dict]:
    with path.open(newline="") as f:
        rows = list(csv.DictReader(f))
    return rows[:limit] if limit else rows


def build_batches(rows: list[dict], device: str, batch_size: int = 10, period_ms: int = 1000) -> list[dict]:
    """Same structure as sm::encode_batch() on the device: seq starts at 1."""
    batches = []
    for start in range(0, len(rows), batch_size):
        chunk = rows[start : start + batch_size]
        batches.append(
            {
                "device": device,
                "dropped": 0,
                "samples": [
                    {
                        "seq": start + i + 1,
                        "t": (start + i) * period_ms,
                        "temp": round(float(r["temperature_c"]), 2),
                        "rh": round(float(r["humidity_pct"]), 2),
                    }
                    for i, r in enumerate(chunk)
                ],
            }
        )
    return batches


def main() -> None:
    import requests

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", default="http://localhost:5000/api/samples")
    parser.add_argument("--device", default="replay-01")
    parser.add_argument("--limit", type=int, default=None, help="number of readings to send")
    parser.add_argument("--delay", type=float, default=0.2, help="seconds between batches")
    args = parser.parse_args()

    for batch in build_batches(load_rows(limit=args.limit), args.device):
        r = requests.post(args.url, json=batch, timeout=3)
        last = batch["samples"][-1]
        print(f"seq {batch['samples'][0]['seq']:>4}-{last['seq']:<4} temp={last['temp']:.2f}C rh={last['rh']:.2f}% -> {r.status_code}")
        time.sleep(args.delay)


if __name__ == "__main__":
    main()
