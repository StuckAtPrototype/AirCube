"""
AirCube - Air Quality Monitor
A standalone desktop application for the AirCube sensor device.
"""

__version__ = "1.0.0"
__app_name__ = "AirCube"

import collections
import csv
import json
import os
import re
import sys
from datetime import datetime

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QComboBox, QCheckBox, QFileDialog,
    QGroupBox, QStatusBar, QMessageBox, QSpinBox, QSplitter,
    QFrame, QGridLayout
)
from PyQt6.QtCore import QTimer, Qt, QThread, pyqtSignal
from PyQt6.QtGui import QFont, QIcon, QAction

import matplotlib
matplotlib.use('QtAgg')
from matplotlib.backends.backend_qtagg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure

import serial
from serial.tools import list_ports

# JSON pattern for parsing sensor data
JSON_PATTERN = re.compile(r"\{.*\}")

# CSV header compatible with other AirCube scripts
CSV_HEADER = [
    "timestamp", "ens210_status", "temperature_c", "temperature_f",
    "humidity", "ens16x_status", "etvoc", "eco2", "aqi"
]


def parse_json_line(line):
    """Parse a JSON sensor data line into a flat dict."""
    match = JSON_PATTERN.search(line)
    if not match:
        return None
    try:
        data = json.loads(match.group(0))
        return {
            "timestamp": data.get("timestamp"),
            "temperature_c": data["ens210"].get("temperature_c"),
            "temperature_f": data["ens210"].get("temperature_f"),
            "humidity": data["ens210"].get("humidity"),
            "ens210_status": data["ens210"].get("status"),
            "ens16x_status": data["ens16x"].get("status"),
            "etvoc": data["ens16x"].get("etvoc"),
            "eco2": data["ens16x"].get("eco2"),
            "aqi": data["ens16x"].get("aqi"),
        }
    except (KeyError, TypeError, json.JSONDecodeError):
        return None


class SerialReaderThread(QThread):
    """Background thread for reading serial data."""
    data_received = pyqtSignal(dict)
    error_occurred = pyqtSignal(str)
    
    def __init__(self, port, baud=115200):
        super().__init__()
        self.port = port
        self.baud = baud
        self.running = False
        self.serial = None
    
    def run(self):
        try:
            self.serial = serial.Serial(self.port, self.baud, timeout=0.1)
            self.running = True
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


