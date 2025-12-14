import serial
import json
import csv
import re

# ---- CONFIG ----
PORT = "COM33"  # Change to match your serial port
BAUD = 115200  # Change if needed
CSV_FILE = "sensor_log.csv"

# ---- JSON pattern: matches JSON starting with { and ending with } ----
json_pattern = re.compile(r'\{.*\}')

def ensure_csv_header(writer):
    # Define the expected CSV fields
    header = [
        "timestamp",
        "ens210_status",
        "temperature_c",
        "temperature_f",
        "humidity",
        "ens16x_status",
        "etvoc",
        "eco2",
        "aqi"
    ]
    writer.writerow(header)


def main():
    ser = serial.Serial(PORT, BAUD, timeout=1)

    # Create file + header if doesn't exist or empty
    try:
        open(CSV_FILE, "x").close()
        new_file = True
    except FileExistsError:
        new_file = False

    with open(CSV_FILE, "a", newline="") as csvfile:
        writer = csv.writer(csvfile)

        if new_file:
            ensure_csv_header(writer)

        print("Logging JSON to CSV... Press CTRL+C to stop.")

        while True:
            line = ser.readline().decode(errors="ignore").strip()

            match = json_pattern.search(line)
            if not match:
                continue

            try:
                data = json.loads(match.group(0))

                # Flatten the JSON into row format
                row = [
                    data.get("timestamp"),
                    data["ens210"].get("status"),
                    data["ens210"].get("temperature_c"),
                    data["ens210"].get("temperature_f"),
                    data["ens210"].get("humidity"),
                    data["ens16x"].get("status"),
                    data["ens16x"].get("etvoc"),
                    data["ens16x"].get("eco2"),
                    data["ens16x"].get("aqi"),
                ]

                writer.writerow(row)
                csvfile.flush()  # Save every row immediately

                print("Logged row:", row)

            except Exception as e:
                print("JSON parse error:", e)
                continue


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nLogging stopped.")
