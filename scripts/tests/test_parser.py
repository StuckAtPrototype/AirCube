from aircube_ui.serial_reader import parse_json_line

VALID = (
    '{"timestamp": 84231, '
    '"ens210": {"temperature_c": 25.0, "temperature_f": 77.0, "humidity": 42.5, "status": "ok"}, '
    '"ens16x": {"status": "ok", "etvoc": 1396, "eco2": 1238, "aqi": 148}}'
)


def test_parses_valid_line():
    d = parse_json_line(VALID)
    assert d["timestamp"] == 84231
    assert d["temperature_c"] == 25.0
    assert d["temperature_f"] == 77.0
    assert d["humidity"] == 42.5
    assert d["aqi"] == 148
    assert d["eco2"] == 1238
    assert d["etvoc"] == 1396


def test_parses_json_embedded_in_noise():
    d = parse_json_line("LOG: " + VALID + "  <-- sample")
    assert d is not None and d["aqi"] == 148


def test_returns_none_for_no_json():
    assert parse_json_line("just a log line, no json here") is None


def test_returns_none_for_malformed_json():
    assert parse_json_line('{"ens210": {bad json}}') is None


def test_returns_none_for_missing_sensor_keys():
    assert parse_json_line('{"timestamp": 1, "ens210": {"temperature_c": 25}}') is None
