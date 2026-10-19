# RAK9154 Battery Telemetry — Implementation Plan

Status: research complete, Phase 1 implementation in progress.
Everything below was read from source on 2026-10-18; items that could **not** be verified are marked **UNVERIFIED**.

## 1. Upstream MeshCore commit

- Repo: `https://github.com/meshcore-dev/MeshCore`, branch `main`
- Commit used: `81dbae592ffad40b4212bc0b37e82b38da08f4bc` ("Merge branch 'dev'", 2026-10-15)
- License: MIT

Baseline build (`RAK_4631_repeater`): **not completed in the authoring environment.** PlatformIO's registry hosts
(`api.registry.platformio.org`) were blocked by the sandbox egress policy, so the `nordicnrf52` platform and toolchain could not be
installed. The baseline must be confirmed by CI / a developer machine (see section 11). Also note `sh build.sh` fails on
Debian/Ubuntu because `/bin/sh` is dash and `build.sh` uses bash arrays; use `bash build.sh ...` there (macOS `sh` is bash and works).

## 2. RAK4631 board class and relevant files

| Purpose | Path |
|---|---|
| Board class `RAK4631Board : NRF52BoardDCDC` | `variants/rak4631/RAK4631Board.h`, `RAK4631Board.cpp` |
| Globals (`board`, `radio_driver`, `sensors`, `rtc_clock`) | `variants/rak4631/target.h`, `target.cpp` |
| Pin map (`PIN_SERIAL1_RX=15`, `PIN_SERIAL1_TX=16`, `WB_IO*`) | `variants/rak4631/variant.h` |
| PlatformIO section `[rak4631]` and envs | `variants/rak4631/platformio.ini` |
| nRF52 base (`[nrf52_base]`) | root `platformio.ini` |
| Repeater firmware | `examples/simple_repeater/{main.cpp,MyMesh.cpp,MyMesh.h}` |

## 3. Current `getBattMilliVolts()`

`variants/rak4631/RAK4631Board.h`:

```cpp
#define PIN_VBAT_READ    5
#define ADC_MULTIPLIER   (3 * 1.73 * 1.187 * 1000)
#define BATTERY_SAMPLES 8
uint16_t getBattMilliVolts() override {
  analogReadResolution(12);
  uint32_t raw = 0;
  for (int i = 0; i < BATTERY_SAMPLES; i++) raw += analogRead(PIN_VBAT_READ);
  raw = raw / BATTERY_SAMPLES;
  return (ADC_MULTIPLIER * raw) / 4096;
}
```

Interface: `mesh::MainBoard::getBattMilliVolts()` is pure virtual, returns `uint16_t` (`src/MeshCore.h:47`).
This is the **only** place that needs to change for Phase 1.

`NRF52_POWER_MANAGEMENT` (boot-voltage lockout, calls `getBattMilliVolts()` in `NRF52Board.cpp`) is **not** defined for the RAK4631 envs
in this commit, so the cached getter returning "not available" at boot cannot trigger a boot lockout. Re-check if that ever changes.

## 4. Telemetry call path

- Remote telemetry request → `examples/simple_repeater/MyMesh.cpp:243-259`:
  `telemetry.reset(); telemetry.addVoltage(TELEM_CHANNEL_SELF, board.getBattMilliVolts()/1000.0f); sensors.querySensors(perm_mask, telemetry);`
- Repeater stats → `MyMesh.cpp:218` (`stats.batt_milli_volts`), also `StatsFormatHelper.h:14`.
- Local/remote CLI `power` → `src/helpers/CommonCLI.cpp:295`.
- **Cayenne LPP is used** (`electroniccats/CayenneLPP @ 1.6.1`). Voltage on `TELEM_CHANNEL_SELF` (=1). `EnvironmentSensorManager::querySensors()`
  appends other sensors on channels 2+.
- No protocol change is needed for Phase 1: the voltage rides the existing field.

## 5. Architecture facts that constrain the design

- Main loop (`examples/simple_repeater/main.cpp`): cooperative, single-threaded. Per iteration: serial CLI, `board.loop()`, `the_mesh.loop()`, `sensors.loop()`, `rtc_clock.tick()`.
  `MainBoard::loop()` (`MeshCore.h`) is an existing, empty-by-default per-iteration hook → the natural place to run the RAK9154 service. **No new scheduling mechanism is needed.**
- CLI extension: `MainBoard::handleCommand(const char* command, uint32_t sender_timestamp, char* reply)` is already consulted first by
  `CommonCLI::handleCommand` (`CommonCLI.cpp:189`). A `rak9154` command can be added from the board class with no change to shared CLI code.
  Reply buffer is 160 bytes.
