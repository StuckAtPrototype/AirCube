from aircube_ui.theme import LIGHT, DARK, Unit, voc_band
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


def test_plain_value_color_follows_theme_switch(qapp):
    sd = SensorDisplay()
    sd.apply_palette(LIGHT)
    assert LIGHT.text_primary in sd.cards["temp"].value_label.styleSheet()
    sd.apply_palette(DARK)
    # Regression: plain value text must re-color for the dark theme, not stay
    # the light text color (which would be invisible on the dark card).
    assert DARK.text_primary in sd.cards["temp"].value_label.styleSheet()


def test_voc_band_color_survives_theme_switch(qapp):
    sd = SensorDisplay()
    sd.apply_palette(LIGHT)
    sd.update_values({
        "temperature_c": 25.0, "humidity": 42.5,
        "aqi": 250, "eco2": 1238, "etvoc": 1396,
    })
    band_color, _ = voc_band(250)
    assert band_color in sd.cards["voc"].value_label.styleSheet()
    sd.apply_palette(DARK)
    # The VOC band color is semantic and must survive a theme switch.
    assert band_color in sd.cards["voc"].value_label.styleSheet()
