"""
AirCube Tray Monitor
A lightweight system tray app that displays AQI in the Windows taskbar.
"""

__version__ = "1.0.2"
__app_name__ = "AirCube Tray"

import collections
import json
import os
import re
import sys
from datetime import datetime
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
    QSpinBox, QGroupBox, QMessageBox, QWidget, QFrame, QGridLayout
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
            "eco2": data["ens16x"].get("eco2"),
            "etvoc": data["ens16x"].get("etvoc"),
        }
    except (KeyError, TypeError, json.JSONDecodeError):
        return None


def get_aqi_color(aqi: float) -> QColor:
    """Get color based on AQI value."""
    if aqi <= 50:
        return QColor("#4CAF50")  # Green - Good
    elif aqi <= 100:
        return QColor("#FFC107")  # Yellow - Moderate
    elif aqi <= 150:
        return QColor("#FF9800")  # Orange - Unhealthy for sensitive
    else:
        return QColor("#F44336")  # Red - Unhealthy


def get_aqi_label(aqi: float) -> str:
    """Get text label for AQI level."""
    if aqi <= 50:
        return "Good"
    elif aqi <= 100:
        return "Moderate"
    elif aqi <= 150:
        return "Bad"
    elif aqi <= 200:
        return "Unhealthy"
    else:
        return "Very Unhealthy"


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
    error_occurred = pyqtSignal(str)
    connected = pyqtSignal()
    
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
                        parsed = parse_json_line(decoded)
                        if parsed:
                            self.data_received.emit(parsed)
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


class MiniChart(FigureCanvas):
    """Small matplotlib chart for the popup."""
    def __init__(self, parent=None, width=4, height=2.5):
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
    
    def update_chart(self, time_data, aqi_data):
        self.ax.cla()
        self.ax.set_facecolor('#2b2b2b')
        
        if time_data and aqi_data:
            self.ax.plot(time_data, aqi_data, color='#4CAF50', linewidth=1.5)
            self.ax.fill_between(time_data, aqi_data, alpha=0.3, color='#4CAF50')
            
            # Format x-axis as time
            self.ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S'))
            self.ax.xaxis.set_major_locator(mdates.AutoDateLocator())
            self.fig.autofmt_xdate(rotation=45, ha='right')
        
        self.ax.set_ylabel('AQI', color='white', fontsize=9)
        self.ax.set_xlabel('Time', color='white', fontsize=9)
        self.ax.tick_params(colors='white', labelsize=7)
        self.ax.grid(True, linestyle='--', alpha=0.3, color='#555')
        self.fig.tight_layout(pad=1.5)
        self.draw()


class PopupWindow(QWidget):
    """Popup window showing all sensor data and a chart."""
    def __init__(self, parent=None):
        super().__init__(parent, Qt.WindowType.Popup | Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setMinimumSize(420, 350)
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
        
        # Chart
        self.chart = MiniChart(self)
        layout.addWidget(self.chart)
        
        # Sample count
        self.sample_label = QLabel("Samples: 0")
        self.sample_label.setStyleSheet("color: #666; font-size: 10px;")
        layout.addWidget(self.sample_label, alignment=Qt.AlignmentFlag.AlignRight)
    
    def update_data(self, data: dict, time_history: list, aqi_history: list, sample_count: int, use_fahrenheit: bool = True):
        aqi = data.get("aqi")
        temp_c = data.get("temperature_c")
        hum = data.get("humidity")
        eco2 = data.get("eco2")
        etvoc = data.get("etvoc")
        
        if aqi is not None:
            self.aqi_value.setText(str(int(aqi)))
            color = get_aqi_color(aqi)
            self.aqi_value.setStyleSheet(f"color: {color.name()};")
            self.aqi_status.setText(get_aqi_label(aqi))
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
        
        self.sample_label.setText(f"Samples: {sample_count}")
        self.chart.update_chart(time_history, aqi_history)
    
    def show_at_cursor(self):
        # Position near cursor but ensure on screen
        cursor_pos = QCursor.pos()
        screen = QApplication.primaryScreen().geometry()
        
        x = cursor_pos.x() - self.width() // 2
        y = cursor_pos.y() - self.height() - 20
        
        # Keep on screen
        if x < screen.left():
            x = screen.left() + 10
        if x + self.width() > screen.right():
            x = screen.right() - self.width() - 10
        if y < screen.top():
            y = cursor_pos.y() + 30
        
        self.move(x, y)
        self.show()
        # Popup will auto-close when user clicks outside (Qt.WindowType.Popup behavior)


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
        
        # History for chart (stores datetime objects and AQI values)
        self.time_history = collections.deque(maxlen=self.history_size)
        self.aqi_history = collections.deque(maxlen=self.history_size)
        
        # Load persisted history from disk
        self.load_history()
        
        # Popup window
        self.popup = PopupWindow()
        
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
    
    def clear_history(self):
        """Clear the stored history data."""
        self.time_history.clear()
        self.aqi_history.clear()
        self.sample_count = 0
        # Update popup if visible
        if self.popup.isVisible():
            self.popup.update_data(
                self.last_data or {},
                list(self.time_history),
                list(self.aqi_history),
                self.sample_count,
                self.use_fahrenheit
            )
    
    def show_popup(self):
        """Show the popup window with current data."""
        if self.last_data:
            self.popup.update_data(
                self.last_data, 
                list(self.time_history), 
                list(self.aqi_history),
                self.sample_count,
                self.use_fahrenheit
            )
        self.popup.show_at_cursor()
    
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
        self.serial_thread.start()
    
    def disconnect(self):
        if self.serial_thread:
            self.serial_thread.stop()
            self.serial_thread = None
        
        # Insert a gap marker (NaN) so the chart shows a break
        if self.time_history:
            self.time_history.append(datetime.now())
            self.aqi_history.append(float('nan'))
        
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
        
        # Update history with actual timestamp
        aqi = data.get("aqi")
        
        if aqi is not None:
            try:
                self.time_history.append(datetime.now())
                self.aqi_history.append(float(aqi))
            except (TypeError, ValueError):
                pass
        
        self.update_icon()
        self.update_tooltip()
        self.check_alert()
        
        # Update popup if visible
        if self.popup.isVisible():
            self.popup.update_data(
                data,
                list(self.time_history),
                list(self.aqi_history),
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
        if aqi is not None:
            status = get_aqi_label(aqi)
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
                f"AQI has reached {int(aqi)} - {get_aqi_label(aqi)}",
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
                old_times = list(self.time_history)
                old_aqis = list(self.aqi_history)
                self.time_history = collections.deque(old_times, maxlen=self.history_size)
                self.aqi_history = collections.deque(old_aqis, maxlen=self.history_size)
            
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
            # Convert NaN to None for JSON compatibility (NaN marks disconnect gaps)
            import math
            aqis = [None if (isinstance(v, float) and math.isnan(v)) else v for v in self.aqi_history]
            data = {
                "times": [t.isoformat() for t in self.time_history],
                "aqis": aqis,
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
                
                # Parse timestamps and rebuild deques
                times = [datetime.fromisoformat(t) for t in data.get("times", [])]
                # Convert None back to NaN (gap markers)
                aqis = [float('nan') if v is None else v for v in data.get("aqis", [])]
                self.sample_count = data.get("sample_count", 0)
                
                # Extend deques (respects maxlen)
                self.time_history.extend(times)
                self.aqi_history.extend(aqis)
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
