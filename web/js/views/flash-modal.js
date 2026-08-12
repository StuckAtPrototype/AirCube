/* Flash firmware dialog.
 * Ports aircubetray/aircubeapp/ui/flash_dialog.py to the browser.
 */

import { h, openModal, progressBar, toast } from "../ui.js";
import { loadManifest, fetchImage, readLocalFile, looksLikeEspImage, formatSize } from "../firmware.js";
import { flashDevice, reconnectAfterFlash, MERGED_IMAGE_ADDRESS, APP_ONLY_ADDRESS } from "../flash.js";

export async function openFlashModal(registry, preselectedDevice) {
  const { backdrop, body, close } = openModal("Flash firmware");

  const deviceSelect = h("select");
  const firmwareSelect = h("select");
  const offsetSelect = h(
    "select",
    h("option", { value: String(MERGED_IMAGE_ADDRESS), text: "0x0 (merged release image)" }),
    h("option", { value: String(APP_ONLY_ADDRESS), text: "0x10000 (app-only build)" }),
  );
  const noteLine = h("div.faint", { style: { fontSize: "11px" } });
  const bar = progressBar();
  bar.style.display = "none";
  const log = h("pre.log");
  const flashBtn = h("button.btn.primary", { type: "button", text: "Flash" });
  const closeBtn = h("button.btn", { type: "button", text: "Close", onclick: close });
  const fileInput = h("input", {
    type: "file",
    accept: ".bin",
    style: { display: "none" },
  });

  let localImage = null;
  let releases = [];

  const write = (line, kind = "info") => {
    const entry = h("span", { text: `${line}\n` });
    if (kind !== "info") entry.className = kind;
    log.append(entry);
    log.scrollTop = log.scrollHeight;
  };

  // ------------------------------------------------------------- population

  function refreshDevices() {
    const previous = deviceSelect.value;
    deviceSelect.replaceChildren();
    const devices = registry.devices;
    if (!devices.length) {
      deviceSelect.append(h("option", { value: "", text: "No AirCube connected" }));
    }
    for (const device of devices) {
      deviceSelect.append(
        h("option", {
          value: device.id,
          text: `${device.name} — ${device.modelLabel}${device.isConnected ? "" : " (offline)"}`,
        }),
      );
    }
    const wanted = preselectedDevice?.id || previous;
    if (wanted && devices.some((d) => d.id === wanted)) deviceSelect.value = wanted;
    updateFlashEnabled();
  }

  function refreshNote() {
    if (localImage) {
      noteLine.textContent = `${localImage.name} · ${formatSize(localImage.bytes.length)}`;
      return;
    }
    const entry = releases.find((r) => r.version === firmwareSelect.value);
    noteLine.textContent = entry
      ? `${entry.file} · ${formatSize(entry.size)}${entry.publishedAt ? ` · released ${entry.publishedAt.slice(0, 10)}` : ""}`
      : "";
  }

  function updateFlashEnabled() {
    const hasDevice = Boolean(deviceSelect.value);
    const hasImage = Boolean(localImage) || releases.length > 0;
    flashBtn.disabled = !hasDevice || !hasImage;
  }

  // ------------------------------------------------------------------ flash

  async function run() {
    const device = registry.byId(deviceSelect.value);
    if (!device) return;

    const address = Number(offsetSelect.value);
    backdrop.dataset.locked = "true";
    flashBtn.disabled = true;
    closeBtn.disabled = true;
    deviceSelect.disabled = true;
    firmwareSelect.disabled = true;
    offsetSelect.disabled = true;
    bar.style.display = "";
    bar.setFraction(0);

    let flashedVersion = "";
    try {
      let image;
      if (localImage) {
        image = localImage.bytes;
        write(`Using local file ${localImage.name}`);
      } else {
        const entry = releases.find((r) => r.version === firmwareSelect.value);
        write(`Downloading ${entry.file}...`);
        image = await fetchImage(entry, (fraction) => bar.setFraction(fraction * 0.15));
        flashedVersion = entry.version;
        write(`Downloaded and verified ${formatSize(image.length)}`, "ok");
      }

      if (address === MERGED_IMAGE_ADDRESS && !looksLikeEspImage(image)) {
        write(
          "Warning: this file does not start with the usual ESP image magic byte. If it is an app-only build, select the 0x10000 offset instead.",
          "err",
        );
      }

      await flashDevice({
        device,
        image,
        address,
        onLog: write,
        onProgress: (fraction) => bar.setFraction(0.15 + fraction * 0.85),
      });

      if (flashedVersion) device.fwVersion = flashedVersion;
      write("Reconnecting...");
      const reconnected = await reconnectAfterFlash(device);
      write(
        reconnected
          ? "Reconnected. Live readings should resume shortly."
          : "Could not reopen the port automatically. Unplug the AirCube and plug it back in.",
        reconnected ? "ok" : "err",
      );
      toast("Firmware flashed");
    } catch (err) {
      write(`FAILED: ${err.message || err}`, "err");
      toast("Flashing failed. See the log for details.", "err");
      await device.releaseFlashHold().catch(() => {});
      device.endFlashGrace();
    } finally {
      backdrop.dataset.locked = "false";
      closeBtn.disabled = false;
      deviceSelect.disabled = false;
      firmwareSelect.disabled = false;
      offsetSelect.disabled = false;
      updateFlashEnabled();
    }
  }

  // ------------------------------------------------------------------ wiring

  flashBtn.addEventListener("click", run);
  firmwareSelect.addEventListener("change", () => {
    localImage = null;
    refreshNote();
  });
  deviceSelect.addEventListener("change", updateFlashEnabled);
  fileInput.addEventListener("change", async () => {
    const file = fileInput.files?.[0];
    if (!file) return;
    localImage = { name: file.name, bytes: await readLocalFile(file) };
    firmwareSelect.value = "";
    refreshNote();
    updateFlashEnabled();
  });

  body.append(
    h("p.muted", {
      style: { margin: "0", lineHeight: "1.6" },
      text:
        "Flashing takes about 30 seconds over USB. Keep the cable connected until it finishes. Your brightness setting and Zigbee pairing are preserved.",
    }),
    h("div.field", h("label", { text: "AirCube" }), deviceSelect,
      h("button.btn", { type: "button", text: "Refresh", onclick: refreshDevices })),
    h("div.field", h("label", { text: "Firmware" }), firmwareSelect,
      h("button.btn", { type: "button", text: "Local .bin", onclick: () => fileInput.click() })),
    noteLine,
    h("div.field", h("label", { text: "Flash offset" }), offsetSelect),
    bar,
    log,
    h("div.actions", closeBtn, flashBtn),
    fileInput,
  );

  refreshDevices();
  write("Ready.");

  const manifest = await loadManifest();
  releases = manifest.releases;
  if (releases.length) {
    for (const entry of releases) {
      firmwareSelect.append(
        h("option", {
          value: entry.version,
          text: `v${entry.version}${entry.version === manifest.latest ? " (latest)" : ""}`,
        }),
      );
    }
    firmwareSelect.value = manifest.latest || releases[0].version;
  } else {
    firmwareSelect.append(h("option", { value: "", text: "No hosted releases found" }));
    write("No firmware catalog found. Use the Local .bin button to pick a file.");
  }
  refreshNote();
  updateFlashEnabled();
}
