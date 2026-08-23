/* One-time CO2 calibration prompt after an upgrade from firmware <= 2.0.4. */

import { confirmDialog, toast } from "./ui.js";
import { compareFw, fwLte } from "./protocol.js";
import { frcSettled } from "./devices.js";

const UPGRADE_CEILING = "2.0.4";

let offering = false;

/**
 * If this cube just crossed from firmware 2.0.4 or older up to something
 * newer, ask for a fresh-air FRC the next time a live frame arrives.
 *
 * A cube whose previous version is unknown does not count as an upgrade: a
 * reflash of the same version would otherwise look like one, since a cube that
 * has not streamed a live frame yet reports no version at all. Genuinely old
 * firmware is caught by the cube's own frc_needed flag instead.
 */
export function markPendingIfUpgradedFrom204(device, previousFw, flashedVersion = "") {
  if (frcSettled(device.slot)) return;
  const fromOld = fwLte(previousFw, UPGRADE_CEILING);
  const toNew = compareFw(flashedVersion, UPGRADE_CEILING) > 0;
  if (!fromOld || !toNew) return;
  device.pendingFrcNudge = true;
  device._frcNudgeOffered = false;
}

export function maybeOfferUpgradeCalibration(device) {
  if (offering || !device || !device.isPro || !device.isConnected || device.heldForFlash) {
    return;
  }
  if (!(device.frcNeeded || device.pendingFrcNudge)) return;

  // Flashing erases the cube's NVS, so a cube that has already been calibrated
  // asks again after every update. Its SCD41 kept the calibration through the
  // reflash, so answer on the user's behalf rather than prompting a second time.
  if (frcSettled(device.slot)) {
    device.pendingFrcNudge = false;
    if (device.frcNeeded) device.dismissFrcNudge().catch(() => {});
    return;
  }

  if (device._frcNudgeOffered) return;
  // Another dialog (flash, settings confirm) is already up; try again on the
  // next live reading after it closes.
  if (document.querySelector(".modal-backdrop")) return;

  device._frcNudgeOffered = true;
  offering = true;
  offer(device).finally(() => {
    offering = false;
  });
}

async function offer(device) {
  const ok = await confirmDialog(
    "Calibrate CO2 after this update",
    "This AirCube was updated from firmware 2.0.4 or older. Automatic CO2 self-calibration is now off, so a stuffy room cannot slowly drag the reading down. Calibration is required once a year. Move the cube to fresh air or open a window for at least 10 minutes, then press Calibrate. That sets the current reading to 425 ppm. You can always recalibrate the device from the menu.",
    "Calibrate now",
    "Later",
  );
  device.pendingFrcNudge = false;
  if (!ok) {
    try {
      await device.dismissFrcNudge();
    } catch {
      /* the cube kept its flag, but this browser has recorded the answer */
    }
    return;
  }
  try {
    const correction = await device.runCo2Frc();
    toast(`CO2 calibrated to 425 ppm (correction ${correction})`);
  } catch (err) {
    toast(err.message || String(err), "err");
  }
}
