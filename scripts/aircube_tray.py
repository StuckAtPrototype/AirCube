"""
AirCube Tray Monitor
A lightweight system tray app that displays AQI in the Windows taskbar.
"""

__version__ = "1.0.2"
__app_name__ = "AirCube Tray"

import collections
import csv
import json
import os
import re
import sys
import time
from datetime import datetime, timedelta
from typing import Optional

# Hide console window on Windows when running as a script
if sys.platform == 'win32':
    import ctypes
    # Get console window handle and hide it
    hwnd = ctypes.windll.kernel32.GetConsoleWindow()
    if hwnd:
        ctypes.windll.user32.ShowWindow(hwnd, 0)  # SW_HIDE = 0

    from PyQt6.QtWidgets import (
    QApplication, QSystemTrayIcon, QMenu, QDialog, QVBoxLayout,
    QHBoxLayout, QLabel, QComboBox, QPushButton, QCheckBox,
    QSpinBox, QGroupBox, QMessageBox, QWidget, QFrame, QGridLayout,
    QFileDialog, QTextEdit, QStackedWidget, QProgressBar
)
from PyQt6.QtCore import QTimer, Qt, QThread, pyqtSignal, QSettings, QPoint, QStandardPaths
from PyQt6.QtGui import QIcon, QPixmap, QImage, QPainter, QFont, QColor, QAction, QCursor

import matplotlib
matplotlib.use('QtAgg')
from matplotlib.backends.backend_qtagg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure
import matplotlib.dates as mdates

import serial
from serial.tools import list_ports

# JSON pattern for parsing sensor data
JSON_PATTERN = re.compile(r"\{.*\}")
MAX_HISTORY = 1000

# Device history metric configurations for the popup chart
METRIC_CONFIGS = {
    'aqi':   {'avg': 'q_a', 'min': 'q_n', 'max': 'q_x', 'label': 'AQI',         'color': '#4CAF50', 'unit': '',    'scale': 1},
    'temp':  {'avg': 't_a', 'min': 't_n', 'max': 't_x', 'label': 'Temperature', 'color': '#FF9800', 'unit': '°C',  'scale': 0.01},
    'hum':   {'avg': 'h_a', 'min': 'h_n', 'max': 'h_x', 'label': 'Humidity',    'color': '#2196F3', 'unit': '%',   'scale': 0.01},
    'eco2':  {'avg': 'c_a', 'min': 'c_n', 'max': 'c_x', 'label': 'eCO2',        'color': '#9C27B0', 'unit': 'ppm', 'scale': 1},
    'etvoc': {'avg': 'v_a', 'min': 'v_n', 'max': 'v_x', 'label': 'eTVOC',       'color': '#009688', 'unit': 'ppb', 'scale': 1},
}


def parse_json_line(line: str) -> Optional[dict]:
    """Parse a JSON sensor data line into a flat dict."""
    match = JSON_PATTERN.search(line)
    if not match:
        return None
    try:
        data = json.loads(match.group(0))
        return {
            "timestamp": data.get("timestamp"),
            "temperature_c": data["ens210"].get("temperature_c"),
            "humidity": data["ens210"].get("humidity"),
            "aqi": data["ens16x"].get("aqi"),
            "aqi_uba": data["ens16x"].get("aqi_uba"),
            "eco2": data["ens16x"].get("eco2"),
            "etvoc": data["ens16x"].get("etvoc"),
        }
    except (KeyError, TypeError, json.JSONDecodeError):
        return None


def get_aqi_color(aqi: float) -> QColor:
    """Get color based on ScioSense AQI-S value (0-500)."""
    if aqi <= 50:
        return QColor("#4CAF50")  # Green - Excellent
    elif aqi <= 100:
        return QColor("#8BC34A")  # Light green - Good
    elif aqi <= 150:
        return QColor("#FFC107")  # Yellow - Moderate
    elif aqi <= 200:
        return QColor("#FF9800")  # Orange - Poor
    elif aqi <= 300:
        return QColor("#F44336")  # Red - Unhealthy
    else:
        return QColor("#9C27B0")  # Purple - Hazardous


def get_aqi_uba_label(aqi_uba: int) -> str:
    """Get text label for AQI-UBA level (1-5, from ENS161 register 0x21)."""
    labels = {
        1: "Excellent",
        2: "Good",
        3: "Moderate",
        4: "Poor",
        5: "Unhealthy",
    }
    return labels.get(aqi_uba, "--")


def create_aqi_icon(aqi: Optional[float] = None, connected: bool = False) -> QIcon:
    """Generate a colored icon with AQI number."""
    size = 64
    img = QImage(size, size, QImage.Format.Format_ARGB32)
    img.fill(Qt.GlobalColor.transparent)
    
    painter = QPainter(img)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    
    if not connected:
        # Gray icon when disconnected
        painter.setBrush(QColor("#9E9E9E"))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(2, 2, size - 4, size - 4, 8, 8)
        painter.setPen(QColor("white"))
        painter.setFont(QFont("Segoe UI", 20, QFont.Weight.Bold))
        painter.drawText(img.rect(), Qt.AlignmentFlag.AlignCenter, "—")
    elif aqi is not None:
        # Colored icon with AQI value
        color = get_aqi_color(aqi)
        painter.setBrush(color)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(2, 2, size - 4, size - 4, 8, 8)
        
        # Draw text
        painter.setPen(QColor("white"))
        aqi_int = int(aqi)
        if aqi_int < 100:
            painter.setFont(QFont("Segoe UI", 28, QFont.Weight.Bold))
        else:
            painter.setFont(QFont("Segoe UI", 22, QFont.Weight.Bold))
        painter.drawText(img.rect(), Qt.AlignmentFlag.AlignCenter, str(aqi_int))
    else:
        # Connected but no data yet
        painter.setBrush(QColor("#2196F3"))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(2, 2, size - 4, size - 4, 8, 8)
        painter.setPen(QColor("white"))
        painter.setFont(QFont("Segoe UI", 18, QFont.Weight.Bold))
        painter.drawText(img.rect(), Qt.AlignmentFlag.AlignCenter, "...")
    
    painter.end()
    return QIcon(QPixmap.fromImage(img))


class SerialReaderThread(QThread):
    """Background thread for reading serial data."""
    data_received = pyqtSignal(dict)
    command_response = pyqtSignal(dict)
    error_occurred = pyqtSignal(str)
    connected = pyqtSignal()
    raw_line = pyqtSignal(str)  # Every line from serial (for debug console)
    
    def __init__(self, port: str, baud: int = 115200):
        super().__init__()
        self.port = port
        self.baud = baud
        self.running = False
        self.serial = None
    
    def run(self):
        try:
            self.serial = serial.Serial(self.port, self.baud, timeout=0.5)
            self.running = True
            self.connected.emit()
            
            while self.running:
                try:
                    line = self.serial.readline()
                    if line:
                        decoded = line.decode(errors="ignore").strip()
                        if decoded:
                            self.raw_line.emit(f"RX: {decoded}")
                        # Try to parse as JSON
                        match = JSON_PATTERN.search(decoded)
                        if match:
                            try:
                                data = json.loads(match.group(0))
                            except json.JSONDecodeError:
                                continue
                            
                            # Route: sensor data has "ens210" key, everything else is a command response
                            if "ens210" in data:
                                parsed = parse_json_line(decoded)
                                if parsed:
                                    self.data_received.emit(parsed)
                            else:
                                self.command_response.emit(data)
                except (serial.SerialException, OSError) as e:
                    if self.running:
                        self.error_occurred.emit(str(e))
                    break
        except serial.SerialException as e:
            self.error_occurred.emit(str(e))
        finally:
            if self.serial and self.serial.is_open:
                self.serial.close()
    
    def send_command(self, cmd_json: str):
        """Send a JSON command string to the device. Thread-safe (pyserial write)."""
        if self.serial and self.serial.is_open:
            try:
                self.raw_line.emit(f"TX: {cmd_json}")
                self.serial.write((cmd_json + "\n").encode())
                self.serial.flush()  # Force data out of OS buffer to USB endpoint
            except (serial.SerialException, OSError) as e:
                self.raw_line.emit(f"TX ERROR: {e}")
    
    def stop(self):
        self.running = False
        self.wait(2000)


