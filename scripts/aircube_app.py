"""
AirCube - Air Quality Monitor
A standalone desktop application for the AirCube sensor device.
"""

__version__ = "1.0.0"
__app_name__ = "AirCube"

import collections
import csv
import os
import sys
from datetime import datetime

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QComboBox, QCheckBox, QFileDialog,
    QGroupBox, QStatusBar, QMessageBox, QSpinBox
)
from PyQt6.QtCore import QTimer, Qt, QSettings
from PyQt6.QtGui import QActionGroup

from serial.tools import list_ports

from aircube_ui.serial_reader import SerialReaderThread
from aircube_ui.theme import (
    Mode, Unit, LIGHT, resolve_palette, build_stylesheet, accent_button_qss, ui_font
)
from aircube_ui.widgets import SensorDisplay
from aircube_ui.plotting import PlotCanvas

# CSV header compatible with other AirCube scripts
CSV_HEADER = [
    "timestamp", "ens210_status", "temperature_c", "temperature_f",
    "humidity", "ens16x_status", "etvoc", "eco2", "aqi"
]


class AirCubeApp(QMainWindow):
    """Main application window."""
    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"{__app_name__} v{__version__} - Air Quality Monitor")
        self.setMinimumSize(900, 700)

        # Appearance and unit settings (persisted via QSettings).
        # Default to Light to preserve the original out-of-box look; System
        # (follow the OS) and Dark are opt-in via View > Appearance.
        self.settings = QSettings("StuckAtPrototype", "AirCube")
        self.mode = Mode(self.settings.value("appearance", Mode.LIGHT.value))
        self.unit = Unit(self.settings.value("unit", Unit.CELSIUS.value))
        self._palette = LIGHT

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

        # Apply persisted appearance/unit now that widgets exist.
        self.apply_mode(self.mode)
        self.sensor_display.set_unit(self.unit)
        self.canvas.set_unit(self.unit)

    def setup_menu(self):
        """Build the View menu (Appearance and Units)."""
        menubar = self.menuBar()
        view_menu = menubar.addMenu("View")

        appearance_menu = view_menu.addMenu("Appearance")
        self.mode_group = QActionGroup(self)
        for label, mode in [("System", Mode.SYSTEM), ("Light", Mode.LIGHT), ("Dark", Mode.DARK)]:
            act = appearance_menu.addAction(label)
            act.setCheckable(True)
            act.setChecked(self.mode == mode)
            act.triggered.connect(lambda _checked, m=mode: self.apply_mode(m))
            self.mode_group.addAction(act)

        units_menu = view_menu.addMenu("Units")
        self.unit_group = QActionGroup(self)
        for label, unit in [("Celsius (°C)", Unit.CELSIUS), ("Fahrenheit (°F)", Unit.FAHRENHEIT)]:
            act = units_menu.addAction(label)
            act.setCheckable(True)
            act.setChecked(self.unit == unit)
            act.triggered.connect(lambda _checked, u=unit: self.apply_unit(u))
            self.unit_group.addAction(act)

    def setup_ui(self):
        """Build the main UI."""
        self.setup_menu()
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
        conn_layout.addWidget(self.connect_btn)

        conn_layout.addSpacing(30)

        # CSV logging
        self.csv_checkbox = QCheckBox("Log to CSV")
        self.csv_checkbox.stateChanged.connect(self.toggle_csv_logging)
        conn_layout.addWidget(self.csv_checkbox)

        self.csv_path_label = QLabel("No file selected")
        self.csv_path_label.setStyleSheet(
            f"color: {self._palette.text_muted}; font-style: italic;"
        )
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
        self.connection_status.setStyleSheet(
            f"color: {self._palette.danger}; font-weight: 600;"
        )
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
        self.connect_btn.setStyleSheet(
            accent_button_qss(self._palette.danger, self._palette.danger_hover, self._palette.text_muted)
        )
        self.port_combo.setEnabled(False)
        self.refresh_btn.setEnabled(False)

        self.connection_status.setText(f"Connected to {port}")
        self.connection_status.setStyleSheet(
            f"color: {self._palette.success}; font-weight: 600;"
        )

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
        self.connect_btn.setStyleSheet(
            accent_button_qss(self._palette.start, self._palette.start_hover, self._palette.text_muted)
        )
        self.port_combo.setEnabled(True)
        self.refresh_btn.setEnabled(True)

        self.connection_status.setText("Disconnected")
        self.connection_status.setStyleSheet(
            f"color: {self._palette.danger}; font-weight: 600;"
        )

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
            self.csv_path_label.setStyleSheet(f"color: {self._palette.text_secondary};")

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
        self.csv_status.setStyleSheet(f"color: {self._palette.success};")

    def stop_csv_logging(self):
        """Stop logging to CSV file."""
        if self.csv_file:
            self.csv_file.close()
            self.csv_file = None
            self.csv_writer = None
        self.csv_status.setText("")

    def apply_mode(self, mode):
        """Apply and persist an appearance mode (System/Light/Dark)."""
        self.mode = mode
        self.settings.setValue("appearance", mode.value)
        self._palette = resolve_palette(mode, QApplication.instance())
        QApplication.instance().setStyleSheet(build_stylesheet(self._palette))
        self.sensor_display.apply_palette(self._palette)
        self.canvas.apply_palette(self._palette)

        connected = bool(self.serial_thread and self.serial_thread.running)
        if connected:
            self.connect_btn.setStyleSheet(
                accent_button_qss(self._palette.danger, self._palette.danger_hover, self._palette.text_muted)
            )
            self.connection_status.setStyleSheet(
                f"color: {self._palette.success}; font-weight: 600;"
            )
        else:
            self.connect_btn.setStyleSheet(
                accent_button_qss(self._palette.start, self._palette.start_hover, self._palette.text_muted)
            )
            self.connection_status.setStyleSheet(
                f"color: {self._palette.danger}; font-weight: 600;"
            )

        # Re-theme the CSV labels for the active palette.
        if self.csv_path:
            self.csv_path_label.setStyleSheet(f"color: {self._palette.text_secondary};")
        else:
            self.csv_path_label.setStyleSheet(
                f"color: {self._palette.text_muted}; font-style: italic;"
            )
        if self.csv_writer:
            self.csv_status.setStyleSheet(f"color: {self._palette.success};")

    def apply_unit(self, unit):
        """Apply and persist a temperature display unit (Celsius/Fahrenheit)."""
        self.unit = unit
        self.settings.setValue("unit", unit.value)
        self.sensor_display.set_unit(unit)
        self.canvas.set_unit(unit)
        if self.data_buffer:
            self.update_plot()

    def closeEvent(self, event):
        """Handle window close."""
        self.disconnect_serial()
        self.stop_csv_logging()
        event.accept()


def main():
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    app.setFont(ui_font(10))
    window = AirCubeApp()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
