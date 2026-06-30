import pytest

from aircube_ui.theme import voc_band, c_to_f, format_temperature, Unit


@pytest.mark.parametrize("value,label", [
    (0, "Good"), (50, "Good"),
    (51, "Moderate"), (100, "Moderate"),
    (101, "Poor"), (200, "Poor"),
    (201, "Unhealthy"), (999, "Unhealthy"),
])
def test_voc_band_labels(value, label):
    color, got = voc_band(value)
    assert got == label
    assert color.startswith("#") and len(color) == 7


def test_voc_band_colors_match_legacy_thresholds():
    assert voc_band(50)[0] == "#2e7d32"
    assert voc_band(100)[0] == "#f9a825"
    assert voc_band(200)[0] == "#ef6c00"
    assert voc_band(201)[0] == "#c62828"


def test_c_to_f():
    assert c_to_f(0) == 32.0
    assert c_to_f(100) == 212.0
    assert c_to_f(25) == 77.0


def test_format_temperature_celsius():
    assert format_temperature(25.0, Unit.CELSIUS) == ("25.0", "°C")


def test_format_temperature_fahrenheit():
    assert format_temperature(25.0, Unit.FAHRENHEIT) == ("77.0", "°F")
