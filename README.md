# MeshCore for RAK4631 + RAK9154 Solar Battery

**A MeshCore repeater build that reports the real battery voltage, charge, current and temperature of a RAK9154 Solar Battery**
(instead of `0.0 V`) on the RAK4631 / WisMesh repeater. Keywords: RAK9154, RAK4631, WisMesh, WisBlock, solar repeater,
battery telemetry. This is a fork of [MeshCore](https://github.com/meshcore-dev/MeshCore); everything else is upstream.
**[Jump to what this adds, status, install and how to keep it updated](#-this-fork-rak9154-solar-battery-telemetry-for-the-rak4631-repeater)**
 | [Latest firmware](../../releases/latest) | [Docs site](https://jenkinsc.github.io/MeshCore-RAK9154/)

## About MeshCore

MeshCore is a lightweight, portable C++ library that enables multi-hop packet routing for embedded projects using LoRa and other packet radios. It is designed for developers who want to create resilient, decentralized communication networks that work without the internet.

## 🔍 What is MeshCore?

MeshCore now supports a range of LoRa devices, allowing for easy flashing without the need to compile firmware manually. Users can flash a pre-built binary using tools like Adafruit ESPTool and interact with the network through a serial console.
MeshCore provides the ability to create wireless mesh networks, similar to Meshtastic and Reticulum but with a focus on lightweight multi-hop packet routing for embedded projects. Unlike Meshtastic, which is tailored for casual LoRa communication, or Reticulum, which offers advanced networking, MeshCore balances simplicity with scalability, making it ideal for custom embedded solutions, where devices (nodes) can communicate over long distances by relaying messages through intermediate nodes. This is especially useful in off-grid, emergency, or tactical situations where traditional communication infrastructure is unavailable.

## ⚡ Key Features

* Multi-Hop Packet Routing
  * Devices can forward messages across multiple nodes, extending range beyond a single radio's reach.
  * Supports up to a configurable number of hops to balance network efficiency and prevent excessive traffic.
  * Nodes use fixed roles where "Companion" nodes are not repeating messages at all to prevent adverse routing paths from being used.
* Supports LoRa Radios – Works with Heltec, RAK Wireless, and other LoRa-based hardware.
* Decentralized & Resilient – No central server or internet required; the network is self-healing.
* Low Power Consumption – Ideal for battery-powered or solar-powered devices.
* Simple to Deploy – Pre-built example applications make it easy to get started.

## 🎯 What Can You Use MeshCore For?

* Off-Grid Communication: Stay connected even in remote areas.
* Emergency Response & Disaster Recovery: Set up instant networks where infrastructure is down.
* Outdoor Activities: Hiking, camping, and adventure racing communication.
* Tactical & Security Applications: Military, law enforcement, and private security use cases.
* IoT & Sensor Networks: Collect data from remote sensors and relay it back to a central location.

## 🚀 How to Get Started

- Watch the [MeshCore QuickStart Playlist](https://www.youtube.com/watch?v=iaFltojJrAc&list=PLshzThxhw4O4WU_iZo3NmNZOv6KMrUuF9) by The Comms Channel
- Watch the [MeshCore Technical Presentation](https://www.youtube.com/watch?v=OwmkVkZQTf4) by Liam Cottle.
- Read through our [Frequently Asked Questions](./docs/faq.md) and [Documentation](https://docs.meshcore.io).
- Flash the MeshCore firmware on a supported device.
- Connect with a supported client.

For developers:

- Install [PlatformIO](https://docs.platformio.org) in [Visual Studio Code](https://code.visualstudio.com).
- Clone and open the MeshCore repository in Visual Studio Code.
- See the example applications you can modify and run:
  - [Companion Radio](./examples/companion_radio) - For use with an external chat app, over BLE, USB or Wi-Fi.
  - [KISS Modem](./examples/kiss_modem) - Serial KISS protocol bridge for host applications. ([protocol docs](./docs/kiss_modem_protocol.md))
  - [Simple Repeater](./examples/simple_repeater) - Extends network coverage by relaying messages.
  - [Simple Room Server](./examples/simple_room_server) - A simple BBS server for shared Posts.
  - [Simple Secure Chat](./examples/simple_secure_chat) - Secure terminal based text communication between devices.
  - [Simple Sensor](./examples/simple_sensor) - Remote sensor node with telemetry and alerting.

The Simple Secure Chat example can be interacted with through the Serial Monitor in Visual Studio Code, or with a Serial USB Terminal on Android.

## ⚡️ MeshCore Flasher

We have prebuilt firmware ready to flash on supported devices.

- Launch https://meshcore.io/flasher
- Select a supported device
- Flash one of the firmware types:
  - Companion, Repeater or Room Server
- Once flashing is complete, you can connect with one of the MeshCore clients below.

## 📱 MeshCore Clients

**Companion Firmware**

The companion firmware can be connected to via BLE, USB or Wi-Fi depending on the firmware type you flashed.

- Web: https://app.meshcore.nz
- Android: https://play.google.com/store/apps/details?id=com.liamcottle.meshcore.android
- iOS: https://apps.apple.com/us/app/meshcore/id6742354151?platform=iphone
- NodeJS: https://github.com/liamcottle/meshcore.js
- Python: https://github.com/fdlamotte/meshcore-cli

**Repeater and Room Server Firmware**

The repeater and room server firmware can be set up via USB in the web config tool.

- https://config.meshcore.io

They can also be managed via LoRa in the mobile app by using the Remote Management feature.

## 🔋 This fork: RAK9154 solar battery telemetry for the RAK4631 repeater

> This repository is a fork of [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore). It adds **one new,
> optional build target** that makes a RAK4631 (WisMesh) repeater report the **real battery data of a RAK9154 Solar
> Battery** in MeshCore's normal telemetry. Everything else is upstream MeshCore; the stock targets are unchanged.
> Base: upstream `main` at commit `81dbae59` (MeshCore 1.18.0).

### The problem

A RAK4631 repeater powered by a RAK9154 Solar Battery shows **0.0 V** for its battery in MeshCore. The stock firmware
measures the battery with the board's own ADC, and the RAK9154 is not connected to it. The RAK9154 does measure
everything (voltage, current, state of charge, temperature, faults), but it publishes it on its **Output 1** port using
RAK's *SensorHub* one-wire serial protocol, which MeshCore does not speak.

### What this fork provides

A new PlatformIO environment, **`RAK_4631_repeater_RAK9154`**. It is the stock `RAK_4631_repeater` plus the flag
`-D RAK9154_BATTERY`, a small driver that talks to the RAK9154, and a few lines that hand its data to MeshCore's existing
telemetry. Nothing in the MeshCore radio protocol changed, so the node still interoperates with any other MeshCore node.

What you see in the MeshCore app's **Telemetry** screen for the repeater:

| Telemetry channel | Value | Source |
|---|---|---|
| 1 | Battery **voltage** | RAK9154 (this is MeshCore's own battery field, now fed by the probe). The percentage the app shows next to it is the *app's* estimate from the voltage |
| 1 | **State of charge** (%) | RAK9154 |
| 1 | **Charge current** | RAK9154 (charging only, see limitations) |
| 1 | Chip temperature | nRF52 MCU, unchanged MeshCore behaviour, *not* the battery |
| next free channel (normally 2) | **Battery temperature** | RAK9154 |

Also available on the repeater's admin console: a `rak9154` command with live status and diagnostics (below), and a
decoder for the probe's battery fault flags and firmware version (visible in the console, not yet published over the mesh).

If the probe is disconnected or silent, the repeater **falls back to the stock battery reading** and never blocks or
hangs; stale data (older than 5 minutes) is dropped rather than shown.

### Status: what is and is not verified

Verified on real hardware (a WisMesh RAK4631 repeater with a RAK9154 whose probe identifies as model `RAK2560-io`,
hardware 0x44, firmware 1.2.9):
- The probe is reached on the default pin (P0.15) and answers; the firmware decodes its announcement and sensor list.
- Console showed `V:12.31V I:41mA SOC:99% T:21.0C ok:15 to:0 cs:0 inv:0` (valid frames, no timeouts, no checksum errors), and
  the app's Telemetry screen showed the battery on channel 1.

Not verified yet:
- The voltage has not been compared against a multimeter; the values are plausible but treat them as unconfirmed until you do.
- The **sign of the current** (which direction is "charging") is assumed, not confirmed.
- The two `0xF3` words (battery faults / firmware version) have not been received from a real probe yet.
- Long-term stability (days) and behaviour with other RAK9154/probe firmware versions.
- Only one hardware unit has been tested. Reports from other setups are very welcome.

### How it works

```
RAK9154 Output 1 ──one wire──> nRF52 pin (default P0.15)
                                   │  9600 8N1, half duplex (bit-banged, GPIOTE edge interrupt + DWT timing)
                                   ▼
                  RAK9154HalfSerial   byte transport
                  RAK9154Protocol     frames, checksum, decoding (pure code, unit tested)
                  RAK9154Battery      state machine + cache: provision the probe, poll every 30 s, validate
                                   │
                  ┌────────────────┴─────────────────┐
                  ▼                                  ▼
   getBattMilliVolts()  (cached, O(1))     telemetry hook (SOC, current, battery temperature)
```

Design rules that were followed on purpose:
- `getBattMilliVolts()` is **instant**: it only reads a cached value and never touches the wire.
- Polling is slow (about every 30 s, clamped to 15-60 s) and receive work is bounded per loop. The only blocking step
  is transmitting a request (about 14 ms; the occasional reply to the probe's announcement takes about 130 ms).
- A value is only cached from a frame whose checksum, length and type all validated. Errors are counted, never hidden.
- If the probe keeps announcing itself (it does, every few seconds), the firmware answers only occasionally once data flows.
- The pin is a build option and **must be checked against your wiring**; nothing is probed or driven blindly beyond that one pin.

**Protocol summary** (RAK's SensorHub one-wire protocol, not Modbus): frames are `FF 7E LEN(2) TYPE FLAG | dest src seq hubType payloadLen payloadType | payload | checksum`;
the checksum is a bit-count sum; the master asks the probe to announce itself (*provision*), assigns it an ID, then requests data.
Sensor values use RAK IPSO ids: `0xBA` voltage (0.01 V), `0xB9` current (signed, 0.01 A), `0xB8` state of charge (%),
`0x67` temperature (0.1 C), `0xF3` battery fault bitfield or firmware version. Layout was taken from RAK's published
documentation; see [docs/RAK9154.md](docs/RAK9154.md).

### What changed in the codebase (small footprint)

New files (all compile to nothing unless `RAK9154_BATTERY` is defined):
- `variants/rak4631/rak9154/` - the driver (`RAK9154Protocol`, `RAK9154Battery`, `RAK9154HalfSerial`, `RAK9154Telemetry`)
- `variants/rak4631_rak9154/platformio.ini` - the new build environment
- `test/test_rak9154/` - unit tests; `.github/workflows/build-rak9154.yml` - CI
- `docs/RAK9154.md`, `docs/RAK9154-IMPLEMENTATION-PLAN.md` - details and design notes

Edited upstream files (every change is inside `#ifdef RAK9154_BATTERY`, so other targets compile to identical code):
- `variants/rak4631/RAK4631Board.h` / `.cpp` - cached voltage, `loop()` hook, charger flag, `rak9154` console command
- `src/helpers/sensors/EnvironmentSensorManager.cpp` - a few lines that append the extra telemetry

### Build and install

You need [PlatformIO](https://platformio.org/). Build:

```
pio run -e RAK_4631_repeater_RAK9154
```

Or download a ready build: on GitHub open **Actions -> Build RAK9154 firmware**, pick a green run, and download the
artifact `RAK_4631_repeater_RAK9154-firmware` (`firmware.zip`, `.hex`, `.elf`). Flash it the same way you flash the stock
RAK4631 repeater firmware (for example the MeshCore web flasher with a custom file, or the bootloader's DFU/UF2 method).
Keep the stock `RAK_4631_repeater` build handy to roll back.

**Before connecting the probe:** confirm which pin carries RAK9154 Output 1 on your harness. The default (`P0.15`, Arduino
pin 15) worked on the tested unit but is **not guaranteed** for yours. Check the wiring with a meter, make sure the signal
is 3.3 V, and set `-D RAK9154_ONEWIRE_PIN=<arduino pin>` in `variants/rak4631_rak9154/platformio.ini` if different.
That default pin is also the GPS UART receive pin on the RAK4631, so this target builds with GPS disabled.

### Console commands

Run from the repeater's admin console (the app's *Repeater Admin* screen or serial):

| Command | Shows |
|---|---|
| `rak9154` or `rak9154 status` | state, voltage, current, SOC, temperature, age, counters (`ok` good frames, `to` timeouts, `cs` bad checksums, `inv` invalid frames, `unk` unknown channels, `rng` rejected values) |
| `rak9154 diag` | requests sent, announcements answered/skipped, frames and bytes received, UART errors, back-off state, TX gap |
| `rak9154 info` | probe model, hardware/firmware version, list of sensor ids |
| `rak9154 extra` | the `0xF3` fault / firmware words, once received |
| `rak9154 raw [offset]` | hex of the last received frame (pages of about 40 bytes: `raw`, `raw 35`, `raw 70`) |
| `rak9154 poll` | request a reading now (rate limited) |
| `rak9154 gap [n]` | show / set idle bits between transmitted bytes (0-16, not saved) |

### Settings (build flags)

| Flag | Default | Meaning |
|---|---|---|
| `RAK9154_BATTERY` | off | enables the feature (set by the new environment) |
| `RAK9154_ONEWIRE_PIN` | `PIN_SERIAL1_RX` (P0.15) | Arduino pin of Output 1 - **verify for your wiring** |
| `RAK9154_TX_GAP_BITS` | 4 | idle bit times after each transmitted byte; 4 is what worked on hardware |
| `RAK9154_POLL_INTERVAL_MS` | 30000 | poll period (clamped 15-60 s) |
| `RAK9154_STALE_MS` | 300000 | data older than this is dropped |
| `RAK9154_RESPONSE_TIMEOUT_MS` / `RAK9154_REPROVISION_AFTER` | 4000 / 10 in the new env | reply timeout / timeouts before re-introducing ourselves |
| `RAK9154_RSP_BIG_ENDIAN` / `RAK9154_REPORT_BIG_ENDIAN` | 0 / 1 | byte order of 16-bit values (defaults worked on hardware) |
| `RAK9154_DEBUG` | off | log frames to the USB serial port |

### Troubleshooting with `rak9154 diag`

- `req` rising, `rxfr` not: nothing is coming back. Check the pin, ground and that Output 1 is the connector in use.
- `rxfr` rising but `ok` stays 0 and `prov` equals `req`: the probe talks but ignores our replies. This was the situation
  during bring-up; spacing the transmitted bytes (`rak9154 gap 4`) fixed it. Try values 2-6.
- `cs` or `inv` rising: marginal timing or noise; check wiring length and connections.
- `rng` rising: values decode but look wrong, usually a byte-order flag (`RAK9154_RSP_BIG_ENDIAN`).
- `uart` rising: the receiver saw bad stop bits or an overflow; same causes as `cs`.

### Known limitations

- The telemetry format only carries a positive current value, so discharge shows as 0 A. The probe's current sign is unverified.
- Battery fault flags and the probe's firmware version are decoded but not published over the mesh yet.
- Only the RAK4631 repeater target is provided (not companion or room-server builds).
- Bit-banged serial costs about 1 ms of interrupt time per received byte; this is small at one poll per 30 s.

### Tests and CI

`test/test_rak9154/` holds host-side unit tests (62 protocol, state-machine and telemetry tests, plus 16 tests that run the real
transport source against a simulated clock and pin) built with googletest under AddressSanitizer/UBSan. Their fixtures are
synthetic, so they prove the code is internally consistent, not that a real probe behaves identically. The GitHub Actions
workflow `build-rak9154.yml` runs the tests and builds **both** the stock `RAK_4631_repeater` and the custom target, and
stores the custom firmware as an artifact.

### Keeping this fork up to date with upstream MeshCore

Setup (once). In your local clone of this fork, `origin` is your fork; add upstream:

```
git remote add upstream https://github.com/meshcore-dev/MeshCore.git
git fetch upstream --tags
```

To pick up new MeshCore code (use a release tag such as `repeater-v1.x.y` if you want a released version instead of `main`):

```
git checkout main                       # the branch of this fork that carries the RAK9154 work
git fetch upstream --tags
git log --oneline HEAD..upstream/main   # what is new upstream
git merge-tree --write-tree HEAD upstream/main >/dev/null && echo "merges cleanly" || echo "conflicts expected"   # optional dry run
git merge upstream/main                 # or: git merge <release-tag>
```

We recommend **merge** over rebase: the branch is public, merge keeps history intact, and re-running it later is safe.
(Rebase gives a tidier history but rewrites published commits; only use it on a branch nobody else has pulled.)
If you work on a feature branch (for example `feature/rak9154-driver`), merge upstream into `main` first, then merge `main` into your branch.

If there are conflicts, they are almost certainly in one of the few upstream files we edit. Resolve them by keeping **both**
sides: the upstream change, and our lines inside `#ifdef RAK9154_BATTERY` blocks.

| File | What we added there |
|---|---|
| `variants/rak4631/RAK4631Board.h` | includes, `getBattMilliVolts()` returning the cached value, `loop()`, `isChargerActive()`, `handleCommand()`, members |
| `variants/rak4631/RAK4631Board.cpp` | `begin()` calls `rak9154.begin()`, the `rak9154` console command, the telemetry hook function |
| `src/helpers/sensors/EnvironmentSensorManager.cpp` | one declaration and one `#ifdef RAK9154_BATTERY` block in `querySensors()` |
| `README.md` | this fork section (inserted before "Hardware Compatibility") and the notice at the top; keep both upstream's text and ours |

After merging, always check (CI does the first two):
1. `pio run -e RAK_4631_repeater` and `pio run -e RAK_4631_repeater_RAK9154` both build.
2. The host tests pass (see `.github/workflows/build-rak9154.yml` for the exact commands).
3. Quickly confirm the interfaces we depend on still exist upstream: `mesh::MainBoard::getBattMilliVolts()`, `loop()`,
   `isChargerActive()`, `handleCommand()`; `SensorManager::querySensors()`; `NRF52Board::begin()`; the `variants/*/platformio.ini` single-level glob.
   A quick check: `git diff HEAD~1 -- src/MeshCore.h src/helpers/SensorManager.h src/helpers/NRF52Board.h`.
4. Flash a test node, run `rak9154 status` and confirm voltage and `ok` frames before rolling out.

Our new files live in their own directories (`variants/rak4631/rak9154/`, `variants/rak4631_rak9154/`, `test/test_rak9154/`) so they
never conflict with upstream. This procedure has been written down but **not yet exercised against new upstream commits**
(upstream had not moved since our base commit when this was written); treat the first real merge as a test of these instructions.

If you improve this, please consider offering the RAK9154 support back upstream as a pull request to
[meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore); the guarded, self-contained layout is meant to make that easy.

#### Automatic sync and firmware releases (optional)

The workflow `.github/workflows/sync-upstream.yml` does the steps above for you every Monday (and whenever you run it by
hand from the **Actions** tab):

1. Fetches upstream and finds the newest `repeater-v*` release tag. If this fork already contains it, it stops.
2. Merges that tag into the default branch. **Upstream's workflow changes are never taken** (so upstream's own release
   workflows do not start running on your fork); our workflows always win.
3. Runs the host tests and builds both `RAK_4631_repeater` and `RAK_4631_repeater_RAK9154`.
4. **Only if everything passes**, pushes the merge and publishes a **GitHub Release** (shown on your fork's *Releases* page)
   named `rak9154-<upstream tag>` with the RAK9154 `firmware.zip`/`.hex` and `SHA256SUMS.txt` attached.
5. On a merge conflict or a failed test/build it pushes nothing and opens an **issue** with the details.

You can choose a different upstream ref when running it manually (for example `repeater-v1.19.0` or `upstream/main`), and untick
*publish* to do a dry run that only builds. Tick *force_release* to build and publish the current code even when there is no new
upstream release (use it for the very first release).

One-time setup on your fork (GitHub does not do these automatically):
- The workflow file must be on the fork's **default branch** (scheduled workflows only run from there). If the RAK9154 work is
  still on `feature/rak9154-driver`, merge it into `main` first.
- Enable Actions on the fork (**Actions** tab -> enable). Scheduled runs are off by default on forks and are paused by GitHub
  after 60 days without repository activity; a merged release or any commit keeps them alive.
- Nothing else: the repo-wide token default can stay read-only (the safer setting); the workflow asks for the write access it needs itself. Also turn on **Issues** (Settings -> General -> Features), because failures are reported as issues.

Limits to know about: releases are **built and unit-tested but not tested on hardware**; every release page says so, and you should flash
a test node and check `rak9154 status` before rolling out. A release only appears when upstream publishes a new `repeater-v*` tag, so
unreleased upstream commits are not built unless you run it manually with `upstream/main`. The merge logic itself is covered by a
local test, `.github/scripts/test-sync-upstream.sh`, which runs it against a throw-away fake upstream (up to date, clean merge,
conflict, version ordering, protected workflows); the full workflow including the GitHub-side steps (push, release, issues)
has **not been run yet**, so check the first run's log.

### Credits and licensing

The driver is original code under the MIT license, consistent with MeshCore. The protocol was worked out from RAK's published
MIT-licensed [RAK-OneWireSerial](https://github.com/beegee-tokyo/RAK-OneWireSerial) library and RAK's public payload-decoder
documentation; no code was copied from it, from RAK's LGPL half-duplex serial class, or from Meshtastic (GPL). Thanks to RAK Wireless
for documenting the SensorHub interface. More detail: [docs/RAK9154.md](docs/RAK9154.md) and
[docs/RAK9154-IMPLEMENTATION-PLAN.md](docs/RAK9154-IMPLEMENTATION-PLAN.md).

## 🛠 Hardware Compatibility

MeshCore is designed for devices listed in the [MeshCore Flasher](https://meshcore.io/flasher)

## 📜 License

MeshCore is open-source software released under the MIT License. You are free to use, modify, and distribute it for personal and commercial projects.

## Contributing

Please submit PR's using 'dev' as the base branch!
For minor changes just submit your PR and we'll try to review it, but for anything more 'impactful' please open an Issue first and start a discussion. It is better to sound out what it is you want to achieve first, and try to come to a consensus on what the best approach is, especially when it impacts the structure or architecture of this codebase.

Here are some general principles you should try to adhere to:
* Keep it simple. Please, don't think like a high-level lang programmer. Think embedded, and keep code concise, without any unnecessary layers.
* No dynamic memory allocation, except during setup/begin functions.
* Use the same brace and indenting style that's in the core source modules. (A .clang-format is probably going to be added soon, but please do NOT retroactively re-format existing code. This just creates unnecessary diffs that make finding problems harder)

Help us prioritize! Please react with thumbs-up to issues/PRs you care about most. We look at reaction counts when planning work.

### Running unit tests

To run unit tests, run the following command:

```bash
pio test --environment native --verbose
```

## Road-Map / To-Do

There are a number of fairly major features in the pipeline, with no particular time-frames attached yet. In very rough chronological order:
- [X] Companion radio: UI redesign
- [X] Repeater + Room Server: add ACL's (like Sensor Node has)
- [X] Standardise Bridge mode for repeaters
- [ ] Repeater/Bridge: Standardise the Transport Codes for zoning/filtering
- [X] Core + Repeater: enhanced zero-hop neighbour discovery
- [ ] Core: round-trip manual path support
- [ ] Companion + Apps: support for multiple sub-meshes (and 'off-grid' client repeat mode)
- [ ] Core + Apps: support for LZW message compression
- [ ] Core: dynamic CR (Coding Rate) for weak vs strong hops
- [ ] Core: new framework for hosting multiple virtual nodes on one physical device
- [ ] V2 protocol spec: discussion and consensus around V2 packet protocol, including path hashes, new encryption specs, etc

## 📞 Get Support

- Report bugs and request features on the [GitHub Issues](https://github.com/meshcore-dev/meshcore/issues) page.
- Find additional guides and components on [my site](https://buymeacoffee.com/ripplebiz).
- Join [MeshCore Discord](https://meshcore.gg) to chat with the developers and get help from the community.
