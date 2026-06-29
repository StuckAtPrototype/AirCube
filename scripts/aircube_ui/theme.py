"""Centralized appearance: palettes, stylesheet, color bands, units, fonts."""
from dataclasses import dataclass
from enum import Enum

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QFont, QFontDatabase


class Mode(str, Enum):
    SYSTEM = "system"
    LIGHT = "light"
    DARK = "dark"


class Unit(str, Enum):
    CELSIUS = "celsius"
    FAHRENHEIT = "fahrenheit"


@dataclass(frozen=True)
class Palette:
    name: str
    window_bg: str
    surface: str
    surface_alt: str
    border: str
    text_primary: str
    text_secondary: str
    text_muted: str
    accent: str
    accent_hover: str
    start: str
    start_hover: str
    danger: str
    danger_hover: str
    success: str
    temp: str
    humidity: str
    voc: str
    eco2: str
    etvoc: str
    plot_bg: str
    grid: str


LIGHT = Palette(
    name="light",
    window_bg="#f4f6f8",
    surface="#ffffff",
    surface_alt="#eef1f5",
    border="#e3e8ee",
    text_primary="#1a2230",
    text_secondary="#5b6573",
    text_muted="#8a94a3",
    accent="#2f8fed",
    accent_hover="#1f7fe0",
    start="#2e9e57",
    start_hover="#268a4b",
    danger="#e5484d",
    danger_hover="#d13b40",
    success="#1a8a4a",
    temp="#e5484d",
    humidity="#2f8fed",
    voc="#34a853",
    eco2="#8b5cf6",
    etvoc="#14b8a6",
    plot_bg="#ffffff",
    grid="#d8dee6",
)

DARK = Palette(
    name="dark",
    window_bg="#15191f",
    surface="#1e242c",
    surface_alt="#262d36",
    border="#2c343d",
    text_primary="#f0f3f7",
    text_secondary="#98a3b2",
    text_muted="#6b7682",
    accent="#4ea8ff",
    accent_hover="#3f97ee",
    start="#3bbf6e",
    start_hover="#34ab62",
    danger="#ff6b6f",
    danger_hover="#f1595d",
    success="#4ade80",
    temp="#ff6b6f",
    humidity="#4ea8ff",
    voc="#4ade80",
    eco2="#a78bfa",
    etvoc="#2dd4bf",
    plot_bg="#1e242c",
    grid="#2c343d",
)


def resolve_palette(mode, app):
    """Resolve a Mode (resolving SYSTEM via the OS color scheme) to a Palette."""
    if mode == Mode.LIGHT:
        return LIGHT
    if mode == Mode.DARK:
        return DARK
    try:
        if app.styleHints().colorScheme() == Qt.ColorScheme.Dark:
            return DARK
    except (AttributeError, TypeError):
        pass
    return LIGHT


# VOC bands preserve the legacy thresholds and colors; labels are new.
def voc_band(value):
    """Return (hex_color, label) for a VOC level value."""
    if value <= 50:
        return "#2e7d32", "Good"
    if value <= 100:
        return "#f9a825", "Moderate"
    if value <= 200:
        return "#ef6c00", "Poor"
    return "#c62828", "Unhealthy"


def c_to_f(celsius):
    """Convert Celsius to Fahrenheit."""
    return celsius * 9.0 / 5.0 + 32.0


def format_temperature(temp_c, unit):
    """Return (value_text, unit_text) for a Celsius reading in the chosen unit."""
    if unit == Unit.FAHRENHEIT:
        return f"{c_to_f(temp_c):.1f}", "°F"
    return f"{temp_c:.1f}", "°C"


def ui_font(size=10, weight=QFont.Weight.Normal, tabular=False):
    """Return the platform's default UI font at the given size/weight.

    When tabular=True, enable tabular (monospaced) figures so updating
    numbers don't shift width. Uses the Qt >= 6.7 font-feature API and
    degrades gracefully on older PyQt6.
    """
    family = QFontDatabase.systemFont(QFontDatabase.SystemFont.GeneralFont).family()
    font = QFont(family, size)
    font.setWeight(weight)
    if tabular:
        try:
            font.setFeature(QFont.Tag("tnum"), 1)
        except (AttributeError, TypeError):
            pass
    return font


def accent_button_qss(bg, hover):
    """QSS for a solid colored action button (Connect/Disconnect)."""
    return f"""
        QPushButton {{
            background-color: {bg};
            color: white;
            border: none;
            padding: 8px 16px;
            border-radius: 6px;
            font-weight: 600;
        }}
        QPushButton:hover {{ background-color: {hover}; }}
        QPushButton:disabled {{ background-color: #9aa3ad; }}
    """


def build_stylesheet(palette):
    """Build the global application stylesheet for the given palette."""
    p = palette
    return f"""
        QMainWindow, QWidget {{
            background-color: {p.window_bg};
            color: {p.text_primary};
        }}
        QMenuBar {{ background-color: {p.surface}; color: {p.text_primary}; }}
        QMenuBar::item:selected {{ background: {p.surface_alt}; }}
        QMenu {{ background-color: {p.surface}; color: {p.text_primary};
                 border: 1px solid {p.border}; }}
        QMenu::item:selected {{ background: {p.accent}; color: white; }}
        QGroupBox {{
            font-weight: 600;
            border: 1px solid {p.border};
            border-radius: 10px;
            margin-top: 12px;
            padding-top: 12px;
            background-color: {p.surface};
        }}
        QGroupBox::title {{
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 6px;
            color: {p.text_secondary};
        }}
        QLabel {{ color: {p.text_primary}; }}
        QPushButton {{
            background-color: {p.surface_alt};
            color: {p.text_primary};
            border: 1px solid {p.border};
            border-radius: 6px;
            padding: 6px 12px;
        }}
        QPushButton:hover {{ border-color: {p.accent}; }}
        QPushButton:disabled {{ color: {p.text_muted}; }}
        QComboBox, QSpinBox {{
            padding: 5px 8px;
            border: 1px solid {p.border};
            border-radius: 6px;
            background: {p.surface};
            color: {p.text_primary};
        }}
        QComboBox:hover, QSpinBox:hover {{ border-color: {p.accent}; }}
        QComboBox QAbstractItemView {{
            background: {p.surface};
            color: {p.text_primary};
            selection-background-color: {p.accent};
        }}
        QCheckBox {{ spacing: 8px; color: {p.text_primary}; }}
        QStatusBar {{ background-color: {p.surface}; color: {p.text_secondary}; }}
        QStatusBar::item {{ border: none; }}
    """