- Hardware abstraction: `mesh::MainBoard` virtuals (`getBattMilliVolts`, `isChargerActive`, `handleCommand`, `loop`). `isChargerActive()` is reported by the `power` CLI command.
- Timing: the radio is driven from `the_mesh.loop()`; anything slow in `loop()`/`getBattMilliVolts()` delays packet handling. The `getBattMilliVolts()` path must stay a pure cache read.
- Build: `build.sh` wraps `pio run -e <env>`; `platformio.ini` loads `variants/*/platformio.ini` (single directory level).
- BSP / deps (pinned in root `platformio.ini`): `platform = nordicnrf52`; `framework-arduinoadafruitnrf52` from the MeshCore fork at commit
  `d541301665b40959682252911e57b11df3ee651a`; `platformio/toolchain-gccarmnoneeabi@^1.140201.0`; CayenneLPP 1.6.1; RadioLib pinned to a commit.
  `-w` (warnings off) is in the base flags.

## 6. RAK9154 — Meshtastic implementation findings

Files (Meshtastic `firmware` @ `8ebb6988b56e8da82bb56a3f55bf7908e9e5c465`): `src/modules/Telemetry/Sensor/RAK9154Sensor.{h,cpp}`, enabled by `-DHAS_RAKPROT=1`.

- **Pin:** `HALF_UART_PIN`, defined in `variants/nrf52840/rak2560/variant.h` as `PIN_SERIAL1_RX` (code comment: "P0.15"). That is the **RAK2560** variant, not a RAK4631 repeater harness.
- **Serial:** RAK's `SoftwareHalfSerial` at 9600 baud, one pin for TX and RX.
- **It delegates framing/checksum entirely to RAK-OneWireSerial** (`RakSNHub_Protocl_API`); `RAK9154Sensor.cpp` itself only handles events.
- **Sequence:** `init()` → master broadcasts a *provision BOOT* request → probe answers with a *provision* request → master replies assigning PID (`ADD_PID` event) → master issues one *get sensor data* request → afterwards the probe sends periodic *report* frames. A 50 ms periodic task drains the RX buffer (2 ms inter-byte timeout) and feeds `process()`.
- **Values used (only three):**

| IPSO id | Meaning | Request-response path (`SDATA_REQ`) | Periodic report path (`REPORT`) |
|---|---|---|---|
| `0xB8` (3384-3200) | capacity, 1 byte, % (clamped to 100) | `msg[1]` | `msg[1]` |
| `0xB9` (3385-3200) | DC current, 2 bytes, signed, mA | **little-endian** `(msg[2]<<8)+msg[1]` | **big-endian** `(msg[1]<<8)+msg[2]` |
| `0xBA` (3386-3200) | DC voltage, 2 bytes, **×10 → mV** | **little-endian** | **big-endian** |

  **Quirk:** the two Meshtastic code paths decode multi-byte values with *opposite* byte order. Whether this reflects real device behaviour or a bug is **UNVERIFIED**; the driver must treat it as an open hardware question (see section 13) and log raw bytes in `RAK9154_DEBUG` builds.
- **Charging:** `current > 0` is treated as charging (positive = into battery).
- **Error handling:** checksum and sequence errors are logged only.
- **License: GPL-3.0** (Meshtastic `LICENSE`).

## 7. RAK-OneWireSerial findings

Repo `beegee-tokyo/RAK-OneWireSerial` (cloned, current default branch). Files: `src/RAK-OneWireSerial.h`, `SoftwareHalfSerial.{h,cpp}`, `onewire_master_protocol.{c,h}`, `onewire_master_api.h`.

- README: RAK2560 SensorHub probes and IO probes, and **RAK9154 Solar Battery Lite**; "designed only for the WisBlock RAK4631"; preferred pin RX1 = P0.15, other GPIOs "should work but are untested"; library marked **BETA**.
- `library.json`: **MIT**. But `SoftwareHalfSerial.{h,cpp}` carry an **LGPL-2.1** header (adapted from Arduino SoftwareSerial).
- Not consumable as a clean dependency here: it is built around global static state (`record[1]`, `menu`, `dataBuff`), has heavy `packed` structs with `enum` members (size depends on `-fshort-enums`), pulls `Adafruit_TinyUSB.h`, bit-bangs RX inside a GPIO ISR, and has no bounds-checking on several paths (`(U8)len` casts, unchecked `rakipso_tbl[ipso]` index, unchecked payload offsets). Only a few hundred lines of protocol are actually needed, so a **minimal, bounds-checked, independent implementation is preferred**.

## 8. Exact protocol / frame information

Derived from `onewire_master_protocol.c`; all multi-byte header fields below were checked by tracing the code.

