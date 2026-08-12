# AirCube Web

The browser app for AirCube: live readings, LED brightness, auto-dim and readout period, history with CSV export, and firmware flashing -- all over USB via the [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API).

Live at **[stuckatprototype.github.io/AirCube](https://stuckatprototype.github.io/AirCube/)**. Requires Chrome or Edge on a desktop OS.

## Running it locally

There is no build step. Serve the folder over HTTP and open it:

```
python -m http.server 8000 --directory web
```

Then browse to `http://localhost:8000/`. Web Serial needs a secure context, and `localhost` counts as one.

## Layout

| Path | Purpose |
|------|---------|
| `js/protocol.js` | JSON-lines serial protocol (see [CONTRIBUTING.md](../CONTRIBUTING.md)) |
| `js/quality.js` | Air-quality thresholds, labels and the 0-100 score |
| `js/serial.js` | Web Serial link with request/reply matching |
| `js/devices.js` | Multi-cube registry, hotplug, history sync |
| `js/flash.js` | esptool-js wrapper |
| `js/charts.js` | Sparkline, air gauge, history chart |
| `js/views/` | Home, detail, settings, flash dialog |
| `css/theme.css` | Design tokens |
| `vendor/` | esptool-js and uPlot, vendored as ES modules |

`js/quality.js` and `js/protocol.js` are ports of [`aircubeapp/models.py`](https://github.com/StuckAtPrototype/AirCubeTray) and `aircubeapp/protocol.py` from the tray app, which in turn mirror the iOS app's `Theme.swift`. If you change a threshold or a label in one, change it in all three so a cube reads the same everywhere.

## Firmware catalog

The flash dialog reads `firmware/manifest.json` and downloads binaries from this same origin. It cannot pull them from GitHub Releases directly: release assets redirect to `release-assets.githubusercontent.com`, which sends no `Access-Control-Allow-Origin` header, so the browser blocks the fetch.

[`scripts/build_web_manifest.py`](../scripts/build_web_manifest.py) stages the newest release binaries here and regenerates the manifest. The Pages workflow runs it on every deploy; the `.bin` files themselves are gitignored.

## Tests

[`scripts/selftest.html`](../scripts/selftest.html) drives the protocol parsers and all three views against a synthetic device. Serve the repository root and open it:

```
python -m http.server 8000
```

Then browse to `http://localhost:8000/scripts/selftest.html`. Every line should read `PASS`. Append `?light` to check the light palette.
