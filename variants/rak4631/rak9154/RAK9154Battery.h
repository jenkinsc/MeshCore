// SPDX-License-Identifier: MIT
//
// Cached RAK9154 battery telemetry service.
//
//   loop()  --every main-loop iteration-->  drain RX (non-blocking) -> validate -> cache
//                                           every poll interval: transmit ONE short request frame
//   getters --any time-->                   read the cache; never touch the wire
//
// The mesh code only ever calls the getters (through RAK4631Board::getBattMilliVolts()), so radio processing
// never waits for the battery. The only blocking operation in this module is the bit-banged transmit of a
// request frame (13 bytes ~= 14 ms at 9600 baud, once per poll interval; the one-time provisioning reply is
// ~80 bytes ~= 85 ms). Receiving is interrupt fed and consumed without waiting.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "RAK9154Protocol.h"

#ifndef RAK9154_POLL_INTERVAL_MS
#define RAK9154_POLL_INTERVAL_MS 30000UL      // default poll period; clamped to 15 s .. 60 s
#endif
#ifndef RAK9154_STALE_MS
#define RAK9154_STALE_MS 300000UL             // telemetry older than this is reported unavailable
#endif
#ifndef RAK9154_RSP_BIG_ENDIAN
#define RAK9154_RSP_BIG_ENDIAN 0              // UNVERIFIED byte order of 16-bit values in get-data responses
#endif
#ifndef RAK9154_REPORT_BIG_ENDIAN
#define RAK9154_REPORT_BIG_ENDIAN 1           // UNVERIFIED byte order of 16-bit values in unsolicited reports
#endif
#ifndef RAK9154_RESPONSE_TIMEOUT_MS
#define RAK9154_RESPONSE_TIMEOUT_MS 1500      // how long a request may stay unanswered before it counts as a timeout
#endif
#ifndef RAK9154_REPROVISION_AFTER
#define RAK9154_REPROVISION_AFTER 3           // consecutive timeouts before starting over with a provision BOOT
#endif
#ifndef RAK9154_RAW_CAPTURE
#define RAK9154_RAW_CAPTURE 1                 // keep the last received candidate frame for "rak9154 raw"
#endif

#ifdef RAK9154_DEBUG
  #if defined(ARDUINO)
    #include <Arduino.h>
    #define RAK9154_LOG(fmt, ...) Serial.printf("RAK9154 " fmt "\n", ##__VA_ARGS__)
  #else
    #include <stdio.h>
    #define RAK9154_LOG(fmt, ...) printf("RAK9154 " fmt "\n", ##__VA_ARGS__)
  #endif
#else
  #define RAK9154_LOG(...) do {} while (0)
#endif

namespace rak9154 {

// Single-wire half-duplex byte transport. Implemented for nRF52 in RAK9154HalfSerial; faked in host tests.
class Transport {
public:
  virtual ~Transport() {}
  virtual bool   begin() = 0;                                  // claim the pin, start receiving
  virtual void   write(const uint8_t* data, size_t len) = 0;   // blocking, bounded; line is released afterwards
  virtual int    read() = 0;                                   // next received byte or -1; never blocks
  virtual void   service() {}                                  // periodic housekeeping (e.g. re-arm after an RX storm)
  virtual uint32_t rxErrors() const { return 0; }              // low-level receive faults (bad stop bit, overflow, storm)
};

struct Config {
  uint32_t pollIntervalMs     = RAK9154_POLL_INTERVAL_MS;
  uint32_t staleMs            = RAK9154_STALE_MS;
  uint32_t responseTimeoutMs  = RAK9154_RESPONSE_TIMEOUT_MS;
  uint32_t firstPollDelayMs   = 3000;      // give the probe time to boot after we start
  uint8_t  reprovisionAfter   = RAK9154_REPROVISION_AFTER;
  DecodeOptions decode;
  Config() {
    decode.responseBigEndian = RAK9154_RSP_BIG_ENDIAN != 0;
    decode.reportBigEndian   = RAK9154_REPORT_BIG_ENDIAN != 0;
  }
};

struct Stats {
  uint32_t requestsSent     = 0;   // get-data + provision BOOT frames transmitted
  uint32_t okFrames         = 0;   // validated frames that updated the cache ("successful polls" incl. reports)
  uint32_t timeouts         = 0;   // requests that got no answer in time
  uint32_t badChecksums     = 0;
  uint32_t invalidFrames    = 0;   // wrong type/length/truncated payload, malformed entries
  uint32_t framingErrors    = 0;   // implausible LEN / stalled frames dropped by the assembler
  uint32_t unknownChannels  = 0;   // IPSO ids this driver cannot decode
  uint32_t rangeRejects     = 0;   // decoded but implausible values (often a wrong byte order)
  uint32_t provisions       = 0;   // probe announcements answered
  uint32_t otherFrames      = 0;   // valid frames of a kind we do not use
  uint32_t rxFrames         = 0;   // candidate frames delivered by the assembler (valid or not)
  uint32_t rxBytes          = 0;   // raw bytes read from the transport
  uint32_t maxTxMs          = 0;   // longest transmit, i.e. longest time loop() was blocked by this driver
};

// Snapshot, shaped like the structure proposed in the task description. `status1/2` are intentionally absent:
// no verified IPSO id for RAK9154 status/fault bits was found in any source (see plan, section 8).
struct RAK9154Telemetry {
  bool     valid;               // voltage present and fresh
  uint32_t voltage_mV;
  bool     hasCurrent;  int32_t current_mA;
  bool     hasSoc;      uint8_t stateOfCharge;
  bool     hasTemp;     float   temperatureC;
  uint32_t lastUpdateMs;        // millis() of the last cache update
};

}  // namespace rak9154

