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

3. **Run the data logger** (in a separate terminal):
   ```bash
   cd scripts
   pip install pyserial matplotlib
   # Edit aircube_logger.py to set your COM port
   python aircube_logger.py
   ```

4. **Visualize the data** (in another terminal):
   ```bash
   python aircube_data_visualizer.py
   ```

That's it! Your AirCube should now be running and logging data.

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

Python utilities for data logging and visualization are located in the `scripts/` directory.

#### Installation

1. **Install Python dependencies**:
   ```bash
   pip install pyserial matplotlib
   ```

2. **Find your serial port**:
   - **Windows**: Check Device Manager → Ports (COM & LPT) or use `idf.py -p PORT monitor` to test
   - **Linux**: Usually `/dev/ttyUSB0` or `/dev/ttyACM0`. Check with `ls /dev/tty*`
   - **macOS**: Usually `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART`. Check with `ls /dev/cu.*`

#### Scripts

1. **aircube_logger.py** - Data Logger
   
   Logs sensor data from serial port to CSV file in real-time.
   
   **Configuration**: Edit the script to set:
   ```python
   PORT = "COM3"      # Your serial port
   BAUD = 115200      # Baud rate (default: 115200)
   CSV_FILE = "sensor_log.csv"  # Output filename
   ```
   
   **Usage**:
   ```bash
   cd scripts
   python aircube_logger.py
   ```
   
   The script will:
   - Create `sensor_log.csv` if it doesn't exist
   - Append new sensor readings as they arrive
   - Display logged data in the terminal
   - Press `Ctrl+C` to stop logging

2. **aircube_data_visualizer.py** - Live Data Visualization
   
   Real-time visualization of sensor data from the CSV file.
   
   **Configuration**: Edit the script to set:
   ```python
   CSV_FILE = "sensor_log.csv"  # Path to your CSV file
   MAX_POINTS = 300             # Number of recent samples to display
   UPDATE_INTERVAL_MS = 1000    # Refresh rate in milliseconds
   ```
   
   **Usage**:
   ```bash
   cd scripts
   python aircube_data_visualizer.py
   ```
   
   The script will:
   - Display three plots: Temperature/Humidity, AQI, and Gas levels
   - Auto-update as new data is logged
   - Show the last 300 samples (configurable)
   - Close the window to stop

3. **aircube_replay_script.py** - Data Replay
   
   Replays logged sensor data with timestamp-based timing for analysis.
   
   **Configuration**: Edit the script to set:
   ```python
   CSV_FILE = "sensor_log.csv"  # Path to your CSV file
   SPEED = 5.0                   # Playback speed (1.0 = real-time, 2.0 = 2x faster)
   MAX_POINTS = 300              # Number of samples to display
   ```
   
   **Usage**:
   ```bash
   cd scripts
   python aircube_replay_script.py
   ```
   
   The script will:
   - Load all data from the CSV
   - Replay it at the specified speed
   - Show the same visualization as the live viewer
   - Useful for analyzing historical data patterns

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
  - Install dependencies: `pip install pyserial matplotlib`
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
