import csv
import os
import time

import matplotlib.pyplot as plt
import matplotlib.animation as animation

# ------------- CONFIG -------------
CSV_FILE = "sensor_log.csv"
MAX_POINTS = 300          # How many recent samples to show
UPDATE_INTERVAL_MS = 1000 # How often to refresh plot in ms
# ----------------------------------


def read_csv_data(csv_file):
    """
    Reads the CSV and returns a dict of lists:
    {
        "x": [...],
        "temperature_c": [...],
        "humidity": [...],
        "aqi": [...],
        "eco2": [...],
        "etvoc": [...]
    }
    """
    if not os.path.exists(csv_file):
        return None

    with open(csv_file, "r", newline="") as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames
        if not fieldnames:
            return None

        # Decide what to use as x axis
        # Prefer pc_time, then timestamp, otherwise just index
        has_pc_time = "pc_time" in fieldnames
        has_timestamp = "timestamp" in fieldnames

        x = []
        temperature_c = []
        humidity = []
        aqi = []
        eco2 = []
        etvoc = []

        for idx, row in enumerate(reader):
            try:
                if has_pc_time:
                    # Use relative time for readability
                    t = float(row["pc_time"])
                elif has_timestamp:
                    t = float(row["timestamp"])
                else:
                    t = float(idx)

                # Parse numeric values, skip row if anything fails
                temp_c = float(row.get("temperature_c", "nan"))
                hum = float(row.get("humidity", "nan"))
                aqi_val = float(row.get("aqi", "nan"))
                eco2_val = float(row.get("eco2", "nan"))
                etvoc_val = float(row.get("etvoc", "nan"))

                x.append(t)
                temperature_c.append(temp_c)
                humidity.append(hum)
                aqi.append(aqi_val)
                eco2.append(eco2_val)
                etvoc.append(etvoc_val)
            except ValueError:
                # If any field is missing or malformed, skip that row
                continue

        if not x:
            return None

        # If using real timestamps, normalize to start at zero for nicer axis
        if has_pc_time or has_timestamp:
            t0 = x[0]
            x = [t - t0 for t in x]

        # Limit to last MAX_POINTS
        x = x[-MAX_POINTS:]
        temperature_c = temperature_c[-MAX_POINTS:]
        humidity = humidity[-MAX_POINTS:]
        aqi = aqi[-MAX_POINTS:]
        eco2 = eco2[-MAX_POINTS:]
        etvoc = etvoc[-MAX_POINTS:]

        return {
            "x": x,
            "temperature_c": temperature_c,
            "humidity": humidity,
            "aqi": aqi,
            "eco2": eco2,
            "etvoc": etvoc,
        }


def main():
    # Prepare figure with 3 stacked plots
    fig, axs = plt.subplots(3, 1, sharex=True, figsize=(10, 8))
    ax_temp_hum = axs[0]
    ax_aqi = axs[1]
    ax_gases = axs[2]

    fig.suptitle("AirCube live data (from CSV)")

    def update(frame):
        data = read_csv_data(CSV_FILE)
        if data is None:
            return

        x = data["x"]
        temp_c = data["temperature_c"]
        hum = data["humidity"]
        aqi = data["aqi"]
        eco2 = data["eco2"]
        etvoc = data["etvoc"]

        # Clear each axis
        ax_temp_hum.cla()
        ax_aqi.cla()
        ax_gases.cla()

        # Top: temperature and humidity
        ax_temp_hum.plot(x, temp_c, label="Temperature (C)")
        ax_temp_hum.plot(x, hum, label="Humidity (%)")
        ax_temp_hum.set_ylabel("Temp / Hum")
        ax_temp_hum.legend(loc="upper left")
        ax_temp_hum.grid(True)

        # Middle: AQI
        ax_aqi.plot(x, aqi, label="AQI")
        ax_aqi.set_ylabel("AQI")
        ax_aqi.legend(loc="upper left")
        ax_aqi.grid(True)

        # Bottom: gases (eCO2 and eTVOC)
        ax_gases.plot(x, eco2, label="eCO2 (ppm)")
        ax_gases.plot(x, etvoc, label="eTVOC (ppb)")
        ax_gases.set_ylabel("Gas levels")
        ax_gases.set_xlabel("Time (seconds or samples)")
        ax_gases.legend(loc="upper left")
        ax_gases.grid(True)

        plt.tight_layout()

    ani = animation.FuncAnimation(
        fig,
        update,
        interval=UPDATE_INTERVAL_MS
    )

    print(f"Watching {CSV_FILE} for updates. Close the window to stop.")
    plt.show()


if __name__ == "__main__":
    main()
