import csv
import os
import time

import matplotlib.pyplot as plt

# ------------- CONFIG -------------
CSV_FILE = "sensor_log.csv"
SPEED = 5.0          # 1.0 = real time, 2.0 = 2x faster, 0.5 = half speed
MAX_POINTS = 300      # How many recent samples to show on screen
PRELOAD_SAMPLES = 300 # How many samples to load before starting timed replay
# ----------------------------------


def load_rows(csv_file):
    """Load all rows from the CSV file."""
    if not os.path.exists(csv_file):
        print(f"CSV file '{csv_file}' not found.")
        return []

    with open(csv_file, "r", newline="") as f:
        reader = csv.DictReader(f)
        rows = [row for row in reader]

    if not rows:
        print("CSV is empty or has no data rows.")
        return []

    return rows


def get_time_from_row(row, idx):
    """
    Choose a time value for this row.
    Priority:
      1. 'timestamp' (your device timestamp)
      2. 'pc_time' (if present)
      3. fallback to row index (seconds)
    Returns (time_value, is_real_timestamp)
    """
    if "timestamp" in row and row["timestamp"]:
        try:
            return float(row["timestamp"]), True
        except ValueError:
            pass

    if "pc_time" in row and row["pc_time"]:
        try:
            return float(row["pc_time"]), True
        except ValueError:
            pass

    # Fallback to index if nothing else works
    return float(idx), False


def parse_float(row, key):
    val = row.get(key)
    if val is None or val == "":
        return None
    try:
        return float(val)
    except ValueError:
        return None


