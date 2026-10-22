// SPDX-License-Identifier: MIT
//
// RAK9154 SensorHub ("one-wire") protocol: frame building, parsing and decoding.
//
// This file is hardware independent (no Arduino headers) so it can be unit tested on a host.
// See docs/RAK9154-IMPLEMENTATION-PLAN.md section 8 for how the wire format was determined.
//
// Wire format reference (verified by reading, NOT yet by bench test against real hardware):
//   RAK-OneWireSerial, onewire_master_protocol.c / .h  (MIT, https://github.com/beegee-tokyo/RAK-OneWireSerial)
// Value scaling / byte order reference (behavioural, GPL code not copied):
//   Meshtastic RAK9154Sensor.cpp
//
//   FF 7E LL LL TT FF | DD SS QQ HT PL PT | payload (PL bytes) | CK
//   |  |  |      |  |    '-------------- SensorHub header, 6 bytes, counted in LEN -------------'
//   |  |  |      |  '- FLAG: 0 request, 1 response
//   |  |  |      '---- TYPE: 2 = SensorHub (only type accepted)
//   |  |  '----------- LEN: big-endian, = 6 + PL (or 6 + PL - 1 from an old probe firmware, see RAK lib)
//   |  '-------------- delimiter
//   '----------------- wakeup byte (sent by master, ignored on receive)
//   CK = popcount(TYPE) + popcount(FLAG) + sum(popcount(byte)) over the LEN bytes of header+payload, mod 256
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rak9154 {

// ---- constants -------------------------------------------------------------------------------

constexpr uint8_t  WAKEUP_BYTE   = 0xFF;
constexpr uint8_t  DELIMITER     = 0x7E;
constexpr uint8_t  PID_MASTER    = 0x00;
constexpr uint8_t  PID_UNKNOWN   = 0xFF;   // broadcast / not yet provisioned
constexpr uint8_t  PID_ASSIGNED  = 0x01;   // PID we hand out to the probe (the RAK lib assigns array index + 1 = 1)

constexpr uint8_t  TYPE_SENSORHUB = 2;
constexpr uint8_t  FLAG_REQ = 0;
constexpr uint8_t  FLAG_RSP = 1;

constexpr uint8_t  HUB_PROVISION = 1;
constexpr uint8_t  HUB_SENDDATA  = 3;

constexpr uint8_t  PROV_BOOT = 2;          // master -> probe "please announce yourself"
constexpr uint8_t  PROV_VER3 = 3;          // probe -> master announcement
constexpr uint8_t  SDATA_SENDAT = 2;       // get-data payload type

constexpr size_t   HUB_HEADER_SIZE = 6;    // dest, source, sequence, hubType, payloadLen, payloadType
constexpr size_t   MAX_PAYLOAD     = 120;
constexpr size_t   MAX_FRAME       = 1 /*delim*/ + 2 /*len*/ + 2 /*type,flag*/ + HUB_HEADER_SIZE + MAX_PAYLOAD + 1 /*chk*/;
constexpr size_t   MAX_TX_FRAME    = 1 /*wakeup*/ + MAX_FRAME;

// IPSO channel ids as carried inside sensor data payloads (RAK lib: id = IPSO object id - 3200).
constexpr uint8_t  IPSO_TEMPERATURE = 0x67;   // 3303. Present in RAK table; NOT confirmed to be reported by RAK9154.
constexpr uint8_t  IPSO_BATT_STATUS = 0xF3;   // 2 bytes. RAK's RAK9154 payload decoder: battery fault bitfield OR battery FW version
constexpr uint8_t  IPSO_CAPACITY    = 0xB8;   // 3384, 1 byte, percent
constexpr uint8_t  IPSO_DC_CURRENT  = 0xB9;   // 3385, 2 bytes, signed mA
constexpr uint8_t  IPSO_DC_VOLTAGE  = 0xBA;   // 3386, 2 bytes, unit 10 mV

// ---- checksum / low level --------------------------------------------------------------------

uint8_t popcount8(uint8_t v);

// Checksum over TYPE, FLAG and `len` header+payload bytes starting at `hub`.
uint8_t checksum(uint8_t type, uint8_t flag, const uint8_t* hub, size_t len);

// Size in bytes of the value carried by an IPSO id, or 0 if unknown to this driver.
uint8_t ipsoValueSize(uint8_t ipso);

// ---- parsed frame ----------------------------------------------------------------------------

enum class FrameStatus : uint8_t {
  OK = 0,
  TOO_SHORT,       // fewer bytes than a minimal frame, or than LEN promises
  BAD_DELIMITER,
  BAD_LENGTH,      // LEN out of range or inconsistent with payload length
  BAD_TYPE,        // TYPE is not SensorHub / FLAG invalid
  BAD_CHECKSUM,
};

struct Frame {
  uint8_t        flag;
  uint8_t        dest;
  uint8_t        source;
  uint8_t        sequence;
  uint8_t        hubType;
  uint8_t        payloadLen;      // as claimed by the header (may be 1 more than present for old probes)
  uint8_t        payloadType;
  uint8_t        dataLen;         // payload bytes actually present = LEN - 6
  const uint8_t* data;            // points into the caller's buffer, valid for dataLen bytes
};

// Validate a complete frame that STARTS AT the delimiter byte (0x7E). Never reads past `size`.
FrameStatus parseFrame(const uint8_t* buf, size_t size, Frame& out);

// ---- incremental receive assembly ------------------------------------------------------------

// Collects bytes from the wire into a complete candidate frame. It only does framing (delimiter,
// LEN bound, inter-byte timeout); validity is decided by parseFrame().
class FrameAssembler {
public:
  void     reset();
  // Feed one byte. Returns true when `size()` bytes form a complete candidate frame (starting at the delimiter).
  bool     push(uint8_t b, uint32_t nowMs);
  const uint8_t* data() const { return _buf; }
  size_t   size() const { return _have; }
  // Count of candidates discarded because LEN was implausible or a frame stalled mid-way.
  uint32_t framingErrors() const { return _framingErrors; }
  void     clearFrame() { reset(); }
  static constexpr uint32_t INTERBYTE_TIMEOUT_MS = 100;
private:
  uint8_t  _buf[MAX_FRAME];
  size_t   _have = 0;
  size_t   _need = 0;      // total bytes expected once LEN is known, 0 = unknown yet
  bool     _complete = false;
  uint32_t _lastByteMs = 0;
  uint32_t _framingErrors = 0;
};

// ---- frame builders --------------------------------------------------------------------------

// Master -> probe "provision BOOT" broadcast. Returns number of bytes written (13), 0 if `cap` too small.
size_t buildProvisionBoot(uint8_t seq, uint8_t* out, size_t cap);

// Master -> probe "get sensor data" for `pid`. Returns 13, or 0 if cap too small.
size_t buildGetData(uint8_t pid, uint8_t seq, uint8_t* out, size_t cap);

// Master reply to a probe provision announcement: echoes the announcement as a RSP assigning `pid`.
// `req`/`reqSize` is the received candidate frame (starting at delimiter), already validated with parseFrame.
// Returns bytes written (includes the wakeup byte), or 0 on error.
size_t buildProvisionResponse(const uint8_t* req, size_t reqSize, uint8_t pid, uint8_t* out, size_t cap);

// ---- payload decoding ------------------------------------------------------------------------

struct DecodeOptions {
  // Meshtastic decodes 16-bit values from "get data" responses little-endian and from unsolicited reports
  // big-endian. UNVERIFIED on real hardware: these defaults mirror that observation and are meant to be
  // changed from bench results (-D RAK9154_RSP_BIG_ENDIAN / RAK9154_REPORT_BIG_ENDIAN).
  bool responseBigEndian = false;   // FLAG_RSP frames
  bool reportBigEndian   = true;    // FLAG_REQ (unsolicited) frames
  // Plausibility window for the decoded pack voltage; values outside are rejected, not cached.
  uint32_t minVoltage_mV = 1000;
  uint32_t maxVoltage_mV = 60000;
};

struct Reading {
  bool     hasVoltage = false;  uint32_t voltage_mV = 0;
  bool     hasCurrent = false;  int32_t  current_mA = 0;
  bool     hasSoc     = false;  uint8_t  soc = 0;
  bool     hasTemp    = false;  int16_t  temp_decideg = 0;   // 0.1 degC, IPSO temperature convention, UNVERIFIED
  // 0xF3 entries (up to 2 kept). Per RAK's RAK9154 payload decoder the same id carries both the fault bitfield and the
  // battery firmware version, told apart only by the channel byte, which is not documented for the faults. So both
  // are stored raw with their channel and classified by describeStatusWord() for display only.
  uint8_t  statusCount = 0;
  uint8_t  statusChannel[2] = {0, 0};
  uint16_t statusValue[2]   = {0, 0};
  uint8_t  unknownIds = 0;      // entries with an IPSO id this driver cannot size (walk stops there)
  uint8_t  rejected   = 0;      // entries decoded but discarded as implausible
};

// Human readable text for a 0xF3 value. Bits 6..11 are fault flags (RAK docs: 6 over-voltage, 7 charge over-current,
// 8 charge low-temp, 9 charge high-temp, 10 charge short-circuit, 11 charge over-current lock; 0-5 and 12-15 reserved).
// A word that has any bit outside 6..11 cannot be a fault word, so it is shown as a firmware version "Vhh.ll"
// (RAK example: 0x0002 -> V00.02). A word of 0 means "no faults". Heuristic: UNVERIFIED on a real probe.
size_t describeStatusWord(uint16_t value, char* out, size_t cap);

enum class DecodeStatus : uint8_t { OK = 0, NOT_DATA, MALFORMED };

// Decode a validated data frame (hubType SENDDATA) into a Reading. Walks {sid, ipso, value} entries strictly
// within `f.dataLen`; a truncated entry yields MALFORMED (fields decoded before it are kept in `r`).
DecodeStatus decodeSensorData(const Frame& f, const DecodeOptions& opt, Reading& r);

// Information carried by a probe's provision announcement.
struct ProvisionInfo {
  uint8_t hw = 0;
  uint8_t sw[3] = {0, 0, 0};
  char    model[21] = {0};
  uint8_t sensorCount = 0;
  uint8_t sid[8] = {0};
  uint8_t ipso[8] = {0};
};

// Decode a provision announcement payload (hubType PROVISION, payloadType VER3). Bounds-checked.
bool decodeProvision(const Frame& f, ProvisionInfo& info);

}  // namespace rak9154