class SensorDisplay(QFrame):
    """Widget showing current sensor values."""
    def __init__(self):
        super().__init__()
        self.setFrameStyle(QFrame.Shape.StyledPanel | QFrame.Shadow.Raised)
        self.setup_ui()
    
    def setup_ui(self):
        layout = QGridLayout(self)
        layout.setSpacing(15)
        
        # Style for value labels
        value_font = QFont("Segoe UI", 24, QFont.Weight.Bold)
        unit_font = QFont("Segoe UI", 12)
        label_font = QFont("Segoe UI", 10)
        
        # Temperature
        self.temp_label = QLabel("--.-")
        self.temp_label.setFont(value_font)
        self.temp_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        temp_unit = QLabel("°C")
        temp_unit.setFont(unit_font)
        temp_title = QLabel("Temperature")
        temp_title.setFont(label_font)
        temp_title.setStyleSheet("color: #666;")
        
        # Humidity
        self.humidity_label = QLabel("--.-")
        self.humidity_label.setFont(value_font)
        self.humidity_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        hum_unit = QLabel("%")
        hum_unit.setFont(unit_font)
        hum_title = QLabel("Humidity")
        hum_title.setFont(label_font)
        hum_title.setStyleSheet("color: #666;")
        
        # AQI
        self.aqi_label = QLabel("---")
        self.aqi_label.setFont(value_font)
        self.aqi_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        aqi_unit = QLabel("AQI")
        aqi_unit.setFont(unit_font)
        aqi_title = QLabel("Air Quality Index")
        aqi_title.setFont(label_font)
        aqi_title.setStyleSheet("color: #666;")
        
        # eCO2
        self.eco2_label = QLabel("----")
        self.eco2_label.setFont(value_font)
        self.eco2_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        eco2_unit = QLabel("ppm")
        eco2_unit.setFont(unit_font)
        eco2_title = QLabel("eCO2")
        eco2_title.setFont(label_font)
        eco2_title.setStyleSheet("color: #666;")
        
        # eTVOC
        self.etvoc_label = QLabel("----")
        self.etvoc_label.setFont(value_font)
        self.etvoc_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        etvoc_unit = QLabel("ppb")
        etvoc_unit.setFont(unit_font)
        etvoc_title = QLabel("eTVOC")
        etvoc_title.setFont(label_font)
        etvoc_title.setStyleSheet("color: #666;")
        
        # Layout grid
        col = 0
        for title, value, unit in [
            (temp_title, self.temp_label, temp_unit),
            (hum_title, self.humidity_label, hum_unit),
            (aqi_title, self.aqi_label, aqi_unit),
            (eco2_title, self.eco2_label, eco2_unit),
            (etvoc_title, self.etvoc_label, etvoc_unit),
        ]:
            box = QVBoxLayout()
            box.addWidget(title, alignment=Qt.AlignmentFlag.AlignCenter)
            row = QHBoxLayout()
            row.addWidget(value)
            row.addWidget(unit, alignment=Qt.AlignmentFlag.AlignBottom)
            box.addLayout(row)
            layout.addLayout(box, 0, col)
            col += 1
    
    def update_values(self, data):
        temp = data.get("temperature_c")
        hum = data.get("humidity")
        aqi = data.get("aqi")
        eco2 = data.get("eco2")
        etvoc = data.get("etvoc")
        
        if temp is not None:
            self.temp_label.setText(f"{temp:.1f}")
        if hum is not None:
            self.humidity_label.setText(f"{hum:.1f}")
        if aqi is not None:
            self.aqi_label.setText(f"{int(aqi)}")
            # Color code AQI
            if aqi <= 50:
                self.aqi_label.setStyleSheet("color: #2e7d32;")  # Green
            elif aqi <= 100:
                self.aqi_label.setStyleSheet("color: #f9a825;")  # Yellow
            elif aqi <= 150:
                self.aqi_label.setStyleSheet("color: #ef6c00;")  # Orange
            else:
                self.aqi_label.setStyleSheet("color: #c62828;")  # Red
        if eco2 is not None:
            self.eco2_label.setText(f"{int(eco2)}")
        if etvoc is not None:
            self.etvoc_label.setText(f"{int(etvoc)}")
    
    def clear_values(self):
        self.temp_label.setText("--.-")
        self.humidity_label.setText("--.-")
        self.aqi_label.setText("---")
        self.aqi_label.setStyleSheet("")
        self.eco2_label.setText("----")
        self.etvoc_label.setText("----")