def main():
    rows = load_rows(CSV_FILE)
    if not rows:
        return

    # Build list of (raw_time, row)
    time_row_pairs = []
    for idx, row in enumerate(rows):
        t_raw, is_real = get_time_from_row(row, idx)
        time_row_pairs.append((t_raw, is_real, row))

    # Sort by time just in case
    time_row_pairs.sort(key=lambda x: x[0])

    # Detect scale of the timestamps (ms vs s)
    times = [p[0] for p in time_row_pairs]
    deltas = [t2 - t1 for t1, t2 in zip(times[:-1], times[1:]) if t2 > t1]

    if deltas:
        avg_dt = sum(deltas) / len(deltas)
    else:
        avg_dt = 1.0

    # Simple heuristic:
    # If average delta > 50, assume timestamps are in ms, otherwise seconds.
    if avg_dt > 50:
        time_unit_scale = 0.001  # ms to s
        print("Detected millisecond timestamps. Converting to seconds.")
    else:
        time_unit_scale = 1.0    # already seconds
        print("Detected second-level timestamps or synthetic index.")

    # Set up live plot (similar to the visualizer script)
    plt.ion()
    fig, axs = plt.subplots(3, 1, sharex=True, figsize=(10, 8))
    ax_temp_hum = axs[0]
    ax_aqi = axs[1]
    ax_gases = axs[2]
    fig.suptitle("AirCube replay from CSV (timestamp based)")

    x_data = []
    temp_data = []
    hum_data = []
    aqi_data = []
    eco2_data = []
    etvoc_data = []

    total_samples = len(time_row_pairs)
    preload_n = min(PRELOAD_SAMPLES, total_samples)

    print(f"Total samples: {total_samples}")
    print(f"Preloading first {preload_n} samples (no delay)...")

    t0_raw = time_row_pairs[0][0]

    # -------- PRELOAD PHASE (no sleeping) --------
    for idx in range(preload_n):
        t_raw, is_real, row = time_row_pairs[idx]
        t_rel = (t_raw - t0_raw) * time_unit_scale

        temp_c = parse_float(row, "temperature_c")
        hum = parse_float(row, "humidity")
        aqi = parse_float(row, "aqi")
        eco2 = parse_float(row, "eco2")
        etvoc = parse_float(row, "etvoc")

        if temp_c is None or hum is None or aqi is None:
            continue

        x_data.append(t_rel)
        temp_data.append(temp_c)
        hum_data.append(hum)
        aqi_data.append(aqi)
        eco2_data.append(eco2 if eco2 is not None else float("nan"))
        etvoc_data.append(etvoc if etvoc is not None else float("nan"))

    # Limit history to MAX_POINTS (in case PRELOAD_SAMPLES > MAX_POINTS)
    x_data = x_data[-MAX_POINTS:]
    temp_data = temp_data[-MAX_POINTS:]
    hum_data = hum_data[-MAX_POINTS:]
    aqi_data = aqi_data[-MAX_POINTS:]
    eco2_data = eco2_data[-MAX_POINTS:]
    etvoc_data = etvoc_data[-MAX_POINTS:]

    # Initial draw after preload
    ax_temp_hum.cla()
    ax_aqi.cla()
    ax_gases.cla()

    ax_temp_hum.plot(x_data, temp_data, label="Temperature (C)")
    ax_temp_hum.plot(x_data, hum_data, label="Humidity (%)")
    ax_temp_hum.set_ylabel("Temp / Hum")
    ax_temp_hum.legend(loc="upper left")
    ax_temp_hum.grid(True)

    ax_aqi.plot(x_data, aqi_data, label="AQI")
    ax_aqi.set_ylabel("AQI")
    ax_aqi.legend(loc="upper left")
    ax_aqi.grid(True)

    ax_gases.plot(x_data, eco2_data, label="eCO2 (ppm)")
    ax_gases.plot(x_data, etvoc_data, label="eTVOC (ppb)")
    ax_gases.set_ylabel("Gas levels")
    ax_gases.set_xlabel("Time (s, replay)")
    ax_gases.legend(loc="upper left")
    ax_gases.grid(True)

    fig.tight_layout()
    fig.canvas.draw()
    fig.canvas.flush_events()

    # -------- TIMED REPLAY PHASE --------
    print(f"Starting timed replay from sample {preload_n} to {total_samples - 1} at {SPEED}x speed.")
    print("Close the plot window or press Ctrl+C in the terminal to stop.")

    # Use the last preloaded timestamp as the previous one
    if preload_n > 0:
        prev_raw_t = time_row_pairs[preload_n - 1][0]
    else:
        prev_raw_t = time_row_pairs[0][0]

    try:
        for idx in range(preload_n, total_samples):
            t_raw, is_real, row = time_row_pairs[idx]
            t_rel = (t_raw - t0_raw) * time_unit_scale

            # Sleep to simulate original timing from this point on
            dt_raw = t_raw - prev_raw_t
            dt = (dt_raw * time_unit_scale) / max(SPEED, 1e-6)
            if dt > 0:
                time.sleep(dt)
            prev_raw_t = t_raw

            temp_c = parse_float(row, "temperature_c")
            hum = parse_float(row, "humidity")
            aqi = parse_float(row, "aqi")
            eco2 = parse_float(row, "eco2")
            etvoc = parse_float(row, "etvoc")

            if temp_c is None or hum is None or aqi is None:
                continue

            x_data.append(t_rel)
            temp_data.append(temp_c)
            hum_data.append(hum)
            aqi_data.append(aqi)
            eco2_data.append(eco2 if eco2 is not None else float("nan"))
            etvoc_data.append(etvoc if etvoc is not None else float("nan"))

            # Limit history
            x_data = x_data[-MAX_POINTS:]
            temp_data = temp_data[-MAX_POINTS:]
            hum_data = hum_data[-MAX_POINTS:]
            aqi_data = aqi_data[-MAX_POINTS:]
            eco2_data = eco2_data[-MAX_POINTS:]
            etvoc_data = etvoc_data[-MAX_POINTS:]

            # Update plots
            ax_temp_hum.cla()
            ax_aqi.cla()
            ax_gases.cla()

            ax_temp_hum.plot(x_data, temp_data, label="Temperature (C)")
            ax_temp_hum.plot(x_data, hum_data, label="Humidity (%)")
            ax_temp_hum.set_ylabel("Temp / Hum")
            ax_temp_hum.legend(loc="upper left")
            ax_temp_hum.grid(True)

            ax_aqi.plot(x_data, aqi_data, label="AQI")
            ax_aqi.set_ylabel("AQI")
            ax_aqi.legend(loc="upper left")
            ax_aqi.grid(True)

            ax_gases.plot(x_data, eco2_data, label="eCO2 (ppm)")
            ax_gases.plot(x_data, etvoc_data, label="eTVOC (ppb)")
            ax_gases.set_ylabel("Gas levels")
            ax_gases.set_xlabel("Time (s, replay)")
            ax_gases.legend(loc="upper left")
            ax_gases.grid(True)

            fig.tight_layout()
            fig.canvas.draw()
            fig.canvas.flush_events()

        # Keep window open at the end
        plt.ioff()
        plt.show()

    except KeyboardInterrupt:
        print("\nReplay interrupted by user.")
        plt.ioff()
        plt.show()


if __name__ == "__main__":
    main()
