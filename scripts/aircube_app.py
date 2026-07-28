"""
AirCube - Air Quality Monitor
A standalone desktop application for the AirCube sensor device.

Supports both AirCube Base and AirCube Pro. Pro units additionally report
true NDIR CO2 (SCD41) and ambient light (VCNL4040); those tiles and plots
light up automatically when a Pro device is detected.
"""

__version__ = "2.1.0"
__app_name__ = "AirCube"

import collections
import csv
import json
import math
import os
import re
import sys
from datetime import datetime, timedelta

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QComboBox, QCheckBox, QFileDialog,
    QStatusBar, QMessageBox, QSpinBox, QFrame, QGridLayout,
    QTabWidget, QProgressBar
)
from PyQt6.QtCore import QTimer, Qt, QThread, pyqtSignal
from PyQt6.QtGui import QFont

import matplotlib
matplotlib.use('QtAgg')
from matplotlib.backends.backend_qtagg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure
from matplotlib.ticker import MaxNLocator
import matplotlib.dates as mdates

import serial
from serial.tools import list_ports

# History fetch tuning (matches firmware serial_protocol.c limits)
HISTORY_PAGE_SIZE = 48
HISTORY_REQUEST_TIMEOUT_MS = 4000
HISTORY_MAX_RETRIES = 2

# JSON pattern for parsing sensor data
JSON_PATTERN = re.compile(r"\{.*\}")

# CSV header: original columns first (compatible with older AirCube scripts),
# Pro-era columns appended at the end.
CSV_HEADER = [
    "timestamp", "ens210_status", "temperature_c", "temperature_f",
    "humidity", "ens16x_status", "etvoc", "eco2", "aqi",
    "aqi_s", "aqi_uba", "model", "co2", "lux",
]

# ---------------------------------------------------------------------------
# Theme
# ---------------------------------------------------------------------------

class Theme:
    BG          = "#0d1117"   # window background
    CARD        = "#161b22"   # tile / panel background
    CARD_BORDER = "#21262d"
    TEXT        = "#e6edf3"
    MUTED       = "#8b949e"
    ACCENT      = "#2f81f7"

    TEMP   = "#ff6b6b"
    HUM    = "#4dabf7"
    VOC    = "#69db7c"
    ECO2   = "#b197fc"
    ETVOC  = "#38d9a9"
    CO2    = "#ffa94d"
    LUX    = "#ffd43b"

    GOOD   = "#3fb950"
    WARN   = "#d29922"
    BAD    = "#f0883e"
    CRIT   = "#f85149"

    GRID   = "#2d333b"


def voc_color(aqi):
    """Color for the canonical VOC Level (0-500 TVOC-derived bands)."""
    if aqi <= 50:
        return Theme.GOOD
    if aqi <= 100:
        return Theme.WARN
    if aqi <= 200:
        return Theme.BAD
    return Theme.CRIT


def co2_color(ppm):
    """Color for true CO2 concentration (ppm)."""
    if ppm < 800:
        return Theme.GOOD
    if ppm < 1200:
        return Theme.WARN
    if ppm < 2000:
        return Theme.BAD
    return Theme.CRIT


def parse_json_line(line):
    """Parse a JSON sensor data line into a flat dict."""
    match = JSON_PATTERN.search(line)
    if not match:
        return None
    try:
        data = json.loads(match.group(0))
        ens210 = data.get("ens210", {})
        ens16x = data.get("ens16x", {})
        scd41 = data.get("scd41", {})
        vcnl = data.get("vcnl4040", {})
        if "timestamp" not in data or not ens210 or not ens16x:
            return None
        return {
            "timestamp": data.get("timestamp"),
            "model": data.get("model", "base"),
            "temperature_c": ens210.get("temperature_c"),
            "temperature_f": ens210.get("temperature_f"),
            "humidity": ens210.get("humidity"),
            "ens210_status": ens210.get("status"),
            "ens16x_status": ens16x.get("status"),
            "etvoc": ens16x.get("etvoc"),
            "eco2": ens16x.get("eco2"),
            "aqi": ens16x.get("aqi"),
            "aqi_s": ens16x.get("aqi_s"),
            "aqi_uba": ens16x.get("aqi_uba"),
            "co2": scd41.get("co2"),
            "lux": vcnl.get("lux"),
        }
    except (KeyError, TypeError, json.JSONDecodeError):
        return None


class SerialReaderThread(QThread):
    """Background thread for reading serial data and sending commands.

    Incoming lines are routed by shape:
      - live sensor JSON (has "ens210"/"ens16x")   -> data_received
      - history page     (has "history")            -> history_received
      - history metadata (has "history_info")       -> history_info_received
      - command acks / errors (has "status")        -> response_received
    """
    data_received = pyqtSignal(dict)
    history_info_received = pyqtSignal(dict)
    history_received = pyqtSignal(dict)
    response_received = pyqtSignal(dict)
    error_occurred = pyqtSignal(str)

    def __init__(self, port, baud=115200):
        super().__init__()
        self.port = port
        self.baud = baud
        self.running = False
        self.serial = None

    def send_command(self, command):
        """Write a JSON command line to the device (thread-safe: pyserial
        write is atomic for small payloads and only the GUI thread calls this)."""
        if not (self.serial and self.serial.is_open):
            return False
        try:
            # Compact separators: the firmware's minimal JSON parser expects
            # "cmd":"..." with no whitespace after the colon.
            payload = (json.dumps(command, separators=(",", ":")) + "\n").encode()
            self.serial.write(payload)
            return True
        except (serial.SerialException, OSError) as e:
            self.error_occurred.emit(str(e))
            return False

    def _route_line(self, decoded):
        match = JSON_PATTERN.search(decoded)
        if not match:
            return
        try:
            data = json.loads(match.group(0))
        except json.JSONDecodeError:
            return
        if not isinstance(data, dict):
            return

        if "history" in data:
            self.history_received.emit(data)
        elif "history_info" in data:
            self.history_info_received.emit(data["history_info"])
        elif "ens210" in data or "ens16x" in data:
            parsed = parse_json_line(decoded)
            if parsed:
                self.data_received.emit(parsed)
        elif "status" in data:
            self.response_received.emit(data)
        # config responses and unknown lines are ignored for now

    def run(self):
        try:
            self.serial = serial.Serial(self.port, self.baud, timeout=0.1)
            self.running = True
            while self.running:
                try:
                    line = self.serial.readline()
                    if line:
                        decoded = line.decode(errors="ignore").strip()
                        if decoded:
                            self._route_line(decoded)
                except (serial.SerialException, OSError) as e:
                    if self.running:
                        self.error_occurred.emit(str(e))
                    break
        except serial.SerialException as e:
            self.error_occurred.emit(str(e))
        finally:
            if self.serial and self.serial.is_open:
                self.serial.close()

    def stop(self):
        self.running = False
        self.wait(2000)