Serial: 9600 baud, 8N1, idle high, single wire (half duplex).

```
byte  0     0xFF   wakeup (ignored by receiver; library prepends it)
byte  1     0x7E   delimiter / start
byte  2..3  LEN    big-endian u16 = 6 (hub header) + hub payload_length
byte  4     TYPE   0=ECHO 1=ATCMD 2=SENSORHUB   (only 2 is accepted)
byte  5     FLAG   0=REQ 1=RSP
--- SensorHub header (6 bytes, counted in LEN) ---
byte  6     dest PID   (0x00 master, 0xFF unknown/broadcast)
byte  7     source PID
byte  8     sequence
byte  9     hub type: 0 wakeup,1 provision,2 paramset,3 senddata,4 control,5 paramget,6 alert,7 err,8 ack,9 nack,...
byte 10     payload_length
byte 11     payload type
byte 12..   payload (payload_length bytes)
last        CHECKSUM
```

- **Checksum** (`cal_chksum`): `popcount(TYPE) + popcount(FLAG) + Σ popcount(b)` over the `LEN` bytes starting at byte 6, 8-bit wrap. It does **not** cover wakeup, delimiter, or LEN. It sits at byte `6+LEN`. (Receiver also tolerates `LEN == 6+payload_length-1`, an old firmware quirk.)
- **Master → probe, provision BOOT:** `FF 7E 00 06 02 00 FF 00 <seq> 01 00 02 <chk>` (13 bytes).
- **Master → probe, get data:** `FF 7E 00 06 02 00 <pid> 00 <seq> 03 00 02 <chk>` (13 bytes). The reference library writes the checksum over the (unused) sid byte; the wire frame is the 13 bytes above.
- **Probe → master provision (hub type 1, ptype 3 "VER3"), payload:** `hw(1) sw(3) sn(18) provId(1) reserved(7) model(20) reserved(4) snsr_num(1) {sid(1) ipso(1) rule(2)}×snsr_num`. Master must answer with the same frame, flag=RSP, dest/source swapped, `provId` = assigned PID (reference assigns 1), checksum recomputed.
- **Sensor data payload (hub type 3):** repeated `{sid(1), ipso(1), value(size[ipso])}`; sizes for our fields: `0xB8`:1, `0xB9`:2, `0xBA`:2. Flag=REQ is an unsolicited report; flag=RSP answers our get-data.
- **Candidate IDs from the task (0x15–0x1A):** **not found in any source read.** The real IDs are the IPSO ones above. Temperature (`RAK_IPSO_TEMP_SENSOR` = 3303-3200 = `0x67`, 2 bytes) exists in the table, but **no evidence that the RAK9154 reports it**; status flags: **no evidence at all**. Phase 2 for those fields is therefore gated on seeing a real device's provision sensor list.

## 9. Licensing assessment

| Source | License | Use |
|---|---|---|
| MeshCore | MIT | target license for all new code |
| Meshtastic `RAK9154Sensor.*` | **GPL-3.0** | **not copied.** Read only to learn observable behaviour (IDs, scaling, sequence). Facts about a wire protocol are not copyrightable expression. |
| RAK-OneWireSerial protocol (`onewire_master_*`) | MIT | read for format; new code is independent; attribution added in docs |
| RAK-OneWireSerial `SoftwareHalfSerial.*` | **LGPL-2.1** | **not copied or linked.** Own half-duplex transport written from scratch using the generic bit-bang technique |

This assessment is an engineering judgement, not legal advice.

## 10. GPIO / interface mapping (**UNVERIFIED — configurable**)

Evidence: RAK-OneWireSerial README (preferred RX1 = P0.15) and Meshtastic RAK2560 (`HALF_UART_PIN = PIN_SERIAL1_RX`) both point at **P0.15 = Arduino pin 15**, which is `PIN_SERIAL1_RX` in `variants/rak4631/variant.h`.
I did **not** find a public document showing (a) the RAK9154 Output 1 connector pinout, (b) the WisMesh Repeater harness pinout, or (c) which RAK19007/RAK19026 base-board net reaches P0.15, nor signal levels or any level-shift/protection circuit. These must be checked against RAK's datasheets and a multimeter/scope before connecting.

Plan: `RAK9154_ONEWIRE_PIN` defaults to `PIN_SERIAL1_RX` (15) with a loud comment; override with `-D RAK9154_ONEWIRE_PIN=<n>`.
Conflict: the RAK4631 env maps the GPS UART to the same pins (`PIN_GPS_TX=PIN_SERIAL1_RX`). The new target therefore adds `-UENV_INCLUDE_GPS` (same trade-off Meshtastic makes for this feature). Documented in `docs/RAK9154.md`.

