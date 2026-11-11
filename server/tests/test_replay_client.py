from app import create_app
from replay_client import build_batches, load_rows


def test_dataset_has_expected_shape():
    rows = load_rows()
    assert len(rows) == 2665
    assert set(rows[0]) == {"timestamp", "temperature_c", "humidity_pct", "occupancy"}


def test_whole_dataset_is_accepted_by_the_bridge_without_gaps():
    client = create_app(forward_url="").test_client()
    rows = load_rows()
    for batch in build_batches(rows, "replay-01"):
        assert client.post("/api/samples", json=batch).status_code == 200

    status = client.get("/api/devices/replay-01").get_json()
    assert status["received"] == len(rows)
    assert status["gaps"] == 0
    assert status["duplicates"] == 0
    assert status["latest"][-1]["temp"] == round(float(rows[-1]["temperature_c"]), 2)


def test_batches_match_device_format():
    batch = build_batches(load_rows(limit=3), "d", batch_size=2)[0]
    assert batch["dropped"] == 0
    assert [s["seq"] for s in batch["samples"]] == [1, 2]
    assert set(batch["samples"][0]) == {"seq", "t", "temp", "rh"}
