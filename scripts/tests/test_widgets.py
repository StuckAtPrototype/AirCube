from aircube_ui.theme import LIGHT, DARK, Unit
from aircube_ui.widgets import SensorDisplay


def test_sensor_display_updates_and_switches_units(qapp):
    sd = SensorDisplay()
    sd.apply_palette(LIGHT)
    sd.update_values({
        "temperature_c": 25.0, "humidity": 42.5,
        "aqi": 148, "eco2": 1238, "etvoc": 1396,
    })
    assert sd.cards["temp"].value_label.text() == "25.0"
    assert sd.cards["temp"].unit_label.text() == "°C"

    sd.set_unit(Unit.FAHRENHEIT)
    assert sd.cards["temp"].value_label.text() == "77.0"
    assert sd.cards["temp"].unit_label.text() == "°F"

    assert sd.cards["voc"].pill_label.text() == "Poor"

    sd.apply_palette(DARK)  # must not raise
    sd.clear_values()
    assert sd.cards["temp"].value_label.text() == "--.-"
