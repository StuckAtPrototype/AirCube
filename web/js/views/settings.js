/* Settings: display preferences, per-cube device settings, firmware, about.
 *
 * Follows the tray's settings page (aircubeapp/ui/settings.py) minus the parts
 * that only make sense on a desktop app (notifications, tray, autostart), plus
 * the two firmware settings the serial protocol exposes but neither native app
 * surfaces today: auto-dim and the sensor readout period.
 */

import { h, clear, pillPicker, toggle, toast, confirmDialog } from "../ui.js";
import { prefs, applyAppearance } from "../prefs.js";

const APP_VERSION = "1.0.0";

function row(label, control, sub) {
  return h(
    "div.settings-row",
    h("div.label", h("div", { text: label }), sub ? h("span.sub", { text: sub }) : null),
    control,
  );
}

function numberField(value, { min, max, step = 1, disabled = false, onCommit }) {
  const input = h("input", { type: "number", min, max, step, value, disabled });
  const commit = () => {
    const next = Number(input.value);
    if (!Number.isFinite(next) || next < min || next > max) {
      input.value = value;
      toast(`Enter a value between ${min} and ${max}`, "err");
      return;
    }
    value = next;
    onCommit(next);
  };
  input.addEventListener("change", commit);
  return input;
}

export class SettingsView {
  constructor(registry, { onBack, onFlash }) {
    this.registry = registry;
    this.onBack = onBack;
    this.onFlash = onFlash;

    this.deviceSection = h("div");

    this.el = h(
      "div.column.narrow",
      h(
        "div.page-header",
        h("button.toolbtn", {
          type: "button",
          text: "\u2190",
          title: "Back",
          "aria-label": "Back",
          onclick: () => this.onBack(),
        }),
        h("h1.detail-title", { text: "Settings" }),
      ),
      this._displaySection(),
      this.deviceSection,
      this._firmwareSection(),
      this._aboutSection(),
    );
  }

  _displaySection() {
    const temperature = pillPicker(
      [
        { value: true, label: "Fahrenheit" },
        { value: false, label: "Celsius" },
      ],
      prefs.get("useFahrenheit"),
      (value) => prefs.set("useFahrenheit", value),
    );

    const appearance = pillPicker(
      [
        { value: "dark", label: "Dark" },
        { value: "light", label: "Light" },
        { value: "system", label: "System" },
      ],
      prefs.get("appearance"),
      (value) => {
        prefs.set("appearance", value);
        applyAppearance();
      },
    );

    return h(
      "div",
      h("h2.section-label", { text: "Display" }),
      h(
        "div.card.settings-group",
        row("Temperature", temperature),
        row("Appearance", appearance),
      ),
      h("div", { style: { height: "14px" } }),
    );
  }

  _firmwareSection() {
    return h(
      "div",
      h("h2.section-label", { text: "Firmware" }),
      h(
        "div.card.settings-group",
        row(
          "Update an AirCube",
          h("button.btn", {
            type: "button",
            text: "Flash firmware",
            onclick: () => this.onFlash(null),
          }),
          "Writes a release image over USB. Your brightness and Zigbee pairing survive the update.",
        ),
      ),
      h("div", { style: { height: "14px" } }),
    );
  }

  _aboutSection() {
    return h(
      "div",
      h("h2.section-label", { text: "About" }),
      h(
        "div.card.settings-group",
        row("AirCube Web", h("span.faint", { text: `v${APP_VERSION}` })),
        row(
          "Project",
          h("a", {
            href: "https://github.com/StuckAtPrototype/AirCube",
            target: "_blank",
            rel: "noreferrer",
            text: "github.com/StuckAtPrototype/AirCube",
          }),
        ),
        row(
          "Made by",
          h("a", {
            href: "https://stuckatprototype.com",
            target: "_blank",
            rel: "noreferrer",
            text: "StuckAtPrototype",
          }),
        ),
      ),
    );
  }

  refresh() {
    const devices = this.registry.devices.filter((d) => d.isConnected);
    // Rebuilding on every tick would steal focus from a field being edited, so
    // only redraw when the cubes or their settings actually changed.
    const signature = JSON.stringify(
      devices.map((d) => [d.id, d.name, d.isPro, d.config]),
    );
    if (signature === this._signature) return;
    this._signature = signature;

    clear(this.deviceSection);
    if (!devices.length) return;

    for (const device of devices) {
      this.deviceSection.append(this._deviceCard(device));
    }
    this.deviceSection.append(h("div", { style: { height: "14px" } }));
  }

  _deviceCard(device) {
    const config = device.config;
    const autoDim = config?.autoDim ?? {
      enabled: false,
      nightEnterLux: 5,
      dayExitLux: 15,
      nightDimPct: 10,
    };
    // Base hardware has no ambient light sensor, so the firmware disables
    // auto-dim there regardless of what we send.
    const proOnly = !device.isPro;

    const push = (patch) => {
      device
        .setAutoDim({ ...autoDim, ...patch })
        .catch((err) => toast(err.message, "err"));
    };

    const group = h(
      "div.card.settings-group",
      row(
        "Auto-dim at night",
        toggle(autoDim.enabled, (on) => push({ enabled: on }), { disabled: proOnly }),
        proOnly
          ? "Pro only. This cube has no ambient light sensor."
          : "Dims the LED once the room goes dark.",
      ),
      row(
        "Night starts below",
        numberField(autoDim.nightEnterLux, {
          min: 0,
          max: 1000,
          disabled: proOnly,
          onCommit: (v) => push({ nightEnterLux: v }),
        }),
        "lux",
      ),
      row(
        "Day resumes above",
        numberField(autoDim.dayExitLux, {
          min: 0,
          max: 1000,
          disabled: proOnly,
          onCommit: (v) => push({ dayExitLux: v }),
        }),
        "lux",
      ),
      row(
        "Night brightness",
        numberField(autoDim.nightDimPct, {
          min: 0,
          max: 100,
          disabled: proOnly,
          onCommit: (v) => push({ nightDimPct: v }),
        }),
        "percent of the configured brightness",
      ),
      row(
        "Sensor readout period",
        numberField(config?.readoutPeriod ?? 1000, {
          min: 100,
          max: 10000,
          step: 100,
          onCommit: (v) =>
            device.setReadoutPeriod(v).catch((err) => toast(err.message, "err")),
        }),
        "milliseconds between readings (100-10000)",
      ),
      row(
        "Calibrate CO2 to 425 ppm",
        h("button.btn", {
          type: "button",
          text: "Calibrate CO2",
          disabled: proOnly,
          onclick: () => this._runCo2Frc(device),
        }),
        proOnly
          ? "Pro only. This cube has no SCD41 CO2 sensor."
          : "Leave the cube in outdoor or open-window air for at least 10 minutes, then calibrate. Sets the current reading to 425 ppm.",
      ),
    );

    return h(
      "div",
      h("h2.section-label", { text: device.name }),
      group,
      h("div", { style: { height: "14px" } }),
    );
  }

  async _runCo2Frc(device) {
    const ok = await confirmDialog(
      "Calibrate CO2 to 425 ppm",
      "Leave the cube in outdoor or open-window air for at least 10 minutes before continuing. This sets whatever it is measuring right now to 425 ppm.",
      "Calibrate",
    );
    if (!ok) return;
    try {
      const correction = await device.runCo2Frc();
      toast(`CO2 calibrated to 425 ppm (correction ${correction})`);
    } catch (err) {
      toast(err.message, "err");
    }
  }
}
