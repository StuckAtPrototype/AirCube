/* One-time CO2 calibration prompt after an upgrade from firmware <= 2.0.4. */

import { confirmDialog, toast } from "./ui.js";
import { compareFw, fwLte } from "./protocol.js";

const UPGRADE_CEILING = "2.0.4";

let offering = false;

/**
 * If this cube just left firmware 2.0.4 or older, ask for a fresh-air FRC
 * the next time a live frame arrives. Empty previousFw means the cube never
 * reported a version (pre-2.0.3).
 */
export function markPendingIfUpgradedFrom204(device, previousFw, flashedVersion = "") {
  const fromOld = !previousFw || fwLte(previousFw, UPGRADE_CEILING);
  const toNew = Boolean(flashedVersion) && compareFw(flashedVersion, UPGRADE_CEILING) > 0;
  if (!fromOld || !toNew) return;
  device.pendingFrcNudge = true;
  device._frcNudgeOffered = false;
}

export function maybeOfferUpgradeCalibration(device) {
  if (offering || !device || !device.isPro || !device.isConnected || device.heldForFlash) {
    return;
  }
  if (!(device.frcNeeded || device.pendingFrcNudge) || device._frcNudgeOffered) {
    return;
  }
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
      /* still one-time for this browser session */
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
