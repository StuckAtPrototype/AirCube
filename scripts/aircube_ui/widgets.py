"""Metric cards and the live sensor value row."""
from PyQt6.QtWidgets import (
    QFrame, QWidget, QVBoxLayout, QHBoxLayout, QLabel, QGridLayout
)
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QFont

from .theme import LIGHT, Unit, voc_band, format_temperature, ui_font


class MetricCard(QFrame):
    """A single metric: accent stripe, label, large value, unit, optional pill."""

    def __init__(self, title, accent, show_pill=False):
        super().__init__()
        self.accent = accent
        self.setObjectName("metricCard")

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        self.stripe = QFrame()
        self.stripe.setFixedHeight(3)
        outer.addWidget(self.stripe)

        body = QVBoxLayout()
        body.setContentsMargins(14, 12, 14, 14)
        body.setSpacing(6)

        self.title_label = QLabel(title)
        self.title_label.setFont(ui_font(10))
        body.addWidget(self.title_label)

        row = QHBoxLayout()
        row.setSpacing(6)
        self.value_label = QLabel("--.-")
        self.value_label.setFont(ui_font(24, QFont.Weight.DemiBold))
        self.value_label.setStyleSheet("font-variant-numeric: tabular-nums;")
        row.addWidget(self.value_label, alignment=Qt.AlignmentFlag.AlignBottom)

        self.unit_label = QLabel("")
        self.unit_label.setFont(ui_font(11))
        row.addWidget(self.unit_label, alignment=Qt.AlignmentFlag.AlignBottom)

        self.pill_label = QLabel("")
        self.pill_label.setFont(ui_font(9, QFont.Weight.DemiBold))
        self.pill_label.setVisible(show_pill)
        row.addWidget(self.pill_label, alignment=Qt.AlignmentFlag.AlignVCenter)
        row.addStretch()

        body.addLayout(row)
        outer.addLayout(body)

        self.apply_palette(LIGHT, accent)

    def set_value(self, value_text, unit_text):
        self.value_label.setText(value_text)
        self.unit_label.setText(unit_text)

    def set_value_color(self, color):
        self.value_label.setStyleSheet(
            f"color: {color}; font-variant-numeric: tabular-nums;"
        )

    def set_pill(self, text, color):
        self.pill_label.setText(text)
        self.pill_label.setStyleSheet(
            f"color: white; background-color: {color};"
            "border-radius: 8px; padding: 1px 8px;"
        )

    def apply_palette(self, palette, accent=None):
        if accent is not None:
            self.accent = accent
        p = palette
        self.setStyleSheet(
            f"#metricCard {{ background-color: {p.surface};"
            f" border: 1px solid {p.border}; border-radius: 12px; }}"
        )
        self.stripe.setStyleSheet(
            f"background-color: {self.accent};"
            "border-top-left-radius: 12px; border-top-right-radius: 12px;"
        )
        self.title_label.setStyleSheet(f"color: {p.text_secondary};")
        self.unit_label.setStyleSheet(f"color: {p.text_secondary};")
        if not self.value_label.styleSheet().startswith("color"):
            self.value_label.setStyleSheet(
                f"color: {p.text_primary}; font-variant-numeric: tabular-nums;"
            )


class SensorDisplay(QWidget):
    """Row of five metric cards with live values."""

    def __init__(self):
        super().__init__()
        self._unit = Unit.CELSIUS
        self._palette = LIGHT
        self._last = {}

        layout = QGridLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)

        self.cards = {
            "temp": MetricCard("Temperature", LIGHT.temp),
            "humidity": MetricCard("Humidity", LIGHT.humidity),
            "voc": MetricCard("VOC Level", LIGHT.voc, show_pill=True),
            "eco2": MetricCard("eCO2", LIGHT.eco2),
            "etvoc": MetricCard("eTVOC", LIGHT.etvoc),
        }
        for col, key in enumerate(["temp", "humidity", "voc", "eco2", "etvoc"]):
            layout.addWidget(self.cards[key], 0, col)

    def update_values(self, data):
        self._last = dict(data)
        temp = data.get("temperature_c")
        hum = data.get("humidity")
        aqi = data.get("aqi")
        eco2 = data.get("eco2")
        etvoc = data.get("etvoc")

        if temp is not None:
            text, unit_text = format_temperature(float(temp), self._unit)
            self.cards["temp"].set_value(text, unit_text)
        if hum is not None:
            self.cards["humidity"].set_value(f"{float(hum):.1f}", "%")
        if aqi is not None:
            color, label = voc_band(float(aqi))
            self.cards["voc"].set_value(f"{int(aqi)}", "")
            self.cards["voc"].set_value_color(color)
            self.cards["voc"].set_pill(label, color)
        if eco2 is not None:
            self.cards["eco2"].set_value(f"{int(eco2)}", "ppm")
        if etvoc is not None:
            self.cards["etvoc"].set_value(f"{int(etvoc)}", "ppb")

    def clear_values(self):
        self._last = {}
        self.cards["temp"].set_value(
            "--.-", "°C" if self._unit == Unit.CELSIUS else "°F"
        )
        self.cards["humidity"].set_value("--.-", "%")
        self.cards["voc"].set_value("---", "")
        self.cards["voc"].set_value_color(self._palette.text_primary)
        self.cards["voc"].pill_label.setText("")
        self.cards["voc"].pill_label.setStyleSheet("")
        self.cards["eco2"].set_value("----", "ppm")
        self.cards["etvoc"].set_value("----", "ppb")

    def set_unit(self, unit):
        self._unit = unit
        if self._last:
            self.update_values(self._last)
        else:
            self.clear_values()

    def apply_palette(self, palette):
        self._palette = palette
        accents = {
            "temp": palette.temp, "humidity": palette.humidity,
            "voc": palette.voc, "eco2": palette.eco2, "etvoc": palette.etvoc,
        }
        for key, card in self.cards.items():
            card.apply_palette(palette, accents[key])
        if self._last:
            self.update_values(self._last)