# ---------------------------------------------------------------------------
# UI widgets
# ---------------------------------------------------------------------------

class SensorTile(QFrame):
    """A modern card-style tile showing a single sensor value."""

    def __init__(self, title, unit, accent, placeholder="--"):
        super().__init__()
        self.accent = accent
        self.placeholder = placeholder
        self.setObjectName("sensorTile")
        self.setStyleSheet(f"""
            QFrame#sensorTile {{
                background-color: {Theme.CARD};
                border: 1px solid {Theme.CARD_BORDER};
                border-radius: 10px;
            }}
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(14, 10, 14, 10)
        layout.setSpacing(2)

        title_row = QHBoxLayout()
        dot = QLabel("\u25cf")
        dot.setStyleSheet(f"color: {accent}; font-size: 9px; background: transparent; border: none;")
        title_label = QLabel(title.upper())
        title_label.setStyleSheet(
            f"color: {Theme.MUTED}; font-size: 10px; font-weight: 600; "
            "letter-spacing: 1px; background: transparent; border: none;"
        )
        title_row.addWidget(dot)
        title_row.addWidget(title_label)
        title_row.addStretch()
        layout.addLayout(title_row)

        value_row = QHBoxLayout()
        value_row.setSpacing(4)
        self.value_label = QLabel(placeholder)
        self.value_label.setFont(QFont("Segoe UI", 22, QFont.Weight.Bold))
        self.value_label.setStyleSheet(f"color: {Theme.TEXT}; background: transparent; border: none;")
        unit_label = QLabel(unit)
        unit_label.setStyleSheet(f"color: {Theme.MUTED}; font-size: 12px; background: transparent; border: none;")
        value_row.addWidget(self.value_label)
        value_row.addWidget(unit_label, alignment=Qt.AlignmentFlag.AlignBottom)
        value_row.addStretch()
        layout.addLayout(value_row)

    def set_value(self, text, color=None):
        self.value_label.setText(text)
        self.value_label.setStyleSheet(
            f"color: {color or Theme.TEXT}; background: transparent; border: none;"
        )

    def clear(self):
        self.set_value(self.placeholder)


class SensorDisplay(QWidget):
    """Row of tiles showing current sensor values. Pro tiles appear when a
    Pro device is detected."""

    def __init__(self):
        super().__init__()
        self.layout = QGridLayout(self)
        self.layout.setContentsMargins(0, 0, 0, 0)
        self.layout.setSpacing(10)

        self.temp_tile = SensorTile("Temperature", "°C", Theme.TEMP, "--.-")
        self.hum_tile = SensorTile("Humidity", "%", Theme.HUM, "--.-")
        self.voc_tile = SensorTile("VOC Level", "", Theme.VOC, "---")
        self.eco2_tile = SensorTile("eCO2", "ppm", Theme.ECO2, "----")
        self.etvoc_tile = SensorTile("eTVOC", "ppb", Theme.ETVOC, "----")
        self.co2_tile = SensorTile("CO2 (NDIR)", "ppm", Theme.CO2, "----")
        self.lux_tile = SensorTile("Ambient Light", "lx", Theme.LUX, "----")

        self.base_tiles = [
            self.temp_tile, self.hum_tile, self.voc_tile,
            self.eco2_tile, self.etvoc_tile,
        ]
        self.pro_tiles = [self.co2_tile, self.lux_tile]

        for col, tile in enumerate(self.base_tiles + self.pro_tiles):
            self.layout.addWidget(tile, 0, col)
            self.layout.setColumnStretch(col, 1)

        # Pro tiles hidden until a pro device reports in
        self._pro_visible = False
        for tile in self.pro_tiles:
            tile.hide()

    def set_pro_visible(self, visible):
        if visible == self._pro_visible:
            return
        self._pro_visible = visible
        for tile in self.pro_tiles:
            tile.setVisible(visible)

    def update_values(self, data):
        temp = data.get("temperature_c")
        hum = data.get("humidity")
        aqi = data.get("aqi")
        eco2 = data.get("eco2")
        etvoc = data.get("etvoc")
        co2 = data.get("co2")
        lux = data.get("lux")

        if temp is not None:
            self.temp_tile.set_value(f"{temp:.1f}")
        if hum is not None:
            self.hum_tile.set_value(f"{hum:.1f}")
        if aqi is not None:
            self.voc_tile.set_value(f"{int(aqi)}", voc_color(aqi))
        if eco2 is not None:
            self.eco2_tile.set_value(f"{int(eco2)}")
        if etvoc is not None:
            self.etvoc_tile.set_value(f"{int(etvoc)}")
        if self._pro_visible:
            if co2 is not None:
                self.co2_tile.set_value(f"{int(co2)}", co2_color(co2))
            if lux is not None:
                self.lux_tile.set_value(f"{lux:.0f}")

    def clear_values(self):
        for tile in self.base_tiles + self.pro_tiles:
            tile.clear()


class PlotCanvas(FigureCanvas):
    """Matplotlib canvas for plotting sensor data, dark-themed."""

    def __init__(self, parent=None):
        self.fig = Figure(figsize=(10, 6), dpi=100)
        self.fig.set_facecolor(Theme.CARD)
        super().__init__(self.fig)
        self.setParent(parent)
        self.pro_mode = False
        self.ax_hum = None   # twin of climate (humidity)
        self.ax_lux = None   # twin of tvoc (ambient light), pro only
        self._build_axes()

    def _build_axes(self):
        self.fig.clf()
        # 2x2 grid: climate, VOC level, CO2, TVOC
        self.ax_climate = self.fig.add_subplot(221)
        self.ax_voc = self.fig.add_subplot(222, sharex=self.ax_climate)
        self.ax_co2 = self.fig.add_subplot(223, sharex=self.ax_climate)
        self.ax_tvoc = self.fig.add_subplot(224, sharex=self.ax_climate)
        self.ax_hum = None
        self.ax_lux = None
        # Style the empty axes up front so they match the dark theme before
        # any data has arrived (otherwise matplotlib draws white panels).
        for ax, title in (
            (self.ax_climate, "CLIMATE"),
            (self.ax_voc, "VOC LEVEL"),
            (self.ax_co2, "CO2"),
            (self.ax_tvoc, "TVOC"),
        ):
            self._style_axis(ax, title)
        self._apply_layout()

    def _apply_layout(self):
        self.fig.subplots_adjust(left=0.075, right=0.925, top=0.9,
                                 bottom=0.12, hspace=0.55, wspace=0.32)

    def _style_axis(self, ax, title):
        """Style a primary axis: dark panel, y-only grid, no top/right spines."""
        ax.set_facecolor(Theme.CARD)
        ax.set_title(title, color=Theme.TEXT, fontsize=10.5, fontweight="bold",
                     loc="left", pad=10)
        ax.grid(True, axis="y", linestyle="-", linewidth=0.6,
                color=Theme.GRID, alpha=0.45)
        ax.set_axisbelow(True)
        ax.tick_params(colors=Theme.MUTED, labelsize=8, length=0)
        ax.margins(x=0.02, y=0.25)
        ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
        ax.xaxis.set_major_locator(MaxNLocator(nbins=6))
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(Theme.CARD_BORDER)
        self._no_offset(ax)

    def _style_twin(self, ax, color):
        """Style a right-hand twin axis, tinted to match its series color."""
        ax.set_facecolor("none")
        ax.tick_params(colors=color, labelsize=8, length=0)
        ax.margins(y=0.25)
        ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
        for side in ("top", "left", "bottom"):
            ax.spines[side].set_visible(False)
        ax.spines["right"].set_color(Theme.CARD_BORDER)
        self._no_offset(ax)

    @staticmethod
    def _no_offset(ax):
        """Disable the +1e1-style offset label that clutters small ranges."""
        try:
            ax.ticklabel_format(useOffset=False, axis="y", style="plain")
        except (AttributeError, ValueError):
            pass

    def _legend(self, ax, handles, labels):
        # Anchor just above the top-right of the panel so lines never collide
        # with the plotted data.
        legend = ax.legend(handles, labels, loc="lower right",
                           bbox_to_anchor=(1.0, 1.0), fontsize=7.5,
                           frameon=False, ncol=len(labels), labelcolor=Theme.MUTED,
                           handlelength=1.3, handletextpad=0.5, columnspacing=1.1,
                           borderaxespad=0.0)
        return legend

    def set_pro_mode(self, pro):
        self.pro_mode = pro

    def update_plot(self, x, series):
        """Redraw all plots. `series` is a dict of lists keyed by field."""
        lw = 1.9
        cap = "round"

        # ---- CLIMATE: temperature (left) + humidity (right twin) ----
        ax = self.ax_climate
        ax.cla()
        self._style_axis(ax, "CLIMATE")
        ax.tick_params(labelbottom=False)
        lt, = ax.plot(x, series["temp"], color=Theme.TEMP, lw=lw, solid_capstyle=cap)
        ax.set_ylabel("°C", color=Theme.TEMP, fontsize=8)
        if self.ax_hum is None:
            self.ax_hum = ax.twinx()
        self.ax_hum.cla()
        self._style_twin(self.ax_hum, Theme.HUM)
        lh, = self.ax_hum.plot(x, series["hum"], color=Theme.HUM, lw=lw, solid_capstyle=cap)
        self.ax_hum.set_ylabel("%", color=Theme.HUM, fontsize=8)
        self._legend(ax, [lt, lh], ["Temp", "Humidity"])

        # ---- VOC LEVEL: single line with soft fill ----
        ax = self.ax_voc
        ax.cla()
        self._style_axis(ax, "VOC LEVEL")
        ax.tick_params(labelbottom=False)
        ax.plot(x, series["aqi"], color=Theme.VOC, lw=lw, solid_capstyle=cap)
        ax.fill_between(x, series["aqi"], color=Theme.VOC, alpha=0.13)

        # ---- CO2: eCO2 (+ true NDIR CO2 on pro), shared ppm axis ----
        ax = self.ax_co2
        ax.cla()
        self._style_axis(ax, "CO2")
        ax.set_xlabel("Time (s)", color=Theme.MUTED, fontsize=8)
        ax.set_ylabel("ppm", color=Theme.MUTED, fontsize=8)
        handles = [ax.plot(x, series["eco2"], color=Theme.ECO2, lw=lw, solid_capstyle=cap)[0]]
        labels = ["eCO2"]
        if self.pro_mode:
            handles.append(ax.plot(x, series["co2"], color=Theme.CO2, lw=lw, solid_capstyle=cap)[0])
            labels.append("CO2 NDIR")
        self._legend(ax, handles, labels)

        # ---- TVOC: eTVOC (+ ambient light twin on pro) ----
        ax = self.ax_tvoc
        ax.cla()
        self._style_axis(ax, "TVOC / LIGHT" if self.pro_mode else "TVOC")
        ax.set_xlabel("Time (s)", color=Theme.MUTED, fontsize=8)
        lv, = ax.plot(x, series["etvoc"], color=Theme.ETVOC, lw=lw, solid_capstyle=cap)
        ax.set_ylabel("ppb", color=Theme.ETVOC, fontsize=8)
        if self.pro_mode:
            if self.ax_lux is None:
                self.ax_lux = ax.twinx()
            self.ax_lux.cla()
            self._style_twin(self.ax_lux, Theme.LUX)
            ll, = self.ax_lux.plot(x, series["lux"], color=Theme.LUX, lw=lw, solid_capstyle=cap)
            self.ax_lux.set_ylabel("lx", color=Theme.LUX, fontsize=8)
            self._legend(ax, [lv, ll], ["eTVOC", "Light"])
        else:
            if self.ax_lux is not None:
                self.ax_lux.remove()
                self.ax_lux = None

        self._apply_layout()
        self.draw()


class HistoryPlotCanvas(PlotCanvas):
    """Dark-themed canvas for on-device history: avg lines with min/max bands
    against real timestamps."""

    def _date_axis(self, ax):
        locator = mdates.AutoDateLocator(maxticks=6)
        ax.xaxis.set_major_locator(locator)
        ax.xaxis.set_major_formatter(mdates.ConciseDateFormatter(locator))

    def _band(self, ax, x, avg, lo, hi, color, band_alpha=0.18):
        line, = ax.plot(x, avg, color=color, lw=1.8, solid_capstyle="round")
        ax.fill_between(x, lo, hi, color=color, alpha=band_alpha, linewidth=0)
        return line

    def update_history(self, times, series, pro=False):
        """Redraw history charts.

        `times` is a list of datetimes; `series` maps field -> (avg, min, max)
        tuples of lists for temp, hum, voc, co2, etvoc.
        """
        # ---- CLIMATE ----
        ax = self.ax_climate
        ax.cla()
        self._style_axis(ax, "CLIMATE")
        lt = self._band(ax, times, *series["temp"], Theme.TEMP)
        ax.set_ylabel("°C", color=Theme.TEMP, fontsize=8)
        if self.ax_hum is None:
            self.ax_hum = ax.twinx()
        self.ax_hum.cla()
        self._style_twin(self.ax_hum, Theme.HUM)
        lh = self._band(self.ax_hum, times, *series["hum"], Theme.HUM)
        self.ax_hum.set_ylabel("%", color=Theme.HUM, fontsize=8)
        self._legend(ax, [lt, lh], ["Temp", "Humidity"])
        self._date_axis(ax)

        # ---- VOC LEVEL ----
        ax = self.ax_voc
        ax.cla()
        self._style_axis(ax, "VOC LEVEL")
        self._band(ax, times, *series["voc"], Theme.VOC)
        self._date_axis(ax)

        # ---- CO2 (true CO2 on Pro, eCO2 estimate on Base) ----
        ax = self.ax_co2
        ax.cla()
        self._style_axis(ax, "CO2" if pro else "eCO2")
        self._band(ax, times, *series["co2"], Theme.CO2 if pro else Theme.ECO2)
        ax.set_ylabel("ppm", color=Theme.MUTED, fontsize=8)
        self._date_axis(ax)

        # ---- TVOC ----
        ax = self.ax_tvoc
        ax.cla()
        self._style_axis(ax, "TVOC")
        self._band(ax, times, *series["etvoc"], Theme.ETVOC)
        ax.set_ylabel("ppb", color=Theme.ETVOC, fontsize=8)
        self._date_axis(ax)
        if self.ax_lux is not None:
            self.ax_lux.remove()
            self.ax_lux = None

        self._apply_layout()
        self.draw()


# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------

FIELDS = ("temp", "hum", "aqi", "eco2", "etvoc", "co2", "lux")

HISTORY_CSV_HEADER = [
    "timestamp", "sequence",
    "temp_avg_c", "temp_min_c", "temp_max_c",
    "hum_avg", "hum_min", "hum_max",
    "voc_avg", "voc_min", "voc_max",
    "co2_avg", "co2_min", "co2_max",
    "etvoc_avg", "etvoc_min", "etvoc_max",
]


class AirCubeApp(QMainWindow):
    """Main application window."""

    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"{__app_name__} v{__version__} — Air Quality Monitor")
        self.setMinimumSize(1080, 760)

        # Data storage
        self.max_points = 300
        self.data_buffer = collections.deque(maxlen=self.max_points)
        self.t0 = None
        self.sample_count = 0
        self.model = None

        # Serial and CSV
        self.serial_thread = None
        self.csv_file = None
        self.csv_writer = None
        self.csv_path = None

        # On-device history fetch state
        self.history_fetching = False
        self.history_expected = 0        # total entries reported by device
        self.history_slots = []          # accumulated slot dicts (device order)
        self.history_next_start = 0
        self.history_retries = 0
        self.history_window_s = 300.0    # slot window (s), refined by history_info
        self.history_fetch_time = None   # datetime anchor for slot timestamps
        self.history_timeout = QTimer()
        self.history_timeout.setSingleShot(True)
        self.history_timeout.timeout.connect(self._history_request_timed_out)

        self.setup_ui()
        self.setup_timers()
        self.refresh_ports()

    # -- UI -----------------------------------------------------------------

    def setup_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        main_layout.setSpacing(12)
        main_layout.setContentsMargins(14, 14, 14, 10)

        # Header: app name, model badge, connection controls
        header = QHBoxLayout()
        header.setSpacing(10)

        title = QLabel(__app_name__)
        title.setFont(QFont("Segoe UI", 18, QFont.Weight.Bold))
        title.setStyleSheet(f"color: {Theme.TEXT};")
        header.addWidget(title)

        self.model_badge = QLabel("")
        self.model_badge.setFont(QFont("Segoe UI", 9, QFont.Weight.Bold))
        self.model_badge.hide()
        header.addWidget(self.model_badge)

        header.addStretch()

        header.addWidget(self._muted_label("PORT"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(220)
        header.addWidget(self.port_combo)

        self.refresh_btn = QPushButton("\u21bb")
        self.refresh_btn.setFixedWidth(34)
        self.refresh_btn.setToolTip("Refresh ports")
        self.refresh_btn.clicked.connect(self.refresh_ports)
        header.addWidget(self.refresh_btn)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.setMinimumWidth(110)
        self.connect_btn.setObjectName("connectBtn")
        self.connect_btn.clicked.connect(self.toggle_connection)
        self._set_connect_style(connected=False)
        header.addWidget(self.connect_btn)

        main_layout.addLayout(header)

        # Tabs: Live dashboard | on-device History browser
        self.tabs = QTabWidget()
        self.tabs.addTab(self._build_live_tab(), "Live")
        self.tabs.addTab(self._build_history_tab(), "History")
        main_layout.addWidget(self.tabs, stretch=1)

        # Status bar
        self.status_bar = QStatusBar()
        self.setStatusBar(self.status_bar)

        self.connection_status = QLabel("Disconnected")
        self.connection_status.setStyleSheet(f"color: {Theme.CRIT}; font-weight: bold;")
        self.status_bar.addWidget(self.connection_status)

        self.sample_status = QLabel("Samples: 0")
        self.sample_status.setStyleSheet(f"color: {Theme.MUTED};")
        self.status_bar.addPermanentWidget(self.sample_status)

        self.csv_status = QLabel("")
        self.status_bar.addPermanentWidget(self.csv_status)

    def _make_plot_card(self, canvas):
        card = QFrame()
        card.setObjectName("plotCard")
        card.setStyleSheet(f"""
            QFrame#plotCard {{
                background-color: {Theme.CARD};
                border: 1px solid {Theme.CARD_BORDER};
                border-radius: 10px;
            }}
        """)
        layout = QVBoxLayout(card)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.addWidget(canvas)
        return card

    def _build_live_tab(self):
        tab = QWidget()
        layout = QVBoxLayout(tab)
        layout.setContentsMargins(0, 10, 0, 0)
        layout.setSpacing(12)

        # Sensor tiles
        self.sensor_display = SensorDisplay()
        layout.addWidget(self.sensor_display)

        # Plot card
        self.canvas = PlotCanvas()
        layout.addWidget(self._make_plot_card(self.canvas), stretch=1)

        # Footer: CSV logging + live buffer size
        footer = QHBoxLayout()
        footer.setSpacing(10)

        self.csv_checkbox = QCheckBox("Log to CSV")
        self.csv_checkbox.stateChanged.connect(self.toggle_csv_logging)
        footer.addWidget(self.csv_checkbox)

        self.csv_path_label = QLabel("No file selected")
        self.csv_path_label.setStyleSheet(f"color: {Theme.MUTED}; font-style: italic;")
        footer.addWidget(self.csv_path_label)

        self.csv_browse_btn = QPushButton("Browse…")
        self.csv_browse_btn.clicked.connect(self.browse_csv)
        footer.addWidget(self.csv_browse_btn)

        footer.addStretch()

        footer.addWidget(self._muted_label("BUFFER"))
        self.history_spin = QSpinBox()
        self.history_spin.setRange(50, 1000)
        self.history_spin.setValue(300)
        self.history_spin.setSuffix(" pts")
        self.history_spin.valueChanged.connect(self.update_max_points)
        footer.addWidget(self.history_spin)

        layout.addLayout(footer)
        return tab

    def _build_history_tab(self):
        tab = QWidget()
        layout = QVBoxLayout(tab)
        layout.setContentsMargins(0, 10, 0, 0)
        layout.setSpacing(12)

        # Controls row
        controls = QHBoxLayout()
        controls.setSpacing(10)

        self.fetch_btn = QPushButton("Fetch from device")
        self.fetch_btn.setEnabled(False)
        self.fetch_btn.clicked.connect(self.start_history_fetch)
        controls.addWidget(self.fetch_btn)

        self.history_progress = QProgressBar()
        self.history_progress.setMaximumWidth(260)
        self.history_progress.setTextVisible(False)
        self.history_progress.hide()
        controls.addWidget(self.history_progress)

        self.history_status = QLabel("Connect to a device, then fetch its stored history "
                                     "(up to 7 days, 5-minute resolution).")
        self.history_status.setStyleSheet(f"color: {Theme.MUTED};")
        controls.addWidget(self.history_status)

        controls.addStretch()

        self.history_export_btn = QPushButton("Export CSV…")
        self.history_export_btn.setEnabled(False)
        self.history_export_btn.clicked.connect(self.export_history_csv)
        controls.addWidget(self.history_export_btn)

        layout.addLayout(controls)

        # History charts
        self.history_canvas = HistoryPlotCanvas()
        layout.addWidget(self._make_plot_card(self.history_canvas), stretch=1)
        return tab

    @staticmethod
    def _muted_label(text):
        label = QLabel(text)
        label.setStyleSheet(
            f"color: {Theme.MUTED}; font-size: 10px; font-weight: 600; letter-spacing: 1px;"
        )
        return label

    def _set_connect_style(self, connected):
        color, hover = ("#da3633", "#f85149") if connected else ("#238636", "#2ea043")
        self.connect_btn.setText("Disconnect" if connected else "Connect")
        self.connect_btn.setStyleSheet(f"""
            QPushButton#connectBtn {{
                background-color: {color};
                color: white;
                border: none;
                padding: 8px 16px;
                border-radius: 6px;
                font-weight: bold;
            }}
            QPushButton#connectBtn:hover {{ background-color: {hover}; }}
            QPushButton#connectBtn:disabled {{ background-color: #30363d; color: {Theme.MUTED}; }}
        """)

    def _set_model_badge(self, model):
        if model == "pro":
            self.model_badge.setText("PRO")
            self.model_badge.setStyleSheet(f"""
                color: #0d1117; background-color: {Theme.LUX};
                border-radius: 4px; padding: 2px 8px;
            """)
        else:
            self.model_badge.setText("BASE")
            self.model_badge.setStyleSheet(f"""
                color: {Theme.TEXT}; background-color: #30363d;
                border-radius: 4px; padding: 2px 8px;
            """)
        self.model_badge.show()

    # -- Timers / ports -------------------------------------------------------

    def setup_timers(self):
        self.plot_timer = QTimer()
        self.plot_timer.timeout.connect(self.update_plot)
        self.plot_timer.start(500)

    def refresh_ports(self):
        self.port_combo.clear()
        ports = list_ports.comports()
        for p in ports:
            self.port_combo.addItem(f"{p.device} — {p.description}", p.device)
        if not ports:
            self.port_combo.addItem("No ports found", None)

    # -- Connection -----------------------------------------------------------

    def toggle_connection(self):
        if self.serial_thread and self.serial_thread.running:
            self.disconnect_serial()
        else:
            self.connect_serial()

    def connect_serial(self):
        port = self.port_combo.currentData()
        if not port:
            QMessageBox.warning(self, "No Port", "Please select a serial port.")
            return

        self.serial_thread = SerialReaderThread(port)
        self.serial_thread.data_received.connect(self.on_data_received)
        self.serial_thread.history_info_received.connect(self.on_history_info)
        self.serial_thread.history_received.connect(self.on_history_page)
        self.serial_thread.response_received.connect(self.on_command_response)
        self.serial_thread.error_occurred.connect(self.on_serial_error)
        self.serial_thread.start()

        self._set_connect_style(connected=True)
        self.port_combo.setEnabled(False)
        self.refresh_btn.setEnabled(False)
        self.fetch_btn.setEnabled(True)

        self.connection_status.setText(f"Connected to {port}")
        self.connection_status.setStyleSheet(f"color: {Theme.GOOD}; font-weight: bold;")

        # Reset data
        self.data_buffer.clear()
        self.t0 = None
        self.sample_count = 0
        self.model = None
        self.sensor_display.clear_values()

    def disconnect_serial(self):
        self._abort_history_fetch("Disconnected.")
        if self.serial_thread:
            self.serial_thread.stop()
            self.serial_thread = None

        self._set_connect_style(connected=False)
        self.port_combo.setEnabled(True)
        self.refresh_btn.setEnabled(True)
        self.fetch_btn.setEnabled(False)

        self.connection_status.setText("Disconnected")
        self.connection_status.setStyleSheet(f"color: {Theme.CRIT}; font-weight: bold;")

    # -- Data handling ----------------------------------------------------------

    def on_data_received(self, data):
        ts = data.get("timestamp")
        if ts is None:
            return

        try:
            ts = float(ts)
        except (TypeError, ValueError):
            return

        if self.t0 is None:
            self.t0 = ts

        # Convert to seconds (firmware sends ms)
        t_rel = (ts - self.t0) / 1000.0 if ts > 1000 else (ts - self.t0)

        # Detect model change (first packet, or device swap)
        model = data.get("model", "base")
        if model != self.model:
            self.model = model
            is_pro = model == "pro"
            self._set_model_badge(model)
            self.sensor_display.set_pro_visible(is_pro)
            self.canvas.set_pro_mode(is_pro)

        def as_float(value):
            if value is None:
                return math.nan
            try:
                return float(value)
            except (TypeError, ValueError):
                return math.nan

        temp_c = as_float(data.get("temperature_c"))
        hum = as_float(data.get("humidity"))
        aqi = as_float(data.get("aqi"))
        if math.isnan(temp_c) or math.isnan(hum) or math.isnan(aqi):
            return

        point = {
            "t": t_rel,
            "temp": temp_c,
            "hum": hum,
            "aqi": aqi,
            "eco2": as_float(data.get("eco2")),
            "etvoc": as_float(data.get("etvoc")),
            "co2": as_float(data.get("co2")),
            "lux": as_float(data.get("lux")),
        }
        self.data_buffer.append(point)
        self.sample_count += 1
        self.sample_status.setText(f"Samples: {self.sample_count}")

        self.sensor_display.update_values(data)

        if self.csv_writer:
            row = [
                data.get("timestamp"),
                data.get("ens210_status"),
                data.get("temperature_c"),
                data.get("temperature_f"),
                data.get("humidity"),
                data.get("ens16x_status"),
                data.get("etvoc"),
                data.get("eco2"),
                data.get("aqi"),
                data.get("aqi_s"),
                data.get("aqi_uba"),
                data.get("model"),
                data.get("co2"),
                data.get("lux"),
            ]
            self.csv_writer.writerow(row)
            self.csv_file.flush()

    def on_serial_error(self, error):
        QMessageBox.critical(self, "Serial Error", f"Serial connection error:\n{error}")
        self.disconnect_serial()

    def update_plot(self):
        if not self.data_buffer:
            return

        x = [p["t"] for p in self.data_buffer]
        series = {field: [p[field] for p in self.data_buffer] for field in FIELDS}
        self.canvas.update_plot(x, series)

    def update_max_points(self, value):
        self.max_points = value
        old_data = list(self.data_buffer)
        self.data_buffer = collections.deque(old_data[-value:], maxlen=value)

    # -- On-device history fetch ------------------------------------------------

    def start_history_fetch(self):
        if self.history_fetching or not self.serial_thread:
            return
        self.history_fetching = True
        self.history_slots = []
        self.history_expected = 0
        self.history_next_start = 0
        self.history_retries = 0
        self.history_fetch_time = datetime.now()

        self.fetch_btn.setEnabled(False)
        self.history_export_btn.setEnabled(False)
        self.history_progress.setRange(0, 0)  # indeterminate until info arrives
        self.history_progress.show()
        self._set_history_status("Requesting history info…")

        self._send_history_command({"cmd": "get_history_info"})

    def _send_history_command(self, command):
        self.history_timeout.stop()
        if self.serial_thread and self.serial_thread.send_command(command):
            self._pending_command = command
            self.history_timeout.start(HISTORY_REQUEST_TIMEOUT_MS)
        else:
            self._abort_history_fetch("Failed to send command to device.")

    def _history_request_timed_out(self):
        if not self.history_fetching:
            return
        if self.history_retries < HISTORY_MAX_RETRIES:
            self.history_retries += 1
            self._set_history_status(
                f"No response, retrying ({self.history_retries}/{HISTORY_MAX_RETRIES})…")
            self._send_history_command(self._pending_command)
        else:
            self._abort_history_fetch("Device did not respond.")

    def on_history_info(self, info):
        if not self.history_fetching:
            return
        self.history_timeout.stop()
        self.history_retries = 0

        entries = int(info.get("entries", 0))
        window_us = info.get("window_us")
        if window_us:
            try:
                self.history_window_s = float(window_us) / 1e6
            except (TypeError, ValueError):
                pass

        if entries <= 0:
            self._finish_history_fetch("Device has no stored history yet.")
            return

        self.history_expected = entries
        self.history_progress.setRange(0, entries)
        self.history_progress.setValue(0)
        self._set_history_status(f"Fetching {entries} entries…")
        self._request_next_history_page()

    def _request_next_history_page(self):
        remaining = self.history_expected - self.history_next_start
        count = min(HISTORY_PAGE_SIZE, remaining)
        self._send_history_command(
            {"cmd": "get_history", "start": self.history_next_start, "count": count})

    def on_history_page(self, data):
        if not self.history_fetching:
            return
        self.history_timeout.stop()
        self.history_retries = 0

        slots = data.get("history") or []
        count = int(data.get("count", len(slots)))
        start = int(data.get("start", self.history_next_start))

        # Guard against stale/duplicate pages
        if start != self.history_next_start:
            self._request_next_history_page()
            return
        if count <= 0:
            # Device produced an empty page (should not happen mid-range) - stop
            self._finish_history_fetch(None)
            return

        self.history_slots.extend(slots[:count])
        self.history_next_start += count
        self.history_progress.setValue(min(self.history_next_start, self.history_expected))

        if self.history_next_start >= self.history_expected:
            self._finish_history_fetch(None)
        else:
            self._request_next_history_page()

    def on_command_response(self, response):
        # Errors during a fetch (e.g. "start index out of range" after a
        # concurrent clear) abort the fetch cleanly.
        if self.history_fetching and response.get("status") == "error":
            self._abort_history_fetch(f"Device error: {response.get('msg', 'unknown')}")

    def _finish_history_fetch(self, message):
        self.history_timeout.stop()
        self.history_fetching = False
        self.history_progress.hide()
        self.fetch_btn.setEnabled(True)

        n = len(self.history_slots)
        if n == 0:
            self._set_history_status(message or "No history data received.")
            return

        self._plot_history()
        self.history_export_btn.setEnabled(True)
        span_h = n * self.history_window_s / 3600.0
        self._set_history_status(
            f"{n} entries fetched ({span_h:.1f} h of data) at "
            f"{self.history_fetch_time.strftime('%H:%M:%S')}.")

    def _abort_history_fetch(self, reason):
        if not self.history_fetching:
            return
        self.history_timeout.stop()
        self.history_fetching = False
        self.history_progress.hide()
        self.fetch_btn.setEnabled(self.serial_thread is not None)
        self._set_history_status(reason)

    def _set_history_status(self, text):
        self.history_status.setText(text)

    def _history_times(self):
        """Wall-clock timestamps: newest slot anchors to fetch time."""
        n = len(self.history_slots)
        window = timedelta(seconds=self.history_window_s)
        return [self.history_fetch_time - (n - 1 - i) * window for i in range(n)]

    @staticmethod
    def _slot_values(slot, keys, scale=1.0):
        """Extract (avg, min, max) from a slot dict; None slot -> NaNs."""
        if not slot:
            return (math.nan, math.nan, math.nan)
        try:
            return tuple(float(slot[k]) * scale for k in keys)
        except (KeyError, TypeError, ValueError):
            return (math.nan, math.nan, math.nan)

    def _history_series(self):
        """Build field -> (avg[], min[], max[]) from accumulated slots."""
        spec = {
            "temp": (("t_a", "t_n", "t_x"), 0.01),
            "hum": (("h_a", "h_n", "h_x"), 0.01),
            "voc": (("q_a", "q_n", "q_x"), 1.0),
            "co2": (("c_a", "c_n", "c_x"), 1.0),
            "etvoc": (("v_a", "v_n", "v_x"), 1.0),
        }
        series = {}
        for field, (keys, scale) in spec.items():
            avg, lo, hi = [], [], []
            for slot in self.history_slots:
                a, n_, x = self._slot_values(slot, keys, scale)
                avg.append(a)
                lo.append(n_)
                hi.append(x)
            series[field] = (avg, lo, hi)
        return series

    def _plot_history(self):
        times = self._history_times()
        series = self._history_series()
        self.history_canvas.update_history(times, series, pro=(self.model == "pro"))

    def export_history_csv(self):
        if not self.history_slots:
            return
        default_name = f"aircube_history_{self.history_fetch_time.strftime('%Y%m%d_%H%M%S')}.csv"
        path, _ = QFileDialog.getSaveFileName(
            self, "Export History CSV", default_name, "CSV Files (*.csv)")
        if not path:
            return

        times = self._history_times()
        try:
            with open(path, "w", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(HISTORY_CSV_HEADER)
                for ts, slot in zip(times, self.history_slots):
                    if not slot:
                        continue
                    writer.writerow([
                        ts.isoformat(timespec="seconds"),
                        slot.get("seq"),
                        *(round(slot.get(k, 0) * 0.01, 2) for k in ("t_a", "t_n", "t_x")),
                        *(round(slot.get(k, 0) * 0.01, 2) for k in ("h_a", "h_n", "h_x")),
                        *(slot.get(k) for k in ("q_a", "q_n", "q_x")),
                        *(slot.get(k) for k in ("c_a", "c_n", "c_x")),
                        *(slot.get(k) for k in ("v_a", "v_n", "v_x")),
                    ])
        except OSError as e:
            QMessageBox.critical(self, "Export Failed", f"Could not write CSV:\n{e}")
            return
        self._set_history_status(f"Exported {len(self.history_slots)} entries to "
                                 f"{os.path.basename(path)}.")

    # -- CSV logging ------------------------------------------------------------

    def toggle_csv_logging(self, state):
        if state == Qt.CheckState.Checked.value:
            if not self.csv_path:
                self.browse_csv()
                if not self.csv_path:
                    self.csv_checkbox.setChecked(False)
                    return
            self.start_csv_logging()
        else:
            self.stop_csv_logging()

    def browse_csv(self):
        default_name = f"aircube_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        path, _ = QFileDialog.getSaveFileName(
            self, "Save CSV Log", default_name, "CSV Files (*.csv)"
        )
        if path:
            self.csv_path = path
            self.csv_path_label.setText(os.path.basename(path))
            self.csv_path_label.setStyleSheet(f"color: {Theme.TEXT};")

    def start_csv_logging(self):
        if not self.csv_path:
            return

        new_file = not os.path.exists(self.csv_path) or os.path.getsize(self.csv_path) == 0
        self.csv_file = open(self.csv_path, "a", newline="")
        self.csv_writer = csv.writer(self.csv_file)

        if new_file:
            self.csv_writer.writerow(CSV_HEADER)
            self.csv_file.flush()

        self.csv_status.setText(f"Logging to {os.path.basename(self.csv_path)}")
        self.csv_status.setStyleSheet(f"color: {Theme.GOOD};")

    def stop_csv_logging(self):
        if self.csv_file:
            self.csv_file.close()
            self.csv_file = None
            self.csv_writer = None
        self.csv_status.setText("")

    def closeEvent(self, event):
        self.disconnect_serial()
        self.stop_csv_logging()
        event.accept()


def main():
    app = QApplication(sys.argv)
    app.setStyle("Fusion")

    font = QFont("Segoe UI", 10)
    app.setFont(font)

    # Dark, modern application-wide stylesheet
    app.setStyleSheet(f"""
        QMainWindow, QWidget {{
            background-color: {Theme.BG};
            color: {Theme.TEXT};
        }}
        QComboBox, QSpinBox {{
            padding: 6px 8px;
            border: 1px solid {Theme.CARD_BORDER};
            border-radius: 6px;
            background-color: {Theme.CARD};
            color: {Theme.TEXT};
            selection-background-color: {Theme.ACCENT};
        }}
        QComboBox:hover, QSpinBox:hover {{
            border-color: {Theme.MUTED};
        }}
        QComboBox QAbstractItemView {{
            background-color: {Theme.CARD};
            color: {Theme.TEXT};
            border: 1px solid {Theme.CARD_BORDER};
            selection-background-color: {Theme.ACCENT};
        }}
        QPushButton {{
            background-color: #21262d;
            color: {Theme.TEXT};
            border: 1px solid {Theme.CARD_BORDER};
            padding: 6px 12px;
            border-radius: 6px;
        }}
        QPushButton:hover {{
            background-color: #30363d;
            border-color: {Theme.MUTED};
        }}
        QCheckBox {{
            spacing: 8px;
            color: {Theme.TEXT};
        }}
        QCheckBox::indicator {{
            width: 16px; height: 16px;
            border: 1px solid {Theme.CARD_BORDER};
            border-radius: 4px;
            background-color: {Theme.CARD};
        }}
        QCheckBox::indicator:checked {{
            background-color: {Theme.ACCENT};
            border-color: {Theme.ACCENT};
        }}
        QStatusBar {{
            background-color: {Theme.CARD};
            border-top: 1px solid {Theme.CARD_BORDER};
        }}
        QTabWidget::pane {{
            border: none;
        }}
        QTabBar::tab {{
            background: transparent;
            color: {Theme.MUTED};
            padding: 7px 18px;
            border: none;
            border-bottom: 2px solid transparent;
            font-weight: 600;
        }}
        QTabBar::tab:selected {{
            color: {Theme.TEXT};
            border-bottom: 2px solid {Theme.ACCENT};
        }}
        QTabBar::tab:hover:!selected {{
            color: {Theme.TEXT};
        }}
        QProgressBar {{
            background-color: {Theme.CARD};
            border: 1px solid {Theme.CARD_BORDER};
            border-radius: 5px;
            height: 10px;
        }}
        QProgressBar::chunk {{
            background-color: {Theme.ACCENT};
            border-radius: 4px;
        }}
        QToolTip {{
            background-color: {Theme.CARD};
            color: {Theme.TEXT};
            border: 1px solid {Theme.CARD_BORDER};
        }}
    """)

    window = AirCubeApp()
    window.show()

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
