"""Flask bridge: receives sample batches from the ESP32 and forwards them.

- Validates every request against a Pydantic schema (bad data -> 422).
- Detects lost samples from gaps in the sequence numbers and compares them
  with the drop counter the device reports.
- Optionally forwards each valid batch to an external service (FORWARD_URL).
"""

from __future__ import annotations

import logging
import os
from collections import deque
from dataclasses import dataclass, field

import requests
from flask import Flask, jsonify, request
from pydantic import BaseModel, Field, ValidationError

log = logging.getLogger("bridge")


class SampleIn(BaseModel):
    seq: int = Field(ge=1)
    t: int = Field(ge=0, description="device time of acquisition in ms")
    temp: float = Field(ge=-45, le=130)
    rh: float = Field(ge=0, le=100)


class BatchIn(BaseModel):
    device: str = Field(min_length=1, max_length=32)
    dropped: int = Field(ge=0, description="samples dropped on the device since the last batch")
    samples: list[SampleIn] = Field(min_length=1, max_length=100)


@dataclass
class DeviceState:
    last_seq: int = 0
    received: int = 0
    gaps: int = 0  # samples missing according to sequence numbers
    reported_dropped: int = 0  # samples the device says it dropped
    duplicates: int = 0  # e.g. a batch re-sent after a lost HTTP response
    latest: deque = field(default_factory=lambda: deque(maxlen=100))


def create_app(forward_url: str | None = None, forward_timeout_s: float = 3.0) -> Flask:
    app = Flask(__name__)
    devices: dict[str, DeviceState] = {}
    forward_url = forward_url if forward_url is not None else os.environ.get("FORWARD_URL")

    @app.post("/api/samples")
    def ingest():
        try:
            batch = BatchIn.model_validate(request.get_json(force=True, silent=True) or {})
        except ValidationError as exc:
            return jsonify(error="invalid batch", details=exc.errors(include_url=False)), 422

        state = devices.setdefault(batch.device, DeviceState())
        state.reported_dropped += batch.dropped
        accepted = 0
        for s in batch.samples:
            if s.seq <= state.last_seq:
                # A device restart begins at seq 1 again; anything else is a retry.
                if s.seq == 1:
                    state.last_seq = 0
                else:
                    state.duplicates += 1
                    continue
            if state.last_seq and s.seq > state.last_seq + 1:
                state.gaps += s.seq - state.last_seq - 1
            state.last_seq = s.seq
            state.received += 1
            state.latest.append(s.model_dump())
            accepted += 1

        if forward_url and accepted:
            try:
                requests.post(forward_url, json=batch.model_dump(), timeout=forward_timeout_s).raise_for_status()
            except requests.RequestException as exc:
                # The device must not retry because of a downstream problem.
                log.warning("forwarding failed: %s", exc)
                return jsonify(accepted=accepted, forwarded=False), 202

        return jsonify(accepted=accepted, forwarded=bool(forward_url)), 200

    @app.get("/api/devices/<device>")
    def device_status(device: str):
        state = devices.get(device)
        if state is None:
            return jsonify(error="unknown device"), 404
        return jsonify(
            last_seq=state.last_seq,
            received=state.received,
            gaps=state.gaps,
            reported_dropped=state.reported_dropped,
            duplicates=state.duplicates,
            latest=list(state.latest)[-10:],
        )

    @app.get("/health")
    def health():
        return jsonify(status="ok")

    return app


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    create_app().run(host="0.0.0.0", port=int(os.environ.get("PORT", "5000")))