class PlotCanvas(FigureCanvas):
    """Matplotlib canvas for plotting sensor data."""
    def __init__(self, parent=None):
        self.fig = Figure(figsize=(10, 6), dpi=100)
        self.fig.set_facecolor('#fafafa')
        super().__init__(self.fig)
        self.setParent(parent)
        
        # Create subplots
        self.ax_temp_hum = self.fig.add_subplot(311)
        self.ax_aqi = self.fig.add_subplot(312, sharex=self.ax_temp_hum)
        self.ax_gases = self.fig.add_subplot(313, sharex=self.ax_temp_hum)
        
        self.fig.tight_layout(pad=2.0)
        self.setup_plots()
    
    def setup_plots(self):
        """Initialize plot styling."""
        for ax in [self.ax_temp_hum, self.ax_aqi, self.ax_gases]:
            ax.set_facecolor('#ffffff')
            ax.grid(True, linestyle='--', alpha=0.7)
        
        self.ax_temp_hum.set_ylabel("Temp (°C) / Humidity (%)")
        self.ax_aqi.set_ylabel("AQI")
        self.ax_gases.set_ylabel("eCO2 (ppm) / eTVOC (ppb)")
        self.ax_gases.set_xlabel("Time (seconds)")
    
    def update_plot(self, x, temp, hum, aqi, eco2, etvoc):
        """Update all three plots with new data."""
        self.ax_temp_hum.cla()
        self.ax_aqi.cla()
        self.ax_gases.cla()
        
        # Temperature and Humidity
        self.ax_temp_hum.plot(x, temp, label="Temperature (°C)", color='#e53935', linewidth=1.5)
        self.ax_temp_hum.plot(x, hum, label="Humidity (%)", color='#1e88e5', linewidth=1.5)
        self.ax_temp_hum.set_ylabel("Temp / Humidity")
        self.ax_temp_hum.legend(loc="upper left", fontsize=8)
        self.ax_temp_hum.grid(True, linestyle='--', alpha=0.7)
        
        # AQI
        self.ax_aqi.plot(x, aqi, label="AQI", color='#7cb342', linewidth=1.5)
        self.ax_aqi.set_ylabel("AQI")
        self.ax_aqi.legend(loc="upper left", fontsize=8)
        self.ax_aqi.grid(True, linestyle='--', alpha=0.7)
        
        # Gases
        self.ax_gases.plot(x, eco2, label="eCO2 (ppm)", color='#8e24aa', linewidth=1.5)
        self.ax_gases.plot(x, etvoc, label="eTVOC (ppb)", color='#00897b', linewidth=1.5)
        self.ax_gases.set_ylabel("Gas levels")
        self.ax_gases.set_xlabel("Time (s)")
        self.ax_gases.legend(loc="upper left", fontsize=8)
        self.ax_gases.grid(True, linestyle='--', alpha=0.7)
        
        self.fig.tight_layout(pad=2.0)
        self.draw()


