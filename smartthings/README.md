# SmartThings Edge driver (Zigbee)

This folder contains the **AirCube Zigbee** Edge driver source for Samsung SmartThings hubs.

**User setup:** see **[SMARTTHINGS.md](../SMARTTHINGS.md)** in the repository root for step-by-step pairing, CLI install, channel workflow, and verification in the app and [Advanced Web App](https://my.smartthings.com/advanced).

**Layout**

```text
smartthings/
├── README.md                 # This file
├── driver-channel.json       # Input for `edge:channels:create -i` (TOS URL preset)
└── aircube-zigbee/           # Driver package (config, fingerprints, profile, Lua)
    ├── config.yml
    ├── fingerprints.yml
    ├── profiles/
    └── src/
```