class MiniChart(FigureCanvas):
    """Small matplotlib chart for the popup."""
    def __init__(self, parent=None, width=4.5, height=3):
        self.fig = Figure(figsize=(width, height), dpi=100)
        self.fig.set_facecolor('#2b2b2b')
        super().__init__(self.fig)
        self.setParent(parent)
        
        self.ax = self.fig.add_subplot(111)
        self.ax.set_facecolor('#2b2b2b')
        self.ax.tick_params(colors='white', labelsize=8)
        self.ax.spines['bottom'].set_color('#555')
        self.ax.spines['top'].set_color('#555')
        self.ax.spines['left'].set_color('#555')
        self.ax.spines['right'].set_color('#555')
        self.fig.tight_layout(pad=1.0)
        
        # Hover tooltip
        self._annot = self.ax.annotate("", xy=(0, 0), xytext=(10, 10),
            textcoords="offset points",
            bbox=dict(boxstyle="round,pad=0.3", fc="#333", ec="#666", alpha=0.9),
            color="white", fontsize=8, zorder=100)
        self._annot.set_visible(False)
        self._hover_data = None   # (x_vals, y_vals, fmt_func)
        self._hover_extra = None  # optional (min_vals, max_vals) for history
        self._scatter_dot = None  # highlight dot on hover
        self.mpl_connect("motion_notify_event", self._on_hover)
    
    def _on_hover(self, event):
        """Show tooltip when hovering over chart data."""
        if event.inaxes != self.ax or self._hover_data is None:
            if self._annot.get_visible():
                self._annot.set_visible(False)
                if self._scatter_dot:
                    self._scatter_dot.set_visible(False)
                self.draw_idle()
            return
        
        x_vals, y_vals, fmt_func = self._hover_data
        if not x_vals or not y_vals:
            return
        
        # Find nearest data point by x position
        import numpy as np
        x_arr = np.array([mdates.date2num(t) if isinstance(t, datetime) else t for t in x_vals])
        idx = int(np.argmin(np.abs(x_arr - event.xdata)))
        
        x_pt = x_vals[idx]
        y_pt = y_vals[idx]
        
        # Skip NaN values
        if isinstance(y_pt, float) and y_pt != y_pt:
            self._annot.set_visible(False)
            if self._scatter_dot:
                self._scatter_dot.set_visible(False)
            self.draw_idle()
            return
        
        # Position annotation
        x_plot = mdates.date2num(x_pt) if isinstance(x_pt, datetime) else x_pt
        self._annot.xy = (x_plot, y_pt)
        
        # Build tooltip text
        text = fmt_func(idx, x_pt, y_pt)
        if self._hover_extra:
            min_vals, max_vals = self._hover_extra
            text += f"\nMin: {min_vals[idx]:.1f}  Max: {max_vals[idx]:.1f}"
        
        self._annot.set_text(text)
        self._annot.set_visible(True)
        
        # Show highlight dot
        if self._scatter_dot:
            self._scatter_dot.set_visible(False)
        self._scatter_dot = self.ax.scatter([x_plot], [y_pt], color='white',
                                            s=25, zorder=99, edgecolors='#888', linewidths=0.5)
        
        self.draw_idle()
    
    def update_chart(self, time_data, value_data, label='AQI', color='#4CAF50', unit=''):
        self.ax.cla()
        self.ax.set_facecolor('#2b2b2b')
        self._scatter_dot = None
        self._hover_extra = None
        
        if time_data and value_data:
            self.ax.plot(time_data, value_data, color=color, linewidth=1.5)
            self.ax.fill_between(time_data, value_data, alpha=0.3, color=color)
            
            # Format x-axis as time
            self.ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S'))
            self.ax.xaxis.set_major_locator(mdates.AutoDateLocator())
            self.fig.autofmt_xdate(rotation=45, ha='right')
            
            # Store hover data
            unit_str = f" {unit}" if unit else ''
            def fmt_live(idx, x, y):
                return f"{x.strftime('%H:%M:%S')}\n{label}: {y:.1f}{unit_str}"
            self._hover_data = (list(time_data), list(value_data), fmt_live)
        else:
            self._hover_data = None
        
        ylabel = f'{label} ({unit})' if unit else label
        self.ax.set_ylabel(ylabel, color='white', fontsize=9)
        self.ax.set_xlabel('Time', color='white', fontsize=9)
        self.ax.tick_params(colors='white', labelsize=7)
        self.ax.grid(True, linestyle='--', alpha=0.3, color='#555')
        
        # Re-create annotation after axes clear
        self._annot = self.ax.annotate("", xy=(0, 0), xytext=(10, 10),
            textcoords="offset points",
            bbox=dict(boxstyle="round,pad=0.3", fc="#333", ec="#666", alpha=0.9),
            color="white", fontsize=8, zorder=100)
        self._annot.set_visible(False)
        
        self.fig.tight_layout(pad=1.5)
        self.draw()
    
    def update_history(self, x_data, avg_data, min_data, max_data, label, color, unit=''):
        """Update chart with device history data (avg line + min/max range band)."""
        self.ax.cla()
        self.ax.set_facecolor('#2b2b2b')
        self._scatter_dot = None
        self._hover_extra = None
        
        if x_data and avg_data:
            # Determine x-axis scale based on time range
            range_val = max(x_data) if x_data else 0
            if range_val < 1:
                x_plot = [v * 60 for v in x_data]
                x_label = 'Minutes ago'
                x_unit = 'min'
            elif range_val < 48:
                x_plot = list(x_data)
                x_label = 'Hours ago'
                x_unit = 'h'
            else:
                x_plot = [v / 24 for v in x_data]
                x_label = 'Days ago'
                x_unit = 'd'
            
            self.ax.plot(x_plot, avg_data, color=color, linewidth=1.5, label='Average')
            if min_data and max_data:
                self.ax.fill_between(x_plot, min_data, max_data,
                                    alpha=0.15, color=color, label='Min / Max')
            
            self.ax.legend(loc='upper left', fontsize=7,
                          facecolor='#2b2b2b', edgecolor='#555',
                          labelcolor='white')
            
            # Invert x so newest (0) is on the right
            self.ax.invert_xaxis()
            self.ax.set_xlabel(x_label, color='white', fontsize=9)
            
            # Store hover data
            unit_str = f" {unit}" if unit else ''
            def fmt_hist(idx, x, y, _xu=x_unit, _us=unit_str, _lbl=label):
                return f"{x:.1f} {_xu} ago\n{_lbl}: {y:.1f}{_us}"
            self._hover_data = (list(x_plot), list(avg_data), fmt_hist)
            if min_data and max_data:
                self._hover_extra = (list(min_data), list(max_data))
        else:
            self.ax.text(0.5, 0.5, 'No history data', transform=self.ax.transAxes,
                        ha='center', va='center', color='#666', fontsize=11)
            self.ax.set_xlabel('', color='white', fontsize=9)
            self._hover_data = None
        
        ylabel = f'{label} ({unit})' if unit else label
        self.ax.set_ylabel(ylabel, color='white', fontsize=9)
        self.ax.tick_params(colors='white', labelsize=7)
        self.ax.grid(True, linestyle='--', alpha=0.3, color='#555')
        
        # Re-create annotation after axes clear
        self._annot = self.ax.annotate("", xy=(0, 0), xytext=(10, 10),
            textcoords="offset points",
            bbox=dict(boxstyle="round,pad=0.3", fc="#333", ec="#666", alpha=0.9),
            color="white", fontsize=8, zorder=100)
        self._annot.set_visible(False)
        
        self.fig.tight_layout(pad=1.5)
        self.draw()