class AirCubeApp(QMainWindow):
    """Main application window."""
    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"{__app_name__} v{__version__} - Air Quality Monitor")
        self.setMinimumSize(900, 700)
        
        # Data storage
        self.max_points = 300
        self.data_buffer = collections.deque(maxlen=self.max_points)
        self.t0 = None
        self.sample_count = 0
        
        # Serial and CSV
        self.serial_thread = None
        self.csv_file = None
        self.csv_writer = None
        self.csv_path = None
        
        self.setup_ui()
        self.setup_timers()
        self.refresh_ports()
    
    def setup_ui(self):
        """Build the main UI."""
        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        main_layout.setSpacing(10)
        main_layout.setContentsMargins(10, 10, 10, 10)
        
        # Connection panel
        conn_group = QGroupBox("Connection")
        conn_layout = QHBoxLayout(conn_group)
        
        conn_layout.addWidget(QLabel("Port:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(150)
        conn_layout.addWidget(self.port_combo)
        
        self.refresh_btn = QPushButton("Refresh")
        self.refresh_btn.clicked.connect(self.refresh_ports)
        conn_layout.addWidget(self.refresh_btn)
        
        conn_layout.addSpacing(20)
        
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.setMinimumWidth(100)
        self.connect_btn.clicked.connect(self.toggle_connection)
        self.connect_btn.setStyleSheet("""
            QPushButton {
                background-color: #4CAF50;
                color: white;
                border: none;
                padding: 8px 16px;
                border-radius: 4px;
                font-weight: bold;
            }
            QPushButton:hover {
                background-color: #45a049;
            }
            QPushButton:disabled {
                background-color: #cccccc;
            }
        """)
        conn_layout.addWidget(self.connect_btn)
        
        conn_layout.addSpacing(30)
        
        # CSV logging
        self.csv_checkbox = QCheckBox("Log to CSV")
        self.csv_checkbox.stateChanged.connect(self.toggle_csv_logging)
        conn_layout.addWidget(self.csv_checkbox)
        
        self.csv_path_label = QLabel("No file selected")
        self.csv_path_label.setStyleSheet("color: #666; font-style: italic;")
        conn_layout.addWidget(self.csv_path_label)
        
        self.csv_browse_btn = QPushButton("Browse...")
        self.csv_browse_btn.clicked.connect(self.browse_csv)
        conn_layout.addWidget(self.csv_browse_btn)
        
        conn_layout.addStretch()
        
        # Settings
        conn_layout.addWidget(QLabel("History:"))
        self.history_spin = QSpinBox()
        self.history_spin.setRange(50, 1000)
        self.history_spin.setValue(300)
        self.history_spin.setSuffix(" pts")
        self.history_spin.valueChanged.connect(self.update_max_points)
        conn_layout.addWidget(self.history_spin)
        
        main_layout.addWidget(conn_group)
        
        # Sensor display panel
        self.sensor_display = SensorDisplay()
        main_layout.addWidget(self.sensor_display)
        
        # Plot canvas
        plot_group = QGroupBox("Sensor History")
        plot_layout = QVBoxLayout(plot_group)
        self.canvas = PlotCanvas()
        plot_layout.addWidget(self.canvas)
        main_layout.addWidget(plot_group, stretch=1)
        
        # Status bar
        self.status_bar = QStatusBar()
        self.setStatusBar(self.status_bar)
        
        self.connection_status = QLabel("Disconnected")
        self.connection_status.setStyleSheet("color: #c62828; font-weight: bold;")
        self.status_bar.addWidget(self.connection_status)
        
        self.sample_status = QLabel("Samples: 0")
        self.status_bar.addPermanentWidget(self.sample_status)
        
        self.csv_status = QLabel("")
        self.status_bar.addPermanentWidget(self.csv_status)
    
    def setup_timers(self):
        """Setup update timer for plots."""
        self.plot_timer = QTimer()
        self.plot_timer.timeout.connect(self.update_plot)
        self.plot_timer.start(500)  # Update plot every 500ms
    
    def refresh_ports(self):
        """Refresh the list of available serial ports."""
        self.port_combo.clear()
        ports = list_ports.comports()
        for p in ports:
            self.port_combo.addItem(f"{p.device} - {p.description}", p.device)
        if not ports:
            self.port_combo.addItem("No ports found", None)
    
    def toggle_connection(self):
        """Connect or disconnect from the serial port."""
        if self.serial_thread and self.serial_thread.running:
            self.disconnect_serial()
        else:
            self.connect_serial()
    
    def connect_serial(self):
        """Start serial connection."""
        port = self.port_combo.currentData()
        if not port:
            QMessageBox.warning(self, "No Port", "Please select a serial port.")
            return
        
        self.serial_thread = SerialReaderThread(port)
        self.serial_thread.data_received.connect(self.on_data_received)
        self.serial_thread.error_occurred.connect(self.on_serial_error)
        self.serial_thread.start()
        
        self.connect_btn.setText("Disconnect")
        self.connect_btn.setStyleSheet("""
            QPushButton {
                background-color: #f44336;
                color: white;
                border: none;
                padding: 8px 16px;
                border-radius: 4px;
                font-weight: bold;
            }
            QPushButton:hover {
                background-color: #da190b;
            }
        """)
        self.port_combo.setEnabled(False)
        self.refresh_btn.setEnabled(False)
        
        self.connection_status.setText(f"Connected to {port}")
        self.connection_status.setStyleSheet("color: #2e7d32; font-weight: bold;")
        
        # Reset data
        self.data_buffer.clear()
        self.t0 = None
        self.sample_count = 0
        self.sensor_display.clear_values()
    
    def disconnect_serial(self):
        """Stop serial connection."""
        if self.serial_thread:
            self.serial_thread.stop()
            self.serial_thread = None
        
        self.connect_btn.setText("Connect")
        self.connect_btn.setStyleSheet("""
            QPushButton {
                background-color: #4CAF50;
                color: white;
                border: none;
                padding: 8px 16px;
                border-radius: 4px;
                font-weight: bold;
            }
            QPushButton:hover {
                background-color: #45a049;
            }
        """)
        self.port_combo.setEnabled(True)
        self.refresh_btn.setEnabled(True)
        
        self.connection_status.setText("Disconnected")
        self.connection_status.setStyleSheet("color: #c62828; font-weight: bold;")
    
    def on_data_received(self, data):
        """Handle incoming sensor data."""
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
        
        temp_c = data.get("temperature_c")
        hum = data.get("humidity")
        aqi = data.get("aqi")
        eco2 = data.get("eco2")
        etvoc = data.get("etvoc")
        
        if temp_c is None or hum is None or aqi is None:
            return
        
        try:
            temp_c = float(temp_c)
            hum = float(hum)
            aqi = float(aqi)
            eco2 = float(eco2) if eco2 is not None else float("nan")
            etvoc = float(etvoc) if etvoc is not None else float("nan")
        except (TypeError, ValueError):
            return
        
        # Store data
        self.data_buffer.append((t_rel, temp_c, hum, aqi, eco2, etvoc))
        self.sample_count += 1
        self.sample_status.setText(f"Samples: {self.sample_count}")
        
        # Update display
        self.sensor_display.update_values(data)
        
        # Write to CSV
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
            ]
            self.csv_writer.writerow(row)
            self.csv_file.flush()
    
    def on_serial_error(self, error):
        """Handle serial errors."""
        QMessageBox.critical(self, "Serial Error", f"Serial connection error:\n{error}")
        self.disconnect_serial()
    
    def update_plot(self):
        """Update the plot with current data."""
        if not self.data_buffer:
            return
        
        x = [p[0] for p in self.data_buffer]
        temp = [p[1] for p in self.data_buffer]
        hum = [p[2] for p in self.data_buffer]
        aqi = [p[3] for p in self.data_buffer]
        eco2 = [p[4] for p in self.data_buffer]
        etvoc = [p[5] for p in self.data_buffer]
        
        self.canvas.update_plot(x, temp, hum, aqi, eco2, etvoc)
    
    def update_max_points(self, value):
        """Update the data buffer size."""
        self.max_points = value
        old_data = list(self.data_buffer)
        self.data_buffer = collections.deque(old_data[-value:], maxlen=value)
    
    def toggle_csv_logging(self, state):
        """Enable or disable CSV logging."""
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
        """Browse for CSV file location."""
        default_name = f"aircube_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        path, _ = QFileDialog.getSaveFileName(
            self, "Save CSV Log", default_name, "CSV Files (*.csv)"
        )
        if path:
            self.csv_path = path
            self.csv_path_label.setText(os.path.basename(path))
            self.csv_path_label.setStyleSheet("color: #333;")
    
    def start_csv_logging(self):
        """Start logging to CSV file."""
        if not self.csv_path:
            return
        
        new_file = not os.path.exists(self.csv_path) or os.path.getsize(self.csv_path) == 0
        self.csv_file = open(self.csv_path, "a", newline="")
        self.csv_writer = csv.writer(self.csv_file)
        
        if new_file:
            self.csv_writer.writerow(CSV_HEADER)
            self.csv_file.flush()
        
        self.csv_status.setText(f"Logging to {os.path.basename(self.csv_path)}")
        self.csv_status.setStyleSheet("color: #2e7d32;")
    
    def stop_csv_logging(self):
        """Stop logging to CSV file."""
        if self.csv_file:
            self.csv_file.close()
            self.csv_file = None
            self.csv_writer = None
        self.csv_status.setText("")
    
    def closeEvent(self, event):
        """Handle window close."""
        self.disconnect_serial()
        self.stop_csv_logging()
        event.accept()


def main():
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    
    # Set application-wide font
    font = QFont("Segoe UI", 10)
    app.setFont(font)
    
    # Set stylesheet for modern look
    app.setStyleSheet("""
        QMainWindow {
            background-color: #f5f5f5;
        }
        QGroupBox {
            font-weight: bold;
            border: 1px solid #ddd;
            border-radius: 6px;
            margin-top: 12px;
            padding-top: 10px;
            background-color: white;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 5px;
        }
        QComboBox, QSpinBox {
            padding: 5px;
            border: 1px solid #ccc;
            border-radius: 4px;
            background: white;
        }
        QComboBox:hover, QSpinBox:hover {
            border-color: #999;
        }
        QCheckBox {
            spacing: 8px;
        }
        QStatusBar {
            background-color: #e0e0e0;
        }
    """)
    
    window = AirCubeApp()
    window.show()
    
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