## 11. Proposed source files and PlatformIO changes

New (no upstream file edited except the board class):

```
variants/rak4631/rak9154/RAK9154Protocol.{h,cpp}    pure C++, no Arduino: frame build/parse/checksum, decode to struct
variants/rak4631/rak9154/RAK9154Battery.{h,cpp}     state machine + cached telemetry + counters (transport injected)
variants/rak4631/rak9154/RAK9154HalfSerial.{h,cpp}  nRF52 single-wire bit-bang transport
variants/rak4631_rak9154/platformio.ini             new env (separate dir so no upstream .ini line changes)
test/test_rak9154/                                  native googletest parser/state-machine tests + fixtures
docs/RAK9154.md, docs/RAK9154-IMPLEMENTATION-PLAN.md
.github/workflows/build-rak9154.yml
```

Why a second `variants/` directory: `extra_configs = variants/*/platformio.ini` only matches one directory level, so a sibling directory holding just the new `[env:...]` means upstream's `variants/rak4631/platformio.ini` is never edited and cannot conflict on merge.

Edited upstream files (guarded by `#ifdef RAK9154_BATTERY`, small):
- `variants/rak4631/RAK4631Board.h/.cpp`: include, one member, `getBattMilliVolts()` prefers the cached value, `loop()` override, `handleCommand()`/`isChargerActive()` overrides.
- Root `platformio.ini`: add a native test env entry for the new tests (only needed if the existing `[env:native]` filter cannot be reused; otherwise untouched).

Env: `[env:RAK_4631_repeater_RAK9154]` extends `env:RAK_4631_repeater`, appends `-D RAK9154_BATTERY`, `-UENV_INCLUDE_GPS`, and an `ADVERT_NAME`. All `rak9154/*.cpp` are wrapped in `#if defined(RAK9154_BATTERY) || defined(RAK9154_HOST_TEST)` so the stock target compiles them to nothing (stock image should be byte-identical; compare sizes).

## 12. Polling / state-machine design

All time is passed in (`loop(nowMs)`), transport is an interface → host-testable.

States: `INIT → PROVISIONING → POLLING` (any state → `PROVISIONING` after repeated timeouts).
- `PROVISIONING`: send provision BOOT broadcast, wait ≤ 250 ms (bounded) for the probe's provision request; reply with RSP assigning PID 1. Retry every poll period.
- `POLLING`: every `RAK9154_POLL_INTERVAL_MS` (default 30 000, clamped 15 000–60 000) send get-data to the PID and wait ≤ 250 ms. Unsolicited reports are accepted at any time.
- RX is interrupt-fed into a 64-byte ring buffer; `loop()` drains it non-blockingly (never waits for bytes), assembles frames with a length-prefixed state machine (max frame 64 bytes, bad length → resync on next `0x7E`), then validates: delimiter, TYPE=2, LEN consistency, checksum, hub type, bounded payload walk, known IPSO sizes.
- Cache updated only from fully validated frames, per-field. `hasValidTelemetry()` = have voltage **and** `now - lastValid < 5 min` (configurable). Getter is a few loads; never touches the wire.
- Counters: ok polls, timeouts, bad checksums, invalid frames, unknown IPSO ids, provisions, last response age. Exposed via `rak9154 status`.
- TX is the only blocking part: ≤ 13 bytes ≈ 14 ms with that one pin's interrupt masked, once per poll. This is a bounded exception to "non-blocking"; the alternative (timer/PPI-driven TX) is a candidate follow-up if bench tests show jitter.

## 13. Risks and unresolved hardware questions

1. **GPIO / Output 1 pinout / level shifting: UNVERIFIED** (section 10). Wrong pin = no data; wrong wiring could damage the GPIO.
2. **Byte order of 16-bit values** differs between the two Meshtastic code paths. First bench step: capture raw frames (`RAK9154_DEBUG`) and compare with the multimeter voltage; decode is isolated in one function.
3. **Does the RAK9154 need the provisioning handshake, or only emit periodic reports?** Driver supports both, but the real behaviour is untested.
4. **Temperature / status fields:** no verified IPSO ids; Phase 2 depends on the provision sensor list from real hardware.
5. **Bit-banged RX timing** can be corrupted by higher-priority interrupts (SoftDevice/USB); checksum rejects corrupted frames, and a missed frame only delays an update.
6. **GPS and the one-wire pin share P0.15 by default**; GPS is disabled in the new target.
7. **PlatformIO builds not run in the authoring sandbox** (registry blocked) — Phase 1 acceptance criteria 1–3 and all on-hardware criteria (4–10) are open until CI/bench confirm them.
8. `sh build.sh` does not work on dash-based `/bin/sh`; use `bash build.sh`.
