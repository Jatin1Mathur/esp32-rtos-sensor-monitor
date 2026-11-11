from unittest.mock import patch

import pytest
import requests

from app import create_app


def batch(seqs, dropped=0, device="m5-01"):
    return {
        "device": device,
        "dropped": dropped,
        "samples": [{"seq": s, "t": s * 1000, "temp": 21.5, "rh": 40.0} for s in seqs],
    }


@pytest.fixture
def client():
    return create_app(forward_url="").test_client()


def status(client, device="m5-01"):
    return client.get(f"/api/devices/{device}").get_json()


def test_valid_batch_is_accepted(client):
    r = client.post("/api/samples", json=batch([1, 2, 3]))
    assert r.status_code == 200
    assert r.get_json()["accepted"] == 3
    assert status(client)["received"] == 3


@pytest.mark.parametrize(
    "bad",
    [
        {},
        {"device": "m5-01", "dropped": 0, "samples": []},
        {"device": "m5-01", "dropped": -1, "samples": [{"seq": 1, "t": 0, "temp": 20, "rh": 40}]},
        {"device": "m5-01", "dropped": 0, "samples": [{"seq": 1, "t": 0, "temp": 20, "rh": 140}]},
        {"device": "m5-01", "dropped": 0, "samples": [{"seq": 0, "t": 0, "temp": 20, "rh": 40}]},
    ],
)
def test_invalid_batch_is_rejected(client, bad):
    assert client.post("/api/samples", json=bad).status_code == 422


def test_non_json_body_is_rejected(client):
    r = client.post("/api/samples", data="not json", content_type="text/plain")
    assert r.status_code == 422


def test_sequence_gap_matches_reported_drops(client):
    client.post("/api/samples", json=batch([1, 2, 3]))
    client.post("/api/samples", json=batch([7, 8], dropped=3))  # 4, 5, 6 dropped on device
    s = status(client)
    assert s["gaps"] == 3
    assert s["reported_dropped"] == 3


def test_resent_batch_is_not_counted_twice(client):
    client.post("/api/samples", json=batch([1, 2, 3]))
    r = client.post("/api/samples", json=batch([1, 2, 3]))  # starts at seq 1 => device restart
    assert r.get_json()["accepted"] == 3
    client.post("/api/samples", json=batch([4, 5]))
    client.post("/api/samples", json=batch([4, 5]))  # retry after a lost HTTP response
    s = status(client)
    assert s["duplicates"] == 2
    assert s["last_seq"] == 5


def test_unknown_device_returns_404(client):
    assert client.get("/api/devices/nope").status_code == 404


def test_forwarding_failure_does_not_make_device_retry():
    app = create_app(forward_url="http://downstream.invalid/ingest")
    with patch("app.requests.post", side_effect=requests.ConnectionError("down")):
        r = app.test_client().post("/api/samples", json=batch([1]))
    assert r.status_code == 202
    assert r.get_json() == {"accepted": 1, "forwarded": False}


def test_forwarding_success():
    app = create_app(forward_url="http://downstream.invalid/ingest")
    with patch("app.requests.post") as post:
        r = app.test_client().post("/api/samples", json=batch([1, 2]))
    assert r.status_code == 200
    assert post.call_args.kwargs["json"]["device"] == "m5-01"


def test_health(client):
    assert client.get("/health").get_json() == {"status": "ok"}
