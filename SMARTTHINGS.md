# Connecting AirCube to SmartThings (Samsung hub)

This guide walks you through pairing AirCube to a **SmartThings-compatible Zigbee hub** and installing the community **AirCube Zigbee** Edge driver so you get **temperature, humidity, eCO2, eTVOC, and AQI** in the SmartThings app.

> **Default behavior:** Without this driver, SmartThings often joins AirCube as a generic **humidity / temperature** device. You only get **temp and humidity**. The Edge driver adds the **air quality** sensors over Zigbee cluster `0xFC01` (same idea as the [ZHA quirk](zha/aircube.py) and [Zigbee2MQTT converter](z2m/aircube.js)).

**You will need**

- AirCube powered over USB-C
- A **SmartThings Edge**–capable hub (e.g. SmartThings v3, Aeotec Smart Home Hub, SmartThings Station) on your Samsung account
- A **Mac, Windows, or Linux PC** for the [SmartThings CLI](https://github.com/SmartThingsCommunity/smartthings-cli) (one-time driver install)
- A [Samsung account](https://account.samsung.com/) used with SmartThings

The first time the CLI needs your SmartThings account, it **starts a browser sign-in** automatically (you may not see a separate `login` subcommand in `smartthings --help`). You do **not** need to create a Personal Access Token in the developer portal for normal interactive use. For **automation / CI**, you can use a [PAT](https://developer.smartthings.com/docs/personal-access-tokens) with channel scopes — see [driver channels](https://developer.smartthings.com/docs/devices/hub-connected/driver-channels).

---

## Step 1 — Put AirCube in pairing mode

1. Plug in AirCube (or, if already powered, use the next step).
2. **Hold the button for about 3 seconds** until the LEDs **flash blue** (Zigbee pairing mode).

See the [README](README.md) LED / button table if you need a refresher.

---

## Step 2 — Add AirCube in the SmartThings mobile app

1. Open the **SmartThings** app → **Add device** (or **+**).
2. Choose **Scan nearby** / **Zigbee** / **Generic Zigbee device** per your app version, and enable **pairing** on the hub when prompted.
3. Wait until AirCube appears and finish naming / room assignment.

**Check:** Open the new device — you should see **temperature** and **humidity**.  
**eCO2, TVOC, and AQI will not appear yet** until the **AirCube Zigbee** driver is installed and selected (next steps).

---

## Step 3 — Install the SmartThings CLI (sign-in is automatic)

1. Install the CLI using the [official instructions](https://github.com/SmartThingsCommunity/smartthings-cli) for your OS (Homebrew on Mac: `brew install smartthings`).
2. Run `smartthings --version` and confirm the command works.

**Authentication:** Many recent builds do **not** list a `smartthings login` command. The **first command that calls the SmartThings API** (for example **`smartthings edge:drivers:package .`** in Step 5) will **open a browser** so you can sign in with your Samsung account. Complete that flow once; later CLI commands reuse the session.

- To **sign out** on this machine: `smartthings logout` (then the next API command will prompt for login again).
- **Optional:** Headless or automation-only setups can use a **Personal Access Token** instead; see [CLI authentication](https://developer.smartthings.com/docs/sdks/cli/).

---

## Step 4 — Get the Edge driver source

From your clone of **this repository** (folder name may differ):

```bash
cd smartthings/aircube-zigbee
```

If you have not cloned yet:

```bash
git clone https://github.com/StuckAtPrototype/AirCube.git
cd AirCube/smartthings/aircube-zigbee
```

That directory contains `config.yml`, `fingerprints.yml`, `profiles/`, and `src/init.lua`.

---

## Step 5 — Package the driver (build + upload)

From `smartthings/aircube-zigbee`:

```bash
smartthings edge:drivers:package .
```

The **first** time you run this (or any API command), the CLI may **open the browser** for Samsung sign-in — complete it, then re-run `package` if needed.

The CLI prints a **Driver Id** and **Version** (timestamp string). **Save both** — you need them to assign the driver to a channel.

Optional: also write a local zip:

```bash
smartthings edge:drivers:package . -b ./aircube-zigbee.zip
```

> Recent CLI versions **upload** the driver when you package; there is **no** separate `publish` command.

---

## Step 6 — Create a driver channel (first time only)

You need a **driver channel** to install your own driver on the hub.

1. Create a JSON file (example below) — e.g. `channel.json` in any folder.

```json
{
  "name": "My AirCube test channel",
  "description": "Private channel for AirCube Edge driver",
  "termsOfServiceUrl": "https://example.com",
  "type": "DRIVER"
}
```

2. Run:

```bash
smartthings edge:channels:create -i channel.json
```

3. List channels and copy your **channel id** (UUID):

```bash
smartthings edge:channels
```

An example template is also in [`smartthings/channel-driver-example.json`](smartthings/channel-driver-example.json).

---

## Step 7 — Assign the packaged driver to your channel

```bash
smartthings edge:channels:assign <DRIVER_ID> "<VERSION>" --channel <CHANNEL_ID>
```

Use the **Driver Id** and **Version** from Step 5. You can omit arguments and use **interactive prompts** instead.

---

## Step 8 — Enroll your hub, then install the driver

**Order matters:** enroll the hub on the channel **before** install.

```bash
smartthings edge:channels:enroll --channel <CHANNEL_ID>
```

Pick your hub when prompted (or pass flags if your CLI supports them).

Then install the driver **onto that hub** from the same channel:

```bash
smartthings edge:drivers:install --channel <CHANNEL_ID> --hub <HUB_DEVICE_UUID>
```

### Hub id must be a UUID

`<HUB_DEVICE_UUID>` looks like `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`. It is **not** the sticker MAC, Zigbee EUI, or `networkId` string.

**Where to find it**

- [SmartThings Advanced Web App](https://my.smartthings.com/advanced) → open your **hub** as a device → copy **Device id**, **or**
- Open any Zigbee child device (e.g. AirCube) → **Parent device** / hub → copy that hub’s id.

If you omit `--hub`, the CLI usually offers a **list** — picking from the list avoids typos.

If install returns **HTTP 400**, see [Troubleshooting](#troubleshooting) below.

---

## Step 9 — Use the AirCube Zigbee driver for your device

The hub must run **AirCube Zigbee** instead of the generic **Zigbee Humidity Sensor** driver.

**Option A — Switch driver (keep device / room)**

```bash
smartthings edge:drivers:switch <DEVICE_ID>
```

Use the AirCube **device** id from the Advanced Web App (not the hub id). Follow prompts to select **AirCube Zigbee**.

**Option B — Remove and re-pair**

Remove AirCube from SmartThings, then add it again (Step 2) **after** the driver is installed on the hub. With a matching fingerprint, SmartThings should pick **AirCube Zigbee** automatically.

---

## Step 10 — Confirm in the Advanced Web App

1. Open [SmartThings Advanced Web App](https://my.smartthings.com/advanced).
2. Select **AirCube**.
3. Check **Driver** (or device summary): it should show **AirCube Zigbee** (or your packaged driver name), not only “Zigbee Humidity Sensor”.
4. Under capabilities / state, you should eventually see **carbon dioxide**, **TVOC**, and **air quality** style attributes after reports arrive (allow a few minutes for sensor warm-up).

---

## Step 11 — Confirm in the mobile app

Open the device card: you should see **temperature**, **humidity**, **CO₂**, **TVOC**, and **air quality** (labels may vary slightly by app version).

Gas sensors may read **0** for the first few minutes after power-on — that is normal while the ENS16x warms up (same as [Home Assistant](HOME_ASSISTANT.md)).

---

## Troubleshooting

| Problem | What to try |
|---------|-------------|
| Only temp / humidity | Driver not applied — repeat [Step 9](#step-9--use-the-aircube-zigbee-driver-for-your-device). |
| `drivers:install` **400 Bad Request** | Use **hub UUID**, not sticker id. Run **`channels:enroll`** for the same channel first. |
| eCO2 / TVOC / AQI stuck at 0 | Wait **3–5+ minutes** after power-on. Ensure driver is **AirCube Zigbee** and hub is **online**. |
| CLI / channel errors | Run `smartthings logout` then repeat the command that triggered login (browser). If you use a **PAT** instead, confirm it includes **channel** read/write. See [driver channels](https://developer.smartthings.com/docs/devices/hub-connected/driver-channels). |

---

## Technical reference

Zigbee layout (endpoint **10**): standard temp/humidity clusters; custom air quality cluster **`0xFC01`**; LED brightness uses Analog Output **`0x000D`** (not exposed in this driver v1). Details match [CONTRIBUTING.md](CONTRIBUTING.md) → *Zigbee Integration*.

---

## Contributors

If you improve the Edge driver or this guide, open a **pull request** on the [AirCube](https://github.com/StuckAtPrototype/AirCube) repository so all users get the update.
