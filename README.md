# AirCube

AirCube is an ESP32-H2 based environmental monitoring device that measures air quality, temperature, and humidity. It provides real-time visual feedback through an RGB LED and communicates sensor data via serial interface.

## 📺 Demo Video

Watch the AirCube in action: [YouTube Demo](https://youtu.be/m12KpLyLCrw)

## 🚀 Quick Start

### Prerequisites

- **Hardware**: AirCube device (see Hardware section for build instructions)
- **Software**:
  - [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) v5.0 or later
  - Python 3.7+ (for data logging/visualization scripts)
  - USB-C cable for programming and power

### Getting Started

1. **Clone the repository**:
   ```bash
   git clone https://github.com/yourusername/AirCube.git
   cd AirCube
   ```

2. **Build and flash the firmware**:
   ```bash
   cd firmware
   idf.py build
   idf.py -p COM3 flash monitor  # Replace COM3 with your port
   ```

3. **Run the desktop app**:
   ```bash
   cd scripts
   pip install -r requirements.txt
   python aircube_app.py
   ```
   The AirCube app will open. Select your serial port from the dropdown, click **Connect**, and you're live!

That's it! Your AirCube should now be running and showing live data.

## ✨ Features

- **Air Quality Monitoring**
  - eTVOC (equivalent Total Volatile Organic Compounds) in ppb
  - eCO2 (equivalent CO2) in ppm
  - AQI (Air Quality Index) calculation
  - Environmental compensation using temperature and humidity data

- **Environmental Sensors**
  - Temperature measurement (Celsius/Fahrenheit)
  - Relative humidity percentage
  - ENS210 temperature/humidity sensor
  - ENS161/ENS16X air quality sensor

- **Visual Feedback**
  - RGB LED (WS2812) with color-coded air quality indication
    - Green: Good air quality (AQI 0-10)
    - Yellow to Red: Degrading air quality (AQI 10-200)
    - Blue (pulsing): Sensor warming up
  - Smooth color transitions
  - Adjustable brightness via button control

- **Communication**
  - JSON-based serial protocol (115200 baud)
  - Configurable sensor readout period
  - Real-time sensor data streaming

- **Power Management**
  - Low-power operation support
  - Configurable CPU frequency

## Hardware

### Components

- **MCU**: ESP32-H2-MINI-1
- **Sensors**:
  - ENS210 (Temperature & Humidity)
  - ENS161/ENS16X (Air Quality)
- **LED**: WS2812 RGB LED
- **Interface**: USB-C connector
- **Power**: USB-C powered

### PCB Design

PCB design files are located in the `kicad/` directory:
- KiCad schematic and PCB layout files
- Gerber files for manufacturing
- BOM (Bill of Materials)
- 3D STEP files

### Mechanical Design

3D printed enclosure files are located in the `mechanical/` directory:
- Top and bottom enclosure parts
- STEP files for CAD integration

## 💻 Software Setup

### Firmware

The firmware is built using ESP-IDF and is located in the `firmware/` directory.

#### Prerequisites

1. **Install ESP-IDF**:
   - Follow the official [ESP-IDF Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
   - Make sure ESP-IDF v5.0 or later is installed
   - Set up the environment (run `get_idf` or `export.sh`/`export.bat`)

2. **Install USB drivers** (if needed):
   - Windows: Install [CP210x USB to UART Bridge drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
   - Linux: Usually works out of the box, may need to add user to `dialout` group
   - macOS: Usually works out of the box

#### Building and Flashing

1. **Navigate to firmware directory**:
   ```bash
   cd firmware
   ```

2. **Set the target** (ESP32-H2):
   ```bash
   idf.py set-target esp32h2
   ```

3. **Configure the project** (optional):
   ```bash
   idf.py menuconfig
   ```
   - Adjust serial port settings if needed
   - Configure power management
   - Modify sensor readout periods

4. **Build the project**:
   ```bash
   idf.py build
   ```

5. **Flash to device**:
   ```bash
   # Windows
   idf.py -p COM3 flash
   
   # Linux/macOS
   idf.py -p /dev/ttyUSB0 flash
   ```
   Replace `COM3` or `/dev/ttyUSB0` with your actual serial port.

6. **Monitor serial output**:
   ```bash
   idf.py -p COM3 monitor  # or /dev/ttyUSB0 on Linux/macOS
   ```
   Press `Ctrl+]` to exit the monitor.

7. **Flash and monitor in one command**:
   ```bash
   idf.py -p COM3 flash monitor
   ```

#### Firmware Structure

```
firmware/
├── main/
│   ├── main.c              # Main application code
│   ├── ens210.c/h          # ENS210 temperature/humidity sensor driver
│   ├── ens16x_driver.c/h   # ENS16X air quality sensor driver
│   ├── i2c_driver.c/h      # I2C communication driver
│   ├── led.c/h             # LED control
│   ├── led_color_lib.c/h   # Color conversion utilities
│   ├── ws2812_control.c/h  # WS2812 LED driver
│   ├── button.c/h          # Button input handling
│   └── serial_protocol.c/h # Serial communication protocol
├── CMakeLists.txt
└── sdkconfig              # ESP-IDF configuration
```

### Python Scripts

Python utilities for connecting to the AirCube, logging, and visualization are in the `scripts/` directory.

#### Installation

1. **Install Python dependencies** (from the `scripts/` directory):
   ```bash
   cd scripts
   pip install -r requirements.txt
   ```

#### Scripts

1. **aircube_app.py** - Desktop Application (recommended)
   
   A full graphical application for monitoring your AirCube in real-time.
   
   **Usage**:
   ```bash
   cd scripts
   python aircube_app.py
   ```
   
   **Features**:
   - **Port selection**: Dropdown to select serial port with refresh button
   - **One-click connect/disconnect**: Green Connect button, red Disconnect button
   - **Live sensor display**: Large, easy-to-read current values for Temperature, Humidity, AQI, eCO2, and eTVOC
   - **Color-coded AQI**: Value changes color based on air quality (green = good, yellow = moderate, orange = unhealthy, red = hazardous)
   - **Historical plots**: Three-panel chart showing Temperature/Humidity, AQI, and Gas levels over time
   - **CSV logging**: Optional checkbox to log data to CSV file (compatible with replay script)
   - **Configurable history**: Adjust how many data points to display (50-1000)
   - **Status bar**: Shows connection status and sample count

2. **aircube_logger.py** - Headless data logger
   
   Logs sensor data from serial to CSV only (no display). Use when matplotlib is not available or you only need a file. The standalone app with `--csv` replaces this for most use cases.
   
   **Configuration**: Edit the script to set `PORT`, `BAUD`, `CSV_FILE`.
   
   **Usage**:
   ```bash
   python aircube_logger.py
   ```

3. **aircube_data_visualizer.py** - CSV-only live viewer
   
   Watches a CSV file and displays live plots (no serial connection). Use when the cube is not connected but you have a CSV being written by another process.
   
   **Configuration**: Edit the script to set `CSV_FILE`, `MAX_POINTS`, `UPDATE_INTERVAL_MS`.
   
   **Usage**:
   ```bash
   python aircube_data_visualizer.py
   ```

4. **aircube_replay_script.py** - Data replay
   
   Replays logged sensor data from a CSV with timestamp-based timing for analysis.
   
   **Configuration**: Edit the script to set `CSV_FILE`, `SPEED`, `MAX_POINTS`.
   
   **Usage**:
   ```bash
   python aircube_replay_script.py
   ```
   
   The script will load the CSV, replay at the specified speed, and show the same three-panel visualization.

#### Building a Standalone Executable

You can build a standalone `.exe` (Windows) or app bundle (macOS) that doesn't require Python:

1. **Install PyInstaller**:
   ```bash
   pip install pyinstaller
   ```

2. **Run the build script**:
   ```bash
   cd scripts
   python build_exe.py
   ```

3. **Find the executable** in the `dist/` folder:
   - Windows: `dist/AirCube.exe`
   - macOS: `dist/AirCube.app`
   - Linux: `dist/AirCube`

The executable is fully self-contained and can be distributed to users without any Python installation.

**Optional**: Add an `aircube.ico` (Windows) or `aircube.icns` (macOS) file to the `scripts/` folder before building to include a custom app icon.

## 📡 Serial Protocol

The device communicates via UART at **115200 baud** using JSON messages.

### Sensor Data Output

The device periodically sends sensor data as JSON (default: every 1000ms, configurable):

```json
{
  "timestamp": 1234567890,
  "ens210": {
    "status": 0,
    "temperature_c": 22.5,
    "temperature_f": 72.5,
    "humidity": 45.2
  },
  "ens16x": {
    "status": "OK",
    "etvoc": 50,
    "eco2": 400,
    "aqi": 5
  }
}
```

### Commands

Commands can be sent to the device via serial (JSON format). See `firmware/main/serial_protocol.c` for available commands and implementation details.

**Example**: To change the sensor readout period, send a JSON command via serial.

## 💡 LED Color Mapping

The RGB LED provides real-time visual feedback based on air quality:

- **🟢 Green**: AQI 0-10 (Good air quality)
- **🟡 Yellow to 🔴 Red**: AQI 10-200 (Degrading air quality, smooth gradient)
- **🔵 Blue (pulsing)**: Sensor warming up (first ~60 seconds after power-on)
- **Smooth transitions**: Color changes smoothly over ~1 second for pleasant visual experience

**Brightness Control**: Press the button on the device to cycle through brightness levels.

## 🏗️ Building the Hardware

### PCB Assembly

1. **Order PCB**: Use the Gerber files in `kicad/gerbers/` to order from your preferred PCB manufacturer
2. **Order Components**: Use `kicad/AirCube v1.0 BOM.csv` to order components
3. **Assembly**: Follow the schematic in `kicad/AirCube.kicad_sch` for assembly
4. **Programming**: Use USB-C connector for programming and power

### 3D Printed Enclosure

1. **Print Files**: Use the STEP files in `mechanical/` directory
2. **Assembly**: Top and bottom parts snap together
3. **Mounting**: PCB mounts inside the enclosure

## 📁 Project Structure

```
AirCube/
├── firmware/          # ESP-IDF firmware source code
│   ├── main/          # Main application code
│   └── CMakeLists.txt # Build configuration
├── kicad/             # PCB design files (KiCad)
│   ├── gerbers/       # Manufacturing files
│   └── *.kicad_*      # KiCad project files
├── mechanical/        # 3D enclosure files
│   └── *.step         # CAD files for 3D printing
├── scripts/           # Python utilities
│   ├── aircube_app.py           # Desktop application (connect + display + optional CSV)
│   ├── build_exe.py             # Build script for standalone executable
│   ├── aircube.spec             # PyInstaller spec file
│   ├── requirements.txt
│   ├── aircube_logger.py
│   ├── aircube_data_visualizer.py
│   └── aircube_replay_script.py
└── README.md          # This file
```

## 🐛 Troubleshooting

### Firmware Issues

- **Device not detected**: 
  - Check USB cable (data-capable, not charge-only)
  - Install USB drivers (CP210x for Windows)
  - Check port permissions (Linux: add user to `dialout` group)

- **Build errors**:
  - Ensure ESP-IDF is properly installed and environment is set up
  - Run `idf.py fullclean` and rebuild
  - Check that you've set the target: `idf.py set-target esp32h2`

- **Flash errors**:
  - Put device in download mode (hold BOOT button, press RESET, release BOOT)
  - Try lowering baud rate in `menuconfig` → Serial flasher config

### Sensor Issues

- **No sensor data**:
  - Check I2C connections on PCB
  - Verify sensor initialization in serial monitor
  - Ensure sensors are properly powered

- **LED not working**:
  - Check WS2812 connections
  - Verify power supply (LEDs need stable 5V)
  - Check button functionality for brightness control

### Python Script Issues

- **Serial port not found**:
  - Verify the port name (Windows: `COM3`, Linux: `/dev/ttyUSB0`)
  - Ensure device is connected and drivers are installed
  - Check if another program is using the port

- **Import errors**:
  - Install dependencies: `pip install -r scripts/requirements.txt`
  - Use virtual environment if needed: `python -m venv venv`

## 🤝 Contributing

Contributions are welcome! Here's how you can help:

1. **Fork the repository**
2. **Create a feature branch**: `git checkout -b feature/amazing-feature`
3. **Make your changes** and test thoroughly
4. **Commit your changes**: `git commit -m 'Add amazing feature'`
5. **Push to the branch**: `git push origin feature/amazing-feature`
6. **Open a Pull Request**

### Areas for Contribution

- Additional sensor support
- Web interface for data visualization
- Mobile app integration
- Power optimization improvements
- Documentation improvements
- Bug fixes and testing

## 📄 License

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.
