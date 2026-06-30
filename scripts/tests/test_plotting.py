from aircube_ui.theme import LIGHT, DARK, Unit
from aircube_ui.plotting import PlotCanvas


def test_plot_canvas_draws_and_rethemes(qapp):
    c = PlotCanvas()
    c.apply_palette(LIGHT)
    x = [0, 1, 2]
    c.update_plot(x, [25, 25.1, 25.2], [42, 43, 44],
                  [148, 150, 149], [1238, 1240, 1239], [1396, 1400, 1398])
    assert c.unit == Unit.CELSIUS

    c.set_unit(Unit.FAHRENHEIT)
    c.update_plot(x, [25, 25.1, 25.2], [42, 43, 44],
                  [148, 150, 149], [1238, 1240, 1239], [1396, 1400, 1398])
    assert c.unit == Unit.FAHRENHEIT
    c.apply_palette(DARK)  # must not raise