class PopupWindow(QWidget):
    """Popup window showing all sensor data and a chart."""
    def __init__(self, parent=None):
        super().__init__(parent, Qt.WindowType.Popup | Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setMinimumSize(500, 540)
        self._history_slots = []
        self._window_us = 300_000_000
        self._selected_live_metric = 'aqi'
        self._selected_history_metric = 'aqi'
        self._live_range = None       # None = all, else minutes
        self._history_range = None    # None = all, else minutes
        self._use_fahrenheit = True
        self._history_loading = False
        self._history_fetched_at = None
        self._live_histories = {}
        self._live_sample_count = 0
        self.setup_ui()
    
    def setup_ui(self):
        # Main container with rounded corners
        container = QFrame(self)
        container.setStyleSheet("""
            QFrame {
                background-color: #2b2b2b;
                border-radius: 12px;
                border: 1px solid #444;
            }
            QLabel {
                color: white;
            }
        """)
        
        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.addWidget(container)
        
        layout = QVBoxLayout(container)
        layout.setContentsMargins(15, 15, 15, 15)
        layout.setSpacing(10)
        
        # Title
        title = QLabel("AirCube Monitor")
        title.setFont(QFont("Segoe UI", 14, QFont.Weight.Bold))
        title.setStyleSheet("color: white;")
        layout.addWidget(title)
        
        # Sensor values grid
        grid = QGridLayout()
        grid.setSpacing(15)
        
        # AQI (large)
        self.aqi_value = QLabel("--")
        self.aqi_value.setFont(QFont("Segoe UI", 36, QFont.Weight.Bold))
        self.aqi_value.setAlignment(Qt.AlignmentFlag.AlignCenter)
        
        self.aqi_label = QLabel("Air Quality Index")
        self.aqi_label.setStyleSheet("color: #aaa;")
        self.aqi_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        
        self.aqi_status = QLabel("")
        self.aqi_status.setFont(QFont("Segoe UI", 10, QFont.Weight.Bold))
        self.aqi_status.setAlignment(Qt.AlignmentFlag.AlignCenter)
        
        aqi_box = QVBoxLayout()
        aqi_box.addWidget(self.aqi_value)
        aqi_box.addWidget(self.aqi_status)
        aqi_box.addWidget(self.aqi_label)
        grid.addLayout(aqi_box, 0, 0, 2, 1)
        
        # Other values
        self.temp_value = QLabel("--.-°F")
        self.temp_value.setFont(QFont("Segoe UI", 16, QFont.Weight.Bold))
        temp_label = QLabel("Temperature")
        temp_label.setStyleSheet("color: #aaa; font-size: 10px;")
        
        self.hum_value = QLabel("--.--%")
        self.hum_value.setFont(QFont("Segoe UI", 16, QFont.Weight.Bold))
        hum_label = QLabel("Humidity")
        hum_label.setStyleSheet("color: #aaa; font-size: 10px;")
        
        self.eco2_value = QLabel("---- ppm")
        self.eco2_value.setFont(QFont("Segoe UI", 16, QFont.Weight.Bold))
        eco2_label = QLabel("eCO2")
        eco2_label.setStyleSheet("color: #aaa; font-size: 10px;")
        
        self.etvoc_value = QLabel("---- ppb")
        self.etvoc_value.setFont(QFont("Segoe UI", 16, QFont.Weight.Bold))
        etvoc_label = QLabel("eTVOC")
        etvoc_label.setStyleSheet("color: #aaa; font-size: 10px;")
        
        grid.addWidget(self.temp_value, 0, 1)
        grid.addWidget(temp_label, 0, 1, Qt.AlignmentFlag.AlignBottom)
        grid.addWidget(self.hum_value, 0, 2)
        grid.addWidget(hum_label, 0, 2, Qt.AlignmentFlag.AlignBottom)
        grid.addWidget(self.eco2_value, 1, 1)
        grid.addWidget(eco2_label, 1, 1, Qt.AlignmentFlag.AlignBottom)
        grid.addWidget(self.etvoc_value, 1, 2)
        grid.addWidget(etvoc_label, 1, 2, Qt.AlignmentFlag.AlignBottom)
        
        layout.addLayout(grid)
        
        # ---- View switcher: Live | History | Serial ----
        view_tab_style = """
            QPushButton {
                background-color: #3a3a3a;
                color: #888;
                border: 1px solid #555;
                border-radius: 3px;
                padding: 4px 12px;
                font-size: 10px;
                font-weight: bold;
            }
            QPushButton:checked {
                background-color: #555;
                color: white;
                border-color: #777;
            }
        """
        
        view_tab_layout = QHBoxLayout()
        view_tab_layout.setSpacing(3)
        
        self._view_buttons = []
        for idx, label in enumerate(["Live", "History", "Serial"]):
            btn = QPushButton(label)
            btn.setCheckable(True)
            btn.setStyleSheet(view_tab_style)
            btn.clicked.connect(lambda checked, i=idx: self._switch_view(i))
            view_tab_layout.addWidget(btn)
            self._view_buttons.append(btn)
        
        self._view_buttons[0].setChecked(True)
        view_tab_layout.addStretch()
        layout.addLayout(view_tab_layout)
        
        # Stacked widget for Live / History / Serial views
        self._view_stack = QStackedWidget()
        
        # --- Page 0: Live chart ---
        live_page = QWidget()
        live_layout = QVBoxLayout(live_page)
        live_layout.setContentsMargins(0, 0, 0, 0)
        live_layout.setSpacing(4)
        
        # Live metric selector buttons
        live_metric_layout = QHBoxLayout()
        live_metric_layout.setSpacing(3)
        
        self._live_metric_buttons = {}
        metrics = [('aqi', 'AQI'), ('temp', 'Temp'), ('hum', 'Humidity'),
                   ('eco2', 'eCO2'), ('etvoc', 'eTVOC')]
        
        metric_tab_style = """
            QPushButton {
                background-color: #333;
                color: #888;
                border: 1px solid #444;
                border-radius: 3px;
                padding: 3px 8px;
                font-size: 9px;
            }
            QPushButton:checked {
                background-color: #4a4a4a;
                color: white;
                border-color: #666;
            }
        """
        
        for key, label in metrics:
            btn = QPushButton(label)
            btn.setCheckable(True)
            btn.setStyleSheet(metric_tab_style)
            btn.clicked.connect(lambda checked, k=key: self._select_live_metric(k))
            live_metric_layout.addWidget(btn)
            self._live_metric_buttons[key] = btn
        
        self._live_metric_buttons['aqi'].setChecked(True)
        live_metric_layout.addStretch()
        live_layout.addLayout(live_metric_layout)
        
        # Live time range buttons
        live_range_layout = QHBoxLayout()
        live_range_layout.setSpacing(3)
        
        range_btn_style = """
            QPushButton {
                background-color: #2a2a2a;
                color: #777;
                border: 1px solid #3a3a3a;
                border-radius: 3px;
                padding: 2px 6px;
                font-size: 8px;
            }
            QPushButton:checked {
                background-color: #3d5a3d;
                color: #8f8;
                border-color: #5a5a5a;
            }
        """
        
        self._live_range_buttons = {}
        for minutes, label in [(5, '5m'), (15, '15m'), (30, '30m'),
                                (60, '1h'), (None, 'All')]:
            btn = QPushButton(label)
            btn.setCheckable(True)
            btn.setStyleSheet(range_btn_style)
            btn.clicked.connect(lambda checked, m=minutes: self._select_live_range(m))
            live_range_layout.addWidget(btn)
            self._live_range_buttons[minutes] = btn
        
        self._live_range_buttons[None].setChecked(True)
        live_range_layout.addStretch()
        live_layout.addLayout(live_range_layout)
        
        self.live_chart = MiniChart(self)
        live_layout.addWidget(self.live_chart)
        
        self.live_status = QLabel("Live data: 0 samples")
        self.live_status.setStyleSheet("color: #666; font-size: 10px;")
        live_layout.addWidget(self.live_status, alignment=Qt.AlignmentFlag.AlignRight)
        
        self._view_stack.addWidget(live_page)
        
        # --- Page 1: Device history chart ---
        history_page = QWidget()
        history_layout = QVBoxLayout(history_page)
        history_layout.setContentsMargins(0, 0, 0, 0)
        history_layout.setSpacing(4)
        
        # Metric selector buttons
        metric_btn_layout = QHBoxLayout()
        metric_btn_layout.setSpacing(3)
        
        self._metric_buttons = {}
        metrics = [('aqi', 'AQI'), ('temp', 'Temp'), ('hum', 'Humidity'),
                   ('eco2', 'eCO2'), ('etvoc', 'eTVOC')]
        
        metric_tab_style = """
            QPushButton {
                background-color: #333;
                color: #888;
                border: 1px solid #444;
                border-radius: 3px;
                padding: 3px 8px;
                font-size: 9px;
            }
            QPushButton:checked {
                background-color: #4a4a4a;
                color: white;
                border-color: #666;
            }
        """
        
        for key, label in metrics:
            btn = QPushButton(label)
            btn.setCheckable(True)
            btn.setStyleSheet(metric_tab_style)
            btn.clicked.connect(lambda checked, k=key: self._select_history_metric(k))
            metric_btn_layout.addWidget(btn)
            self._metric_buttons[key] = btn
        
        self._metric_buttons['aqi'].setChecked(True)
        metric_btn_layout.addStretch()
        history_layout.addLayout(metric_btn_layout)
        
        # History time range buttons
        hist_range_layout = QHBoxLayout()
        hist_range_layout.setSpacing(3)
        
        range_btn_style_h = """
            QPushButton {
                background-color: #2a2a2a;
                color: #777;
                border: 1px solid #3a3a3a;
                border-radius: 3px;
                padding: 2px 6px;
                font-size: 8px;
            }
            QPushButton:checked {
                background-color: #3d5a3d;
                color: #8f8;
                border-color: #5a5a5a;
            }
        """
        
        self._hist_range_buttons = {}
        for minutes, label in [(60, '1h'), (360, '6h'), (1440, '24h'),
                                (4320, '3d'), (None, '7d')]:
            btn = QPushButton(label)
            btn.setCheckable(True)
            btn.setStyleSheet(range_btn_style_h)
            btn.clicked.connect(lambda checked, m=minutes: self._select_history_range(m))
            hist_range_layout.addWidget(btn)
            self._hist_range_buttons[minutes] = btn
        
        self._hist_range_buttons[None].setChecked(True)
        hist_range_layout.addStretch()
        history_layout.addLayout(hist_range_layout)
        
        self.history_chart = MiniChart(self)
        history_layout.addWidget(self.history_chart)
        
        # Progress bar (hidden by default)
        self.history_progress = QProgressBar()
        self.history_progress.setFixedHeight(10)
        self.history_progress.setTextVisible(False)
        self.history_progress.setStyleSheet("""
            QProgressBar {
                background-color: #2a2a2a;
                border: 1px solid #444;
                border-radius: 4px;
            }
            QProgressBar::chunk {
                background-color: #4CAF50;
                border-radius: 3px;
            }
        """)
        self.history_progress.setVisible(False)
        history_layout.addWidget(self.history_progress)
        
        # Status row: label + refresh button
        status_row = QHBoxLayout()
        status_row.setSpacing(6)
        
        self.history_status = QLabel("")
        self.history_status.setStyleSheet("color: #666; font-size: 10px;")
        status_row.addWidget(self.history_status)
        
        status_row.addStretch()
        
        self.refresh_btn = QPushButton("↻ Refresh")
        self.refresh_btn.setStyleSheet("""
            QPushButton {
                background-color: #333;
                color: #aaa;
                border: 1px solid #555;
                border-radius: 3px;
                padding: 2px 8px;
                font-size: 9px;
            }
            QPushButton:hover {
                background-color: #444;
                color: white;
            }
            QPushButton:disabled {
                background-color: #2a2a2a;
                color: #555;
                border-color: #3a3a3a;
            }
        """)
        self.refresh_btn.setFixedHeight(20)
        status_row.addWidget(self.refresh_btn)
        
        history_layout.addLayout(status_row)
        
        self._view_stack.addWidget(history_page)
        
        # --- Page 2: Serial console ---
        serial_page = QWidget()
        serial_layout = QVBoxLayout(serial_page)
        serial_layout.setContentsMargins(0, 0, 0, 0)
        serial_layout.setSpacing(4)
        
        self.serial_log = QTextEdit()
        self.serial_log.setReadOnly(True)
        self.serial_log.setFont(QFont("Consolas", 8))
        self.serial_log.setStyleSheet("""
            QTextEdit {
                background-color: #1e1e1e;
                color: #d4d4d4;
                border: 1px solid #444;
                border-radius: 4px;
                padding: 4px;
            }
        """)
        self.serial_log.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)
        serial_layout.addWidget(self.serial_log)
        
        # Clear button
        clear_btn = QPushButton("Clear")
        clear_btn.setStyleSheet(view_tab_style)
        clear_btn.setFixedWidth(60)
        clear_btn.clicked.connect(self.serial_log.clear)
        serial_layout.addWidget(clear_btn, alignment=Qt.AlignmentFlag.AlignRight)
        
        self._view_stack.addWidget(serial_page)
        
        layout.addWidget(self._view_stack)
    
    def update_data(self, data: dict, live_histories: dict, sample_count: int, use_fahrenheit: bool = True):
        self._use_fahrenheit = use_fahrenheit
        self._live_histories = live_histories
        self._live_sample_count = sample_count
        
        aqi = data.get("aqi")
        temp_c = data.get("temperature_c")
        hum = data.get("humidity")
        eco2 = data.get("eco2")
        etvoc = data.get("etvoc")
        
        aqi_uba = data.get("aqi_uba")
        if aqi is not None:
            self.aqi_value.setText(str(int(aqi)))
            color = get_aqi_color(aqi)
            self.aqi_value.setStyleSheet(f"color: {color.name()};")
            if aqi_uba is not None:
                self.aqi_status.setText(get_aqi_uba_label(int(aqi_uba)))
            else:
                self.aqi_status.setText("--")
            self.aqi_status.setStyleSheet(f"color: {color.name()};")
        
        if temp_c is not None:
            if use_fahrenheit:
                temp_f = temp_c * 9 / 5 + 32
                self.temp_value.setText(f"{temp_f:.1f}°F")
            else:
                self.temp_value.setText(f"{temp_c:.1f}°C")
        if hum is not None:
            self.hum_value.setText(f"{hum:.1f}%")
        if eco2 is not None:
            self.eco2_value.setText(f"{int(eco2)} ppm")
        if etvoc is not None:
            self.etvoc_value.setText(f"{int(etvoc)} ppb")
        
        # Always update the live chart with streaming data
        self._render_live_chart()
        self.live_status.setText(f"Live data: {sample_count} samples")
    
    def set_device_history(self, slots, window_us):
        """Set device history data and update the chart."""
        self._history_loading = False
        self._history_slots = slots
        self._window_us = window_us
        self._history_fetched_at = datetime.now()
        self.history_progress.setVisible(False)
        self.refresh_btn.setEnabled(True)
        n = len(slots)
        if n > 0:
            total_hours = n * window_us / 3_600_000_000
            if total_hours >= 24:
                span_text = f"{total_hours / 24:.1f} days"
            else:
                span_text = f"{total_hours:.1f} hours"
            fetched = self._history_fetched_at.strftime("%H:%M:%S")
            self.history_status.setText(f"Device history: {n} entries, {span_text} — refreshed {fetched}")
        else:
            self.history_status.setText("No device history available")
        self._render_history_chart()
    
    def set_history_loading(self, loading):
        """Show loading status while fetching device history."""
        self._history_loading = loading
        self.refresh_btn.setEnabled(not loading)
        if loading:
            self.history_status.setText("Loading device history...")
            self.history_progress.setValue(0)
            self.history_progress.setVisible(True)
            # Show loading state in the chart immediately
            self._render_history_chart()
        else:
            self.history_progress.setVisible(False)
    
    def set_history_progress(self, current, total):
        """Update the history fetch progress bar."""
        if total > 0:
            pct = int(current * 100 / total)
            self.history_progress.setValue(pct)
            self.history_status.setText(f"Loading device history... {current}/{total}")
    
    def _select_live_metric(self, metric):
        """Handle live metric tab button selection."""
        self._selected_live_metric = metric
        for key, btn in self._live_metric_buttons.items():
            btn.setChecked(key == metric)
        self._render_live_chart()
    
    def _select_live_range(self, minutes):
        """Handle live time range button selection."""
        self._live_range = minutes
        for key, btn in self._live_range_buttons.items():
            btn.setChecked(key == minutes)
        self._render_live_chart()
    
    def _select_history_range(self, minutes):
        """Handle history time range button selection."""
        self._history_range = minutes
        for key, btn in self._hist_range_buttons.items():
            btn.setChecked(key == minutes)
        self._render_history_chart()
    
    def _render_live_chart(self):
        """Render the live chart for the selected metric."""
        times = self._live_histories.get('times', [])
        metric = self._selected_live_metric
        cfg = METRIC_CONFIGS[metric]
        
        # Map metric key to the live history dict key
        values = self._live_histories.get(metric, [])
        
        # Apply time range filter
        if self._live_range is not None and times:
            cutoff = datetime.now() - timedelta(minutes=self._live_range)
            # Find the first index where time >= cutoff
            start_idx = 0
            for i, t in enumerate(times):
                if t >= cutoff:
                    start_idx = i
                    break
            else:
                start_idx = len(times)  # All data is older than cutoff
            times = times[start_idx:]
            values = values[start_idx:]
        
        if not times or not values:
            self.live_chart.ax.cla()
            self.live_chart.ax.set_facecolor('#2b2b2b')
            self.live_chart.ax.text(0.5, 0.5, 'No live data yet',
                                    transform=self.live_chart.ax.transAxes,
                                    ha='center', va='center', color='#888', fontsize=11)
            self.live_chart.ax.tick_params(colors='white', labelsize=7)
            self.live_chart.ax.grid(True, linestyle='--', alpha=0.3, color='#555')
            self.live_chart.fig.tight_layout(pad=1.5)
            self.live_chart.draw()
            return
        
        # Apply unit conversion for temperature
        unit = cfg['unit']
        if metric == 'temp' and self._use_fahrenheit:
            values = [v * 9 / 5 + 32 if not (isinstance(v, float) and v != v) else v for v in values]
            unit = '°F'
        
        self.live_chart.update_chart(times, values, cfg['label'], cfg['color'], unit)
    
    def _select_history_metric(self, metric):
        """Handle history metric tab button selection."""
        self._selected_history_metric = metric
        for key, btn in self._metric_buttons.items():
            btn.setChecked(key == metric)
        self._render_history_chart()
    
    def _render_history_chart(self):
        """Render the device history chart for the selected metric."""
        if not self._history_slots:
            # Show appropriate message in the chart area
            msg = 'Loading device history...' if self._history_loading else 'No history data'
            self.history_chart.ax.cla()
            self.history_chart.ax.set_facecolor('#2b2b2b')
            self.history_chart.ax.text(0.5, 0.5, msg, transform=self.history_chart.ax.transAxes,
                              ha='center', va='center', color='#888', fontsize=11)
            self.history_chart.ax.set_xlabel('', color='white', fontsize=9)
            self.history_chart.ax.set_ylabel('', color='white', fontsize=9)
            self.history_chart.ax.tick_params(colors='white', labelsize=7)
            self.history_chart.ax.grid(True, linestyle='--', alpha=0.3, color='#555')
            self.history_chart.fig.tight_layout(pad=1.5)
            self.history_chart.draw()
            return
        
        cfg = METRIC_CONFIGS[self._selected_history_metric]
        window_hours = self._window_us / 3_600_000_000
        
        # Apply time range filter — keep only the most recent N slots
        slots = self._history_slots
        if self._history_range is not None:
            range_hours = self._history_range / 60.0
            max_slots = int(range_hours / window_hours) if window_hours > 0 else len(slots)
            if max_slots < len(slots):
                slots = slots[-max_slots:]
        
        n = len(slots)
        
        # x = "hours ago": 0 = most recent, positive values = older
        x = [(n - 1 - i) * window_hours for i in range(n)]
        
        scale = cfg['scale']
        avg = [s.get(cfg['avg'], 0) * scale for s in slots]
        min_v = [s.get(cfg['min'], 0) * scale for s in slots]
        max_v = [s.get(cfg['max'], 0) * scale for s in slots]
        
        # Fahrenheit conversion for temperature
        unit = cfg['unit']
        if self._selected_history_metric == 'temp' and self._use_fahrenheit:
            avg = [v * 9 / 5 + 32 for v in avg]
            min_v = [v * 9 / 5 + 32 for v in min_v]
            max_v = [v * 9 / 5 + 32 for v in max_v]
            unit = '°F'
        
        self.history_chart.update_history(x, avg, min_v, max_v,
                                          cfg['label'], cfg['color'], unit)
    
    def _switch_view(self, index):
        """Switch between Live (0), History (1) and Serial (2) views."""
        self._view_stack.setCurrentIndex(index)
        for i, btn in enumerate(self._view_buttons):
            btn.setChecked(i == index)
    
    def append_serial_line(self, line: str):
        """Append a line to the serial console log."""
        # Color-code TX vs RX
        if line.startswith("TX:"):
            colored = f'<span style="color:#569cd6">{line}</span>'
        elif line.startswith("TX ERROR:"):
            colored = f'<span style="color:#f44747">{line}</span>'
        elif "history" in line.lower():
            colored = f'<span style="color:#4ec9b0">{line}</span>'
        else:
            colored = f'<span style="color:#d4d4d4">{line}</span>'
        
        self.serial_log.append(colored)
        # Auto-scroll and limit lines
        if self.serial_log.document().blockCount() > 500:
            cursor = self.serial_log.textCursor()
            cursor.movePosition(cursor.MoveOperation.Start)
            cursor.movePosition(cursor.MoveOperation.Down, cursor.MoveMode.KeepAnchor, 50)
            cursor.removeSelectedText()
    
    def show_at_cursor(self):
        # Position near cursor but ensure fully on screen
        cursor_pos = QCursor.pos()
        
        # Use the screen the cursor is actually on (multi-monitor safe)
        screen_at = QApplication.screenAt(cursor_pos)
        if screen_at is None:
            screen_at = QApplication.primaryScreen()
        avail = screen_at.availableGeometry()  # excludes taskbar
        
        w = self.sizeHint().width()
        h = self.sizeHint().height()
        
        x = cursor_pos.x() - w // 2
        y = cursor_pos.y() - h - 20
        
        # Clamp horizontally
        if x < avail.left():
            x = avail.left() + 10
        if x + w > avail.right():
            x = avail.right() - w - 10
        
        # Clamp vertically — if it would go above the screen, show below cursor
        if y < avail.top():
            y = cursor_pos.y() + 30
        # If it would go below the available area (past taskbar), push it up
        if y + h > avail.bottom():
            y = avail.bottom() - h - 10
        
        self.move(x, y)
        self.show()


