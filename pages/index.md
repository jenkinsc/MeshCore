# MeshCore RAK9154 battery telemetry

Firmware for the **RAK4631 MeshCore repeater** that reads the real battery data from a
**RAK9154 Solar Battery** (Output 1, RAK SensorHub one-wire) and reports it through MeshCore's normal
telemetry, instead of the default 0.0 V.

[:material-download: Download the latest firmware](https://github.com/jenkinsc/MeshCore-RAK9154/releases/latest){ .md-button .md-button--primary }
[:fontawesome-brands-github: Source and README](https://github.com/jenkinsc/MeshCore-RAK9154){ .md-button }

## What you get

| Telemetry channel | Value |
|---|---|
| Channel 1 | Battery voltage, state of charge (%), charge current |
| Next channel | Battery temperature |

The stock `RAK_4631_repeater` build is unchanged. The new build is the separate target
`RAK_4631_repeater_RAK9154`.

## Install

1. Open the [latest release](https://github.com/jenkinsc/MeshCore-RAK9154/releases/latest) and download
   `RAK_4631_repeater_RAK9154-<version>.zip`.
2. Flash it to the RAK4631 as a DFU package (for example with the Adafruit nRF52 bootloader / nRF Connect).
3. On the repeater console run `rak9154 status`. A working link shows voltage, current, SOC and temperature.

See the [RAK9154 guide](RAK9154.md) for flags, console commands, troubleshooting and limitations.

## Status

Releases are built automatically from upstream MeshCore plus this change, and are unit-tested and compiled
for both targets. They are **not hardware-tested for every version**: flash a test node first.
