"""Theme- and unit-aware matplotlib canvas with three stacked subplots."""
import matplotlib
matplotlib.use('QtAgg')
from matplotlib.backends.backend_qtagg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

from .theme import LIGHT, Unit, c_to_f


class PlotCanvas(FigureCanvas):
    """Temp/humidity, VOC, and gas subplots that re-theme with the palette."""

    def __init__(self, parent=None):
        self.palette = LIGHT
        self.unit = Unit.CELSIUS
        self.fig = Figure(figsize=(10, 6), dpi=100)
        super().__init__(self.fig)
        self.setParent(parent)

        self.ax_temp_hum = self.fig.add_subplot(311)
        self.ax_aqi = self.fig.add_subplot(312, sharex=self.ax_temp_hum)
        self.ax_gases = self.fig.add_subplot(313, sharex=self.ax_temp_hum)

        self.fig.tight_layout(pad=2.0)
        self._apply_theme_styling()

    def _temp_label(self):
        return "Temperature (°F)" if self.unit == Unit.FAHRENHEIT else "Temperature (°C)"

    def _apply_theme_styling(self):
        p = self.palette
        self.fig.set_facecolor(p.plot_bg)
        for ax in (self.ax_temp_hum, self.ax_aqi, self.ax_gases):
            ax.set_facecolor(p.plot_bg)
            ax.grid(True, linestyle='--', alpha=0.7, color=p.grid)
            ax.tick_params(colors=p.text_secondary)
            for spine in ax.spines.values():
                spine.set_color(p.border)
            ax.yaxis.label.set_color(p.text_secondary)
            ax.xaxis.label.set_color(p.text_secondary)

    def apply_palette(self, palette):
        self.palette = palette
        self._apply_theme_styling()
        self.draw()

    def set_unit(self, unit):
        self.unit = unit
        self.draw()

    def update_plot(self, x, temp_c, hum, aqi, eco2, etvoc):
        p = self.palette
        temp = [c_to_f(v) for v in temp_c] if self.unit == Unit.FAHRENHEIT else list(temp_c)

        for ax in (self.ax_temp_hum, self.ax_aqi, self.ax_gases):
            ax.cla()

        self.ax_temp_hum.plot(x, temp, label=self._temp_label(), color=p.temp, linewidth=1.5)
        self.ax_temp_hum.plot(x, hum, label="Humidity (%)", color=p.humidity, linewidth=1.5)
        self.ax_temp_hum.set_ylabel("Temp / Humidity")
        self.ax_temp_hum.legend(loc="upper left", fontsize=8)

        self.ax_aqi.plot(x, aqi, label="VOC Level", color=p.voc, linewidth=1.5)
        self.ax_aqi.set_ylabel("VOC Level")
        self.ax_aqi.legend(loc="upper left", fontsize=8)

        self.ax_gases.plot(x, eco2, label="eCO2 (ppm)", color=p.eco2, linewidth=1.5)
        self.ax_gases.plot(x, etvoc, label="eTVOC (ppb)", color=p.etvoc, linewidth=1.5)
        self.ax_gases.set_ylabel("Gas levels")
        self.ax_gases.set_xlabel("Time (s)")
        self.ax_gases.legend(loc="upper left", fontsize=8)

        self._apply_theme_styling()
        self.fig.tight_layout(pad=2.0)
        self.draw()
