# Assembling Your AirCube

This guide walks through putting an AirCube together from printed parts and a populated PCB -- **Base** first, then **Pro**. Each model takes about ten minutes with a single screwdriver.

You'll need a populated AirCube PCB. If you don't have one, grab the [AirCube Populated PCB](https://stuckatprototype.com/products/aircube-populated-pcb) or build one from the KiCad files in [kicad/](kicad).

> Already assembled? Head back to **[Getting Started](README.md#getting-started)**.

---

## What You Need

| Item | Notes |
|------|-------|
| **3x M2 x 5 mm screws** | Self-tapping, for plastic. Longer screws work too -- they just bottom out deeper. |
| **Small Phillips screwdriver** | PH0 or similar. Hand-driven, no power tools. |
| **Populated AirCube PCB** | Base or Pro. |
| **All printed parts for your model** | See below. |

### Printed parts

**Base** -- files in [mechanical/base](mechanical/base):

- `AirCube bottom_prod_12-11.step` -- bottom shell, holds the PCB
- `AirCube top 20mm PROD.step` -- top shell
- `AirCube air wall 11-7-43.3mf` -- air wall, separates the sensor from the rest of the cube

**Pro** -- files in [mechanical/pro](mechanical/pro):

- `AirCube bottom_prod_12-11.step` -- bottom shell, holds the PCB
- `AirCube_top-7-16-26-716.stl` -- top shell
- `AirCube_air_wall_7-14-26-1149.stl` -- air wall
- `AirCube_light_wall-7-10-26-1034.stl` -- light wall, shapes the LED glow
- `AirCube_button_7-15-26-514.stl` -- button cap

Print everything before you start. A missing air wall means taking the whole cube apart again.

---

## Base Assembly

### 1. Gather your screws

Three **M2 x 5 mm** self-tapping screws. Longer screws are fine.

### 2. Print all the parts

Bottom, top, and air wall -- all three on the bench before you begin.

![Base printed parts and screws laid out](mechanical/guide_images/base_A.jpg)

### 3. Drop in the PCB, USB-C end first

Angle the board so the USB-C connector slides into its opening in the bottom shell, then lower the rest of the board flat into the pocket.

![PCB seated in the bottom shell with the USB-C port through its opening](mechanical/guide_images/base_B.jpg)

### 4. Fit the front screw only

The bottom shell has three screw holes. Drive only the one nearest the front edge for now -- it holds the PCB down and sets up the alignment for the air wall.

![Underside of the bottom shell showing the three screw holes](mechanical/guide_images/base_C.jpg)

Leave the other two holes empty until the top is on.

![Front screw driven, air wall standing by](mechanical/guide_images/base_D.jpg)

### 5. Install the air wall

Drop the air wall into the cutout as pictured. It should sit flat against the PCB with no gap along the side wall.

![Air wall seated against the PCB](mechanical/guide_images/base_E.jpg)

### 6. Install the top

Lower the top onto the bottom. Line it up so the USB-C opening at the back stays fully clear -- if the port is pinched or partly covered, lift the top and reseat it.

![Top installed with the USB-C opening clear and the screw holes accessible](mechanical/guide_images/base_F.jpg)

### 7. Secure the first screw

Drive the front screw home. **Snug, not tight** -- these are self-tapping screws in plastic and it's easy to strip the thread.

### 8. Install the other two screws

Same treatment for the remaining two holes. Done.

![Finished Base AirCube](mechanical/guide_images/base_G.jpg)

---

## Pro Assembly

Pro adds two parts to the Base sequence: a button cap and a light wall.

### 1. Gather your screws

Three **M2 x 5 mm** self-tapping screws. Longer screws are fine.

### 2. Print all the parts

Bottom, top, air wall, light wall, and button cap.

![Pro printed parts, button cap and screws laid out](mechanical/guide_images/pro_A.jpg)

### 3. Put the button into the enclosure

Drop the button cap into its slot in the bottom shell from the inside. It goes in before the PCB -- the board is what holds it captive.

![Bottom shell interior with the button slot](mechanical/guide_images/pro_B.jpg)

### 4. Drop in the PCB, USB-C end first

Angle the board so the USB-C connector slides into its opening, then lower it flat. Check the button cap is still seated and moves freely against the switch.

![PCB seated in the bottom shell, button cap captured](mechanical/guide_images/pro_C.jpg)

### 5. Fit the front screw only

Drive only the screw nearest the front edge. It holds the PCB down and sets up the alignment for the air wall.

![Front screw driven, remaining holes empty](mechanical/guide_images/pro_E.jpg)

### 6. Install the light wall

Slide the light wall into its pocket along the side wall, as pictured.

![Light wall seated in its pocket along the side wall](mechanical/guide_images/pro_D.jpg)

### 7. Install the air wall

Drop the air wall into the cutout as pictured. It should sit flat with no gap along the side wall.

![Air wall seated against the PCB](mechanical/guide_images/pro_F.jpg)

### 8. Install the top

Lower the top on, lined up so the USB-C opening at the back stays fully clear.

![Top installed with the USB-C opening clear and the three screw holes accessible](mechanical/guide_images/pro_G.jpg)

### 9. Secure the first screw

Drive the front screw home. **Snug, not tight** -- self-tapping screws strip plastic threads easily.

### 10. Install the other two screws

Same treatment for the remaining two holes. Done.

![Finished Pro AirCube](mechanical/guide_images/pro_H.jpg)

---

## Tips and Troubleshooting

**Don't overtighten.** Every screw here is a self-tapping screw biting into printed plastic. Stop as soon as it feels snug -- another quarter turn is how you strip the hole.

**Check the USB-C opening before you drive any screws.** With the top resting in place, look down the back face. The port should be fully exposed. Clamping a misaligned top down with screws can crack the shell.

**Air wall won't sit flush?** Back the front screw out a turn or two, reseat the air wall, then retighten. The front screw sets PCB alignment, so a board that's sitting slightly proud leaves the wall standing off.

**Button feels stuck (Pro)?** Take the top off and check the cap is square in its slot. It should have a little free travel and click against the switch on the board.

**Stripped a screw hole?** A slightly longer M2 screw usually bites into fresh plastic deeper down.

---

## Next Steps

| | |
|---|---|
| [Getting Started](README.md#getting-started) | Power it up and read the LED |
| [Firmware Update Guide](FIRMWARE_UPDATE.md) | Flash the latest firmware from your browser |
| [Home Assistant Guide](HOME_ASSISTANT.md) | ZHA and Zigbee2MQTT setup |
| [Contributing Guide](CONTRIBUTING.md) | Build from source, hardware and firmware architecture |
