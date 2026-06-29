from PyQt6.QtCore import QSettings

from aircube_ui.theme import Mode, Unit


def test_app_constructs_and_toggles(qapp):
    import aircube_app

    # Preserve the user's real settings; the toggles below would overwrite them.
    settings = QSettings("StuckAtPrototype", "AirCube")
    prev_appearance = settings.value("appearance")
    prev_unit = settings.value("unit")

    win = aircube_app.AirCubeApp()
    try:
        win.on_data_received({
            "timestamp": 1000, "temperature_c": 25.0, "temperature_f": 77.0,
            "humidity": 42.5, "aqi": 148, "eco2": 1238, "etvoc": 1396,
            "ens210_status": "ok", "ens16x_status": "ok",
        })
        assert win.sample_count == 1

        win.apply_mode(Mode.DARK)
        win.apply_unit(Unit.FAHRENHEIT)
        assert win.sensor_display.cards["temp"].value_label.text() == "77.0"

        win.apply_mode(Mode.LIGHT)
        win.apply_unit(Unit.CELSIUS)
        assert win.sensor_display.cards["temp"].value_label.text() == "25.0"
    finally:
        win.close()
        for key, value in (("appearance", prev_appearance), ("unit", prev_unit)):
            if value is None:
                settings.remove(key)
            else:
                settings.setValue(key, value)