class RAK9154Battery {
public:
  typedef uint32_t (*ClockFn)();

  RAK9154Battery(rak9154::Transport& transport, ClockFn clock, const rak9154::Config& cfg = rak9154::Config());

  // Start the transport. Safe to call once, early in setup. Returns false if the pin/interrupt could not be claimed;
  // the driver then stays inert and the board falls back to its normal battery method.
  bool begin();

  // Call every main-loop iteration. Never waits for the probe.
  void loop();

  // ---- cached accessors: O(1), no I/O -------------------------------------------------------------
  bool     hasValidTelemetry() const;           // voltage known and younger than the stale threshold
  uint32_t getVoltageMilliVolts() const;        // 0 when !hasValidTelemetry()
  bool     hasCurrent() const;
  int32_t  getCurrentMilliAmps() const;         // sign convention UNVERIFIED (Meshtastic: positive = charging)
  bool     hasStateOfCharge() const;
  uint8_t  getStateOfCharge() const;
  bool     hasTemperature() const;
  float    getTemperatureC() const;
  uint32_t getLastUpdateMillis() const { return _voltageAt; }
  bool     isCharging() const;                  // fresh current above a small threshold

  rak9154::RAK9154Telemetry snapshot() const;

  // ---- control / diagnostics ----------------------------------------------------------------------
  void requestPollNow();                        // next loop() sends a request (rate limited to 1/s)
  void setPollIntervalMs(uint32_t ms);          // clamped to 15 s .. 60 s
  uint32_t getPollIntervalMs() const { return _cfg.pollIntervalMs; }
  bool     detected() const { return _everGood; }
  const rak9154::Stats& stats() const { return _stats; }
  const rak9154::ProvisionInfo* provisionInfo() const { return _haveProv ? &_prov : nullptr; }

  // Text for the CLI. All write at most `cap` bytes including the terminator and return the length.
  size_t formatStatus(char* out, size_t cap) const;
  size_t formatInfo(char* out, size_t cap) const;
  size_t formatDiag(char* out, size_t cap) const;
  size_t formatRaw(char* out, size_t cap, size_t offset = 0) const;   // hex of the last received frame from `offset`

private:
  enum State : uint8_t { ST_PROVISIONING, ST_POLLING };

  void drainRx(uint32_t now);
  void handleCandidate(const uint8_t* data, size_t len, uint32_t now);
  void applyReading(const rak9154::Frame& f, const rak9154::Reading& r, uint32_t now);
  void sendRequest(uint32_t now);
  void transmit(const uint8_t* buf, size_t len);
  static bool fresh(uint32_t now, uint32_t at, uint32_t windowMs) { return (uint32_t)(now - at) <= windowMs; }

  rak9154::Transport& _t;
  ClockFn             _clock;
  rak9154::Config     _cfg;
  rak9154::Stats      _stats;
  rak9154::FrameAssembler _asm;

  bool     _begun = false;
  bool     _transportOk = false;
  State    _state = ST_PROVISIONING;
  uint8_t  _pid = rak9154::PID_ASSIGNED;
  uint8_t  _seq = 0;
  bool     _awaiting = false;
  uint32_t _sentAt = 0;
  uint32_t _nextPollAt = 0;
  uint32_t _lastForced = 0;
  bool     _haveForced = false;
  uint8_t  _consecFail = 0;
  bool     _everGood = false;

  bool     _haveVolt = false;     uint32_t _voltage_mV = 0;     uint32_t _voltageAt = 0;
  bool     _haveCur  = false;     int32_t  _current_mA = 0;     uint32_t _currentAt = 0;
  bool     _haveSoc  = false;     uint8_t  _soc = 0;            uint32_t _socAt = 0;
  bool     _haveTemp = false;     int16_t  _temp_dC = 0;        uint32_t _tempAt = 0;

  bool     _haveProv = false;
  rak9154::ProvisionInfo _prov;

#if RAK9154_RAW_CAPTURE
  uint8_t  _raw[rak9154::MAX_FRAME];
  size_t   _rawLen = 0;
  uint32_t _rawAt = 0;
#endif
};