class SettingsDialog(QDialog):
    """Settings dialog for the tray app."""
    def __init__(self, parent=None, current_port: str = "", alert_threshold: int = 100, 
                 alert_enabled: bool = True, autostart: bool = False, history_size: int = 1000,
                 use_fahrenheit: bool = True):
        super().__init__(parent)
        self.setWindowTitle("AirCube Tray Settings")
        self.setMinimumWidth(400)
        self.current_port = current_port
        self.setup_ui(current_port, alert_threshold, alert_enabled, autostart, history_size, use_fahrenheit)
    
    def setup_ui(self, current_port: str, alert_threshold: int, alert_enabled: bool, autostart: bool, history_size: int, use_fahrenheit: bool):
        layout = QVBoxLayout(self)
        
        # Connection group
        conn_group = QGroupBox("Connection")
        conn_layout = QHBoxLayout(conn_group)
        
        conn_layout.addWidget(QLabel("Serial Port:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(200)
        self.refresh_ports(current_port)
        conn_layout.addWidget(self.port_combo, stretch=1)
        
        refresh_btn = QPushButton("Refresh")
        refresh_btn.clicked.connect(lambda: self.refresh_ports(self.port_combo.currentData()))
        conn_layout.addWidget(refresh_btn)
        
        layout.addWidget(conn_group)
        
        # Display group
        display_group = QGroupBox("Display")
        display_layout = QHBoxLayout(display_group)
        display_layout.addWidget(QLabel("Temperature Unit:"))
        self.temp_unit_combo = QComboBox()
        self.temp_unit_combo.addItem("Fahrenheit (°F)", "F")
        self.temp_unit_combo.addItem("Celsius (°C)", "C")
        self.temp_unit_combo.setCurrentIndex(0 if use_fahrenheit else 1)
        display_layout.addWidget(self.temp_unit_combo)
        display_layout.addStretch()
        layout.addWidget(display_group)
        
        # History group
        history_group = QGroupBox("History")
        history_layout = QHBoxLayout(history_group)
        history_layout.addWidget(QLabel("Max samples to store:"))
        self.history_spin = QSpinBox()
        self.history_spin.setRange(100, 100000)
        self.history_spin.setSingleStep(100)
        self.history_spin.setValue(history_size)
        history_layout.addWidget(self.history_spin)
        history_layout.addStretch()
        layout.addWidget(history_group)
        
        # Alerts group
        alert_group = QGroupBox("Alerts")
        alert_layout = QVBoxLayout(alert_group)
        
        self.alert_checkbox = QCheckBox("Show notification when AQI exceeds threshold")
        self.alert_checkbox.setChecked(alert_enabled)
        alert_layout.addWidget(self.alert_checkbox)
        
        threshold_layout = QHBoxLayout()
        threshold_layout.addWidget(QLabel("Threshold:"))
        self.threshold_spin = QSpinBox()
        self.threshold_spin.setRange(10, 500)
        self.threshold_spin.setValue(alert_threshold)
        threshold_layout.addWidget(self.threshold_spin)
        threshold_layout.addStretch()
        alert_layout.addLayout(threshold_layout)
        
        layout.addWidget(alert_group)
        
        # Startup group
        startup_group = QGroupBox("Startup")
        startup_layout = QVBoxLayout(startup_group)
        self.autostart_checkbox = QCheckBox("Start with Windows")
        self.autostart_checkbox.setChecked(autostart)
        startup_layout.addWidget(self.autostart_checkbox)
        layout.addWidget(startup_group)
        
        # Buttons
        btn_layout = QHBoxLayout()
        btn_layout.addStretch()
        
        ok_btn = QPushButton("OK")
        ok_btn.setDefault(True)
        ok_btn.clicked.connect(self.accept)
        btn_layout.addWidget(ok_btn)
        
        cancel_btn = QPushButton("Cancel")
        cancel_btn.clicked.connect(self.reject)
        btn_layout.addWidget(cancel_btn)
        
        layout.addLayout(btn_layout)
    
    def refresh_ports(self, select_port: str = ""):
        self.port_combo.clear()
        ports = list_ports.comports()
        selected_idx = 0
        
        for i, p in enumerate(ports):
            self.port_combo.addItem(f"{p.device} - {p.description}", p.device)
            if p.device == select_port:
                selected_idx = i
        
        if not ports:
            self.port_combo.addItem("No ports found", None)
        else:
            self.port_combo.setCurrentIndex(selected_idx)
    
    def get_settings(self) -> dict:
        return {
            "port": self.port_combo.currentData(),
            "alert_enabled": self.alert_checkbox.isChecked(),
            "alert_threshold": self.threshold_spin.value(),
            "autostart": self.autostart_checkbox.isChecked(),
            "history_size": self.history_spin.value(),
            "use_fahrenheit": self.temp_unit_combo.currentData() == "F",
        }


class AirCubeTray(QSystemTrayIcon):
    """System tray icon for AirCube monitoring."""
    
    def __init__(self):
        super().__init__()
        
        # Settings
        self.settings = QSettings("StuckAtPrototype", "AirCubeTray")
        self.port = self.settings.value("port", "")
        self.alert_threshold = int(self.settings.value("alert_threshold", 100))
        self.alert_enabled = self.settings.value("alert_enabled", "true") == "true"
        self.autostart = self.settings.value("autostart", "false") == "true"
        self.history_size = int(self.settings.value("history_size", MAX_HISTORY))
        self.use_fahrenheit = self.settings.value("use_fahrenheit", "true") == "true"
        
        # State
        self.serial_thread: Optional[SerialReaderThread] = None
        self.is_connected = False
        self.last_data: Optional[dict] = None
        self.alert_shown = False
        self.sample_count = 0
        
        # History for live chart (stores datetime objects and sensor values)
        self.time_history = collections.deque(maxlen=self.history_size)
        self.aqi_history = collections.deque(maxlen=self.history_size)
        self.temp_history = collections.deque(maxlen=self.history_size)
        self.hum_history = collections.deque(maxlen=self.history_size)
        self.eco2_history = collections.deque(maxlen=self.history_size)
        self.etvoc_history = collections.deque(maxlen=self.history_size)
        
        # Device history cache (fetched from device flash)
        self._device_history = []
        self._device_history_window_us = 300_000_000
        self._device_history_time = 0
        self._history_fetch_in_progress = False
        self._history_fetch_purpose = None
        self._history_fetch_slots = []
        self._history_fetch_total = 0
        self._history_fetch_start = 0
        self._history_fetch_window_us = 300_000_000
        self._csv_export_path = ''
        
        # Load persisted history from disk
        self.load_history()
        
        # Popup window
        self.popup = PopupWindow()
        self.popup.refresh_btn.clicked.connect(self._on_refresh_history_clicked)
        
        self.setup_ui()
        self.update_icon()
        
        # Auto-connect on startup if port is saved
        if self.port:
            QTimer.singleShot(1000, self.connect)
    
    def setup_ui(self):
        # Create context menu
        self.menu = QMenu()
        
        self.status_action = QAction("Disconnected")
        self.status_action.setEnabled(False)
        self.menu.addAction(self.status_action)
        
        self.menu.addSeparator()
        
        self.connect_action = QAction("Connect")
        self.connect_action.triggered.connect(self.toggle_connection)
        self.menu.addAction(self.connect_action)
        
        # Keep as instance variable to prevent garbage collection
        self.settings_action = QAction("Settings...")
        self.settings_action.triggered.connect(self.show_settings)
        self.menu.addAction(self.settings_action)
        
        self.clear_history_action = QAction("Clear History")
        self.clear_history_action.triggered.connect(self.clear_history)
        self.menu.addAction(self.clear_history_action)
        
        self.download_history_action = QAction("Download Device History...")
        self.download_history_action.triggered.connect(self.fetch_device_history)
        self.menu.addAction(self.download_history_action)
        
        self.menu.addSeparator()
        
        # Keep as instance variable to prevent garbage collection
        self.quit_action = QAction("Quit")
        self.quit_action.triggered.connect(self.quit_app)
        self.menu.addAction(self.quit_action)
        
        self.setContextMenu(self.menu)
        
        # Click handling
        self.activated.connect(self.on_activated)
        
        # Notification click handling
        self.messageClicked.connect(self.on_notification_clicked)
    
    def on_activated(self, reason):
        if reason == QSystemTrayIcon.ActivationReason.Trigger:
            # Single click - show popup
            self.show_popup()
        elif reason == QSystemTrayIcon.ActivationReason.DoubleClick:
            # Double click - toggle connection
            self.toggle_connection()
        elif reason == QSystemTrayIcon.ActivationReason.MiddleClick:
            # Middle click - open settings
            self.show_settings()
    
    def on_notification_clicked(self):
        """Handle click on notification balloon."""
        # Only show popup if we have data to show
        if self.last_data and self.is_connected:
            self.show_popup()
        # Otherwise just ignore the click (don't open anything)
    
    def _build_live_histories(self):
        """Build a dict of live history deques for the popup."""
        return {
            'times': list(self.time_history),
            'aqi': list(self.aqi_history),
            'temp': list(self.temp_history),
            'hum': list(self.hum_history),
            'eco2': list(self.eco2_history),
            'etvoc': list(self.etvoc_history),
        }
    
    def clear_history(self):
        """Clear the stored history data."""
        self.time_history.clear()
        self.aqi_history.clear()
        self.temp_history.clear()
        self.hum_history.clear()
        self.eco2_history.clear()
        self.etvoc_history.clear()
        self.sample_count = 0
        # Update popup if visible
        if self.popup.isVisible():
            self.popup.update_data(
                self.last_data or {},
                self._build_live_histories(),
                self.sample_count,
                self.use_fahrenheit
            )
    
    # ---- Device History Fetch (unified mechanism) ----
    
    def fetch_device_history(self):
        """Download sensor history from device flash to CSV file."""
        if not self.is_connected or not self.serial_thread:
            self.showMessage("Device History", "Not connected to device.",
                             QSystemTrayIcon.MessageIcon.Warning, 3000)
            return
        
        # Ask user where to save the CSV
        default_name = f"aircube_history_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        docs = os.path.expanduser("~/Documents")
        path, _ = QFileDialog.getSaveFileName(
            None, "Save Device History", os.path.join(docs, default_name),
            "CSV Files (*.csv);;All Files (*)")
        if not path:
            return
        
        self._csv_export_path = path
        self._start_history_fetch('csv')
    
    def _start_history_fetch(self, purpose='display'):
        """Begin paginated fetch of all device history entries."""
        if self._history_fetch_in_progress:
            return
        if not self.is_connected or not self.serial_thread:
            return
        
        self._history_fetch_in_progress = True
        self._history_fetch_purpose = purpose
        self._history_fetch_slots = []
        self._history_fetch_total = 0
        self._history_fetch_start = 0
        self._history_fetch_window_us = 300_000_000
        
        self.serial_thread.command_response.connect(self._on_history_fetch_response)
        
        # Timeout timer - cancel fetch if device doesn't respond
        self._history_fetch_timer = QTimer()
        self._history_fetch_timer.setSingleShot(True)
        self._history_fetch_timer.timeout.connect(self._on_history_fetch_timeout)
        self._history_fetch_timer.start(8000)  # 8 second timeout
        
        if purpose == 'csv':
            self.showMessage("Device History", "Fetching history info...",
                             QSystemTrayIcon.MessageIcon.Information, 2000)
        
        self.serial_thread.send_command('{"cmd":"get_history_info"}')
    
    def _on_history_fetch_response(self, data: dict):
        """Handle paginated history fetch responses."""
        # Reset timeout on each successful response
        if hasattr(self, '_history_fetch_timer'):
            self._history_fetch_timer.start(8000)
        
        if "history_info" in data:
            info = data["history_info"]
            self._history_fetch_total = info.get("entries", 0)
            self._history_fetch_window_us = info.get("window_us", 300_000_000)
            
            if self._history_fetch_total == 0:
                self._complete_history_fetch()
                return
            
            if self._history_fetch_purpose == 'csv':
                self.showMessage("Device History",
                                 f"Downloading {self._history_fetch_total} entries...",
                                 QSystemTrayIcon.MessageIcon.Information, 2000)
            
            self._history_fetch_start = 0
            self._send_next_history_page()
        
        elif "history" in data:
            slots = [s for s in data.get("history", []) if s is not None]
            self._history_fetch_slots.extend(slots)
            self._history_fetch_start += data.get("count", len(slots))
            
            # Update progress bar
            self.popup.set_history_progress(
                self._history_fetch_start, self._history_fetch_total)
            
            if self._history_fetch_start >= self._history_fetch_total:
                self._complete_history_fetch()
            else:
                self._send_next_history_page()
    
    def _send_next_history_page(self):
        """Send the next get_history page request."""
        remaining = self._history_fetch_total - self._history_fetch_start
        count = min(48, remaining)
        cmd = json.dumps({
            "cmd": "get_history",
            "start": self._history_fetch_start,
            "count": count
        }, separators=(',', ':'))
        self.serial_thread.send_command(cmd)
    
    def _on_history_fetch_timeout(self):
        """Handle fetch timeout - device didn't respond in time."""
        if not self._history_fetch_in_progress:
            return
        
        try:
            self.serial_thread.command_response.disconnect(self._on_history_fetch_response)
        except (TypeError, RuntimeError):
            pass
        
        self._history_fetch_in_progress = False
        self.popup.set_device_history([], 300_000_000)
        
        if self._history_fetch_purpose == 'csv':
            self.showMessage("Device History", "Timeout: device did not respond.",
                             QSystemTrayIcon.MessageIcon.Warning, 3000)
    
    def _complete_history_fetch(self):
        """Handle fetch completion - route to CSV export or display update."""
        # Cancel timeout timer
        if hasattr(self, '_history_fetch_timer'):
            self._history_fetch_timer.stop()
        
        try:
            self.serial_thread.command_response.disconnect(self._on_history_fetch_response)
        except (TypeError, RuntimeError):
            pass
        
        purpose = self._history_fetch_purpose
        slots = self._history_fetch_slots
        window_us = self._history_fetch_window_us
        self._history_fetch_in_progress = False
        
        if purpose == 'csv':
            self._write_history_csv(slots)
        elif purpose == 'display':
            self._device_history = slots
            self._device_history_window_us = window_us
            self._device_history_time = time.time()
            # Always update popup (even if it was closed and reopened)
            self.popup.set_device_history(slots, window_us)
    
    def _write_history_csv(self, slots):
        """Write fetched history slots to CSV file."""
        if not slots:
            self.showMessage("Device History", "No history data on device.",
                             QSystemTrayIcon.MessageIcon.Information, 3000)
            return
        try:
            with open(self._csv_export_path, 'w', newline='') as f:
                writer = csv.writer(f)
                writer.writerow([
                    "slot", "sequence",
                    "temp_avg_c", "temp_min_c", "temp_max_c",
                    "humidity_avg", "humidity_min", "humidity_max",
                    "aqi_avg", "aqi_min", "aqi_max",
                    "eco2_avg", "eco2_min", "eco2_max",
                    "etvoc_avg", "etvoc_min", "etvoc_max"
                ])
                for i, s in enumerate(slots):
                    writer.writerow([
                        i, s.get("seq", ""),
                        s.get("t_a", 0) / 100.0, s.get("t_n", 0) / 100.0, s.get("t_x", 0) / 100.0,
                        s.get("h_a", 0) / 100.0, s.get("h_n", 0) / 100.0, s.get("h_x", 0) / 100.0,
                        s.get("q_a", ""), s.get("q_n", ""), s.get("q_x", ""),
                        s.get("c_a", ""), s.get("c_n", ""), s.get("c_x", ""),
                        s.get("v_a", ""), s.get("v_n", ""), s.get("v_x", ""),
                    ])
            self.showMessage("Device History",
                             f"Saved {len(slots)} entries to:\n{self._csv_export_path}",
                             QSystemTrayIcon.MessageIcon.Information, 5000)
        except Exception as e:
            self.showMessage("Device History", f"Failed to write CSV: {e}",
                             QSystemTrayIcon.MessageIcon.Warning, 5000)
    
    def show_popup(self):
        """Show the popup window with current data and device history chart."""
        if self.last_data:
            self.popup.update_data(
                self.last_data,
                self._build_live_histories(),
                self.sample_count,
                self.use_fahrenheit
            )
        
        # Show cached device history if available
        if self._device_history:
            self.popup.set_device_history(
                self._device_history, self._device_history_window_us)
        
        # Fetch fresh history if cache is stale or empty
        if (self.is_connected and self.serial_thread
                and not self._history_fetch_in_progress):
            cache_age = time.time() - self._device_history_time
            if not self._device_history or cache_age > 120:
                self.popup.set_history_loading(True)
                self._start_history_fetch('display')
        
        self.popup.show_at_cursor()
    
    def _on_refresh_history_clicked(self):
        """Handle manual history refresh button click."""
        if (self.is_connected and self.serial_thread
                and not self._history_fetch_in_progress):
            self.popup.set_history_loading(True)
            self._start_history_fetch('display')
    
    def toggle_connection(self):
        if self.is_connected:
            self.disconnect()
        else:
            self.connect()
    
    def connect(self):
        if not self.port:
            self.show_settings()
            return
        
        # Don't clear history - keep it across reconnects
        # sample_count continues from where it was
        
        self.serial_thread = SerialReaderThread(self.port)
        self.serial_thread.data_received.connect(self.on_data_received)
        self.serial_thread.error_occurred.connect(self.on_error)
        self.serial_thread.connected.connect(self.on_connected)
        self.serial_thread.raw_line.connect(self.popup.append_serial_line)
        self.serial_thread.start()
    
    def disconnect(self):
        if self.serial_thread:
            self.serial_thread.stop()
            self.serial_thread = None
        
        # Clear device history cache
        self._device_history = []
        self._device_history_time = 0
        self._history_fetch_in_progress = False
        self.popup.set_device_history([], 300_000_000)
        
        # Insert a gap marker (NaN) so the chart shows a break
        if self.time_history:
            self.time_history.append(datetime.now())
            self.aqi_history.append(float('nan'))
            self.temp_history.append(float('nan'))
            self.hum_history.append(float('nan'))
            self.eco2_history.append(float('nan'))
            self.etvoc_history.append(float('nan'))
        
        self.is_connected = False
        self.last_data = None
        self.alert_shown = False
        self.connect_action.setText("Connect")
        self.status_action.setText("Disconnected")
        self.update_icon()
        self.setToolTip("AirCube Tray - Disconnected\nClick to view details")
    
    def on_connected(self):
        self.is_connected = True
        self.connect_action.setText("Disconnect")
        self.status_action.setText(f"Connected to {self.port}")
        self.update_icon()
        self.setToolTip("AirCube Tray - Waiting for data...\nClick to view details")
    
    def on_data_received(self, data: dict):
        self.last_data = data
        self.sample_count += 1
        
        # Update history with actual timestamp for all metrics
        aqi = data.get("aqi")
        
        if aqi is not None:
            try:
                now = datetime.now()
                self.time_history.append(now)
                self.aqi_history.append(float(aqi))
                # Store other metrics (use NaN if not present so deques stay aligned)
                temp_c = data.get("temperature_c")
                self.temp_history.append(float(temp_c) if temp_c is not None else float('nan'))
                hum = data.get("humidity")
                self.hum_history.append(float(hum) if hum is not None else float('nan'))
                eco2 = data.get("eco2")
                self.eco2_history.append(float(eco2) if eco2 is not None else float('nan'))
                etvoc = data.get("etvoc")
                self.etvoc_history.append(float(etvoc) if etvoc is not None else float('nan'))
            except (TypeError, ValueError):
                pass
        
        self.update_icon()
        self.update_tooltip()
        self.check_alert()
        
        # Update popup if visible
        if self.popup.isVisible():
            self.popup.update_data(
                data,
                self._build_live_histories(),
                self.sample_count,
                self.use_fahrenheit
            )
    
    def on_error(self, error: str):
        self.showMessage(
            "AirCube Connection Error",
            f"Lost connection: {error}",
            QSystemTrayIcon.MessageIcon.Warning,
            3000
        )
        self.disconnect()
    
    def update_icon(self):
        aqi = self.last_data.get("aqi") if self.last_data else None
        icon = create_aqi_icon(aqi, self.is_connected)
        self.setIcon(icon)
    
    def update_tooltip(self):
        if not self.last_data:
            return
        
        aqi = self.last_data.get("aqi")
        aqi_uba = self.last_data.get("aqi_uba")
        if aqi is not None:
            status = get_aqi_uba_label(int(aqi_uba)) if aqi_uba is not None else "--"
            self.setToolTip(f"AirCube - AQI: {int(aqi)} ({status})\nClick to view details")
        else:
            self.setToolTip("AirCube Tray\nClick to view details")
    
    def check_alert(self):
        if not self.alert_enabled or not self.last_data:
            return
        
        aqi = self.last_data.get("aqi")
        if aqi is None:
            return
        
        if aqi >= self.alert_threshold and not self.alert_shown:
            self.showMessage(
                "Air Quality Alert",
                f"AQI has reached {int(aqi)} - {get_aqi_uba_label(int(self.last_data.get('aqi_uba', 0)))}",
                QSystemTrayIcon.MessageIcon.Warning,
                5000
            )
            self.alert_shown = True
        elif aqi < self.alert_threshold - 10:
            self.alert_shown = False
    
    def show_settings(self):
        dialog = SettingsDialog(
            current_port=self.port,
            alert_threshold=self.alert_threshold,
            alert_enabled=self.alert_enabled,
            autostart=self.autostart,
            history_size=self.history_size,
            use_fahrenheit=self.use_fahrenheit
        )
        
        if dialog.exec() == QDialog.DialogCode.Accepted:
            settings = dialog.get_settings()
            
            # Save settings
            new_port = settings["port"] or ""
            self.alert_enabled = settings["alert_enabled"]
            self.alert_threshold = settings["alert_threshold"]
            self.autostart = settings["autostart"]
            new_history_size = settings["history_size"]
            self.use_fahrenheit = settings["use_fahrenheit"]
            
            self.settings.setValue("port", new_port)
            self.settings.setValue("alert_enabled", "true" if self.alert_enabled else "false")
            self.settings.setValue("alert_threshold", self.alert_threshold)
            self.settings.setValue("autostart", "true" if self.autostart else "false")
            self.settings.setValue("history_size", new_history_size)
            self.settings.setValue("use_fahrenheit", "true" if self.use_fahrenheit else "false")
            
            # Handle autostart
            if self.autostart:
                self.enable_autostart()
            else:
                self.disable_autostart()
            
            # Handle history size change - recreate deques with new maxlen
            if new_history_size != self.history_size:
                self.history_size = new_history_size
                # Create new deques, preserving existing data (up to new limit)
                self.time_history = collections.deque(list(self.time_history), maxlen=self.history_size)
                self.aqi_history = collections.deque(list(self.aqi_history), maxlen=self.history_size)
                self.temp_history = collections.deque(list(self.temp_history), maxlen=self.history_size)
                self.hum_history = collections.deque(list(self.hum_history), maxlen=self.history_size)
                self.eco2_history = collections.deque(list(self.eco2_history), maxlen=self.history_size)
                self.etvoc_history = collections.deque(list(self.etvoc_history), maxlen=self.history_size)
            
            # Only reconnect if port changed
            if new_port != self.port:
                if self.is_connected:
                    self.disconnect()
                self.port = new_port
                if self.port:
                    self.connect()
            else:
                self.port = new_port
    
    def enable_autostart(self):
        """Add to Windows startup."""
        if sys.platform != 'win32':
            return
        import winreg
        try:
            key = winreg.OpenKey(
                winreg.HKEY_CURRENT_USER,
                r"Software\Microsoft\Windows\CurrentVersion\Run",
                0, winreg.KEY_SET_VALUE
            )
            winreg.SetValueEx(key, "AirCubeTray", 0, winreg.REG_SZ, sys.executable)
            winreg.CloseKey(key)
        except Exception:
            pass
    
    def disable_autostart(self):
        """Remove from Windows startup."""
        if sys.platform != 'win32':
            return
        import winreg
        try:
            key = winreg.OpenKey(
                winreg.HKEY_CURRENT_USER,
                r"Software\Microsoft\Windows\CurrentVersion\Run",
                0, winreg.KEY_SET_VALUE
            )
            winreg.DeleteValue(key, "AirCubeTray")
            winreg.CloseKey(key)
        except Exception:
            pass
    
    def get_history_file_path(self) -> str:
        """Get the path to the history data file."""
        app_data = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.AppDataLocation)
        if not os.path.exists(app_data):
            os.makedirs(app_data)
        return os.path.join(app_data, "history.json")
    
    def save_history(self):
        """Save history data to disk."""
        try:
            import math
            def sanitize(values):
                return [None if (isinstance(v, float) and math.isnan(v)) else v for v in values]
            
            data = {
                "times": [t.isoformat() for t in self.time_history],
                "aqis": sanitize(self.aqi_history),
                "temps": sanitize(self.temp_history),
                "hums": sanitize(self.hum_history),
                "eco2s": sanitize(self.eco2_history),
                "etvocs": sanitize(self.etvoc_history),
                "sample_count": self.sample_count
            }
            with open(self.get_history_file_path(), 'w') as f:
                json.dump(data, f)
        except Exception as e:
            print(f"Failed to save history: {e}")
    
    def load_history(self):
        """Load history data from disk."""
        try:
            path = self.get_history_file_path()
            if os.path.exists(path):
                with open(path, 'r') as f:
                    data = json.load(f)
                
                def restore(values):
                    return [float('nan') if v is None else v for v in values]
                
                # Parse timestamps and rebuild deques
                times = [datetime.fromisoformat(t) for t in data.get("times", [])]
                self.sample_count = data.get("sample_count", 0)
                
                # Extend deques (respects maxlen)
                self.time_history.extend(times)
                self.aqi_history.extend(restore(data.get("aqis", [])))
                self.temp_history.extend(restore(data.get("temps", [])))
                self.hum_history.extend(restore(data.get("hums", [])))
                self.eco2_history.extend(restore(data.get("eco2s", [])))
                self.etvoc_history.extend(restore(data.get("etvocs", [])))
                
                # Backfill missing metric deques with NaN if history file
                # was saved before multi-metric support was added
                n = len(self.time_history)
                for deq in (self.temp_history, self.hum_history,
                            self.eco2_history, self.etvoc_history):
                    while len(deq) < n:
                        deq.appendleft(float('nan'))
        except Exception as e:
            print(f"Failed to load history: {e}")
    
    def quit_app(self):
        self.popup.hide()
        self.save_history()
        self.disconnect()
        QApplication.quit()


def main():
    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)
    app.setStyle("Fusion")
    
    # Check if system tray is available
    if not QSystemTrayIcon.isSystemTrayAvailable():
        QMessageBox.critical(
            None, "Error",
            "System tray is not available on this system."
        )
        sys.exit(1)
    
    tray = AirCubeTray()
    tray.show()
    
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
