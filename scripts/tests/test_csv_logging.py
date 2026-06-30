import csv


def test_csv_logging_matches_canonical_format(qapp, tmp_path):
    """The CSV log must stay byte-compatible with the other AirCube scripts:
    the canonical header, and both temperature_c and temperature_f logged."""
    import aircube_app

    out = tmp_path / "log.csv"
    win = aircube_app.AirCubeApp()
    win.csv_path = str(out)
    try:
        win.start_csv_logging()
        win.on_data_received({
            "timestamp": 84231, "ens210_status": "ok",
            "temperature_c": 25.0, "temperature_f": 77.0, "humidity": 42.5,
            "ens16x_status": "ok", "etvoc": 1396, "eco2": 1238, "aqi": 148,
        })
        win.stop_csv_logging()
    finally:
        win.close()

    with open(out, newline="") as f:
        rows = list(csv.reader(f))

    assert rows[0] == aircube_app.CSV_HEADER
    assert rows[1] == [
        "84231", "ok", "25.0", "77.0", "42.5", "ok", "1396", "1238", "148"
    ]
