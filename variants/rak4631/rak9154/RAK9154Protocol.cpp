// SPDX-License-Identifier: MIT
//
// Hardware independent. Compiled either into the RAK9154 firmware target (RAK9154_BATTERY) or into the
// host unit tests (RAK9154_HOST_TEST); it compiles to nothing in every other build, so stock targets
// are unchanged. See RAK9154Protocol.h for the wire format and its provenance.
#if defined(RAK9154_BATTERY) || defined(RAK9154_HOST_TEST)

#include "RAK9154Protocol.h"

#include <stdio.h>

#include <string.h>

namespace rak9154 {

// Offsets inside the provision announcement payload (RAK lib: SNHub_Api_Provision_t, packed).
static constexpr size_t PROV_OFF_HW       = 0;    // 1 byte
static constexpr size_t PROV_OFF_SW       = 1;    // 3 bytes
static constexpr size_t PROV_OFF_PROVID   = 22;   // after 18 byte serial number
static constexpr size_t PROV_OFF_MODEL    = 30;   // 20 bytes, after 7 reserved
static constexpr size_t PROV_OFF_SNSRNUM  = 54;   // after 4 reserved
static constexpr size_t PROV_OFF_SNSRLIST = 55;   // {sid, ipso, rule(2)} * n
static constexpr size_t PROV_MIN_SIZE     = PROV_OFF_SNSRLIST;

uint8_t popcount8(uint8_t v) {
  v = (uint8_t)((v & 0x55) + ((v >> 1) & 0x55));
  v = (uint8_t)((v & 0x33) + ((v >> 2) & 0x33));
  v = (uint8_t)((v & 0x0F) + ((v >> 4) & 0x0F));
  return v;
}

uint8_t checksum(uint8_t type, uint8_t flag, const uint8_t* hub, size_t len) {
  uint8_t sum = (uint8_t)(popcount8(type) + popcount8(flag));
  for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + popcount8(hub[i]));
  return sum;
}

// Value sizes of the IPSO ids known to the RAK one-wire library (onewire_master_protocol.c, rakipso_tbl).
// Needed only to step over entries we do not care about when several share one frame.
uint8_t ipsoValueSize(uint8_t ipso) {
  switch (ipso) {
    case 0x00: case 0x01: case 0x66: case 0x68: case 0x78: case 0xB8:
      return 1;
    case 0x02: case 0x03: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
    case 0x17: case 0x18: case 0x19: case 0x1A: case 0x67: case 0x69: case 0x70: case 0xF3: case 0x73: case 0x74:
    case 0x77: case 0x7D: case 0x8A: case 0x92: case 0x93: case 0xB9: case 0xBA: case 0xBC: case 0xBE:
    case 0xBF: case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xE3: case 0xE4: case 0xE5: case 0xE9:
      return 2;
    case 0x7E:
      return 3;
    case 0x65: case 0x7F: case 0x82:
      return 4;
    case 0x71: case 0x86:
      return 6;
    case 0x88:
      return 9;
    case 0x89:
      return 11;
    default:
      return 0;   // unknown to this driver (the RAK ids 0xF1..0xF8 are variable length and not handled)
  }
}

// ---- parsing ---------------------------------------------------------------------------------

FrameStatus parseFrame(const uint8_t* buf, size_t size, Frame& out) {
  if (buf == nullptr || size < 1 + 2 + 2 + HUB_HEADER_SIZE + 1) return FrameStatus::TOO_SHORT;
  if (buf[0] != DELIMITER) return FrameStatus::BAD_DELIMITER;

  const size_t len = ((size_t)buf[1] << 8) | buf[2];
  if (len < HUB_HEADER_SIZE || len > HUB_HEADER_SIZE + MAX_PAYLOAD) return FrameStatus::BAD_LENGTH;

  const size_t total = 5 + len + 1;
  if (size < total) return FrameStatus::TOO_SHORT;

  const uint8_t type = buf[3];
  const uint8_t flag = buf[4];
  const uint8_t* hub = buf + 5;

  if (checksum(type, flag, hub, len) != buf[5 + len]) return FrameStatus::BAD_CHECKSUM;
  if (type != TYPE_SENSORHUB || flag > FLAG_RSP) return FrameStatus::BAD_TYPE;

  const uint8_t plen = hub[4];
  // LEN normally = header + payloadLen. The RAK reference accepts one byte less (old probe firmware, "crc byte" quirk).
  if (!(len == HUB_HEADER_SIZE + (size_t)plen || (plen >= 1 && len == HUB_HEADER_SIZE + (size_t)plen - 1)))
    return FrameStatus::BAD_LENGTH;

  out.flag        = flag;
  out.dest        = hub[0];
  out.source      = hub[1];
  out.sequence    = hub[2];
  out.hubType     = hub[3];
  out.payloadLen  = plen;
  out.payloadType = hub[5];
  out.dataLen     = (uint8_t)(len - HUB_HEADER_SIZE);
  out.data        = hub + HUB_HEADER_SIZE;
  return FrameStatus::OK;
}

// ---- frame assembly --------------------------------------------------------------------------

void FrameAssembler::reset() {
  _have = 0;
  _need = 0;
  _complete = false;
}

bool FrameAssembler::push(uint8_t b, uint32_t nowMs) {
  if (_complete) reset();

  // A frame that stalls mid-way (probe stopped, noise) must not wedge the assembler.
  if (_have > 0 && (uint32_t)(nowMs - _lastByteMs) > INTERBYTE_TIMEOUT_MS) {
    _framingErrors++;
    reset();
  }

  if (_have == 0) {
    if (b != DELIMITER) return false;     // skip wakeup byte / line noise before a frame
    _buf[_have++] = b;
    _lastByteMs = nowMs;
    return false;
  }

  _buf[_have++] = b;
  _lastByteMs = nowMs;

  if (_have == 3) {
    const size_t len = ((size_t)_buf[1] << 8) | _buf[2];
    if (len < HUB_HEADER_SIZE || len > HUB_HEADER_SIZE + MAX_PAYLOAD) {
      // Implausible LEN: the delimiter we locked on was probably data. Re-scan the two bytes we hold.
      _framingErrors++;
      const uint8_t a = _buf[1], c = _buf[2];
      reset();
      push(a, nowMs);
      push(c, nowMs);
      return false;
    }
    _need = 5 + len + 1;
  }

  if (_need != 0 && _have >= _need) {
    _complete = true;
    return true;
  }
  return false;
}

// ---- builders --------------------------------------------------------------------------------

static size_t buildHeaderOnly(uint8_t hubType, uint8_t dest, uint8_t seq, uint8_t payloadType, uint8_t* out, size_t cap) {
  constexpr size_t TOTAL = 1 + 1 + 2 + 2 + HUB_HEADER_SIZE + 1;   // 13
  if (out == nullptr || cap < TOTAL) return 0;
  out[0]  = WAKEUP_BYTE;
  out[1]  = DELIMITER;
  out[2]  = 0x00;                       // LEN high
  out[3]  = (uint8_t)HUB_HEADER_SIZE;   // LEN low: header only, no payload
  out[4]  = TYPE_SENSORHUB;
  out[5]  = FLAG_REQ;
  out[6]  = dest;
  out[7]  = PID_MASTER;
  out[8]  = seq;
  out[9]  = hubType;
  out[10] = 0;                          // payload length
  out[11] = payloadType;
  out[12] = checksum(TYPE_SENSORHUB, FLAG_REQ, &out[6], HUB_HEADER_SIZE);
  return TOTAL;
}

size_t buildProvisionBoot(uint8_t seq, uint8_t* out, size_t cap) {
  return buildHeaderOnly(HUB_PROVISION, PID_UNKNOWN, seq, PROV_BOOT, out, cap);
}

size_t buildGetData(uint8_t pid, uint8_t seq, uint8_t* out, size_t cap) {
  return buildHeaderOnly(HUB_SENDDATA, pid, seq, SDATA_SENDAT, out, cap);
}

size_t buildProvisionResponse(const uint8_t* req, size_t reqSize, uint8_t pid, uint8_t* out, size_t cap) {
  Frame f;
  if (parseFrame(req, reqSize, f) != FrameStatus::OK) return 0;
  if (f.flag != FLAG_REQ || f.hubType != HUB_PROVISION || f.payloadType != PROV_VER3) return 0;
  if (f.dataLen < PROV_MIN_SIZE) return 0;

  const size_t len   = HUB_HEADER_SIZE + f.dataLen;
  const size_t total = 5 + len + 1;
  if (out == nullptr || cap < 1 + total) return 0;

  out[0] = WAKEUP_BYTE;
  memcpy(&out[1], req, total);
  uint8_t* body = &out[1];
  body[4] = FLAG_RSP;
  body[5] = f.source;                    // dest   <- probe's source
  body[6] = f.dest;                      // source <- our address
  body[5 + HUB_HEADER_SIZE + PROV_OFF_PROVID] = pid;
  body[5 + len] = checksum(body[3], FLAG_RSP, &body[5], len);
  return 1 + total;
}

// ---- decoding --------------------------------------------------------------------------------

static uint16_t readU16(const uint8_t* p, bool bigEndian) {
  return bigEndian ? (uint16_t)(((uint16_t)p[0] << 8) | p[1]) : (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

size_t describeStatusWord(uint16_t value, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  static const char* const names[6] = {"OV", "CHG-OC", "CHG-LT", "CHG-HT", "CHG-SC", "CHG-OCLOCK"};
  int n;
  if (value == 0) {
    n = snprintf(out, cap, "faults:none");
  } else if ((value & ~0x0FC0u) == 0) {
    n = snprintf(out, cap, "faults:");
    for (int bit = 6; bit <= 11 && n > 0 && (size_t)n < cap; bit++)
      if (value & (1u << bit)) n += snprintf(out + n, cap - (size_t)n, "%s%s", n > 7 ? "," : "", names[bit - 6]);
  } else {
    n = snprintf(out, cap, "fw:V%02u.%02u", (unsigned)(value >> 8), (unsigned)(value & 0xFF));
  }
  if (n < 0) { out[0] = 0; return 0; }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

DecodeStatus decodeSensorData(const Frame& f, const DecodeOptions& opt, Reading& r) {
  if (f.hubType != HUB_SENDDATA) return DecodeStatus::NOT_DATA;
  const bool big = (f.flag == FLAG_RSP) ? opt.responseBigEndian : opt.reportBigEndian;

  size_t off = 0;
  while (off < f.dataLen) {
    if (f.dataLen - off < 2) return DecodeStatus::MALFORMED;          // dangling byte
    const uint8_t ipso = f.data[off + 1];
    const uint8_t size = ipsoValueSize(ipso);
    if (size == 0) {                                                  // cannot step over what we cannot size
      r.unknownIds++;
      return DecodeStatus::OK;
    }
    if (f.dataLen - off - 2 < size) return DecodeStatus::MALFORMED;   // truncated value
    const uint8_t* v = &f.data[off + 2];

    switch (ipso) {
      case IPSO_CAPACITY:
        r.hasSoc = true;
        r.soc = v[0] > 100 ? 100 : v[0];
        break;
      case IPSO_DC_CURRENT:
        r.hasCurrent = true;
        r.current_mA = (int16_t)readU16(v, big);
        break;
      case IPSO_DC_VOLTAGE: {
        const uint32_t mv = (uint32_t)readU16(v, big) * 10u;
        if (mv >= opt.minVoltage_mV && mv <= opt.maxVoltage_mV) {
          r.hasVoltage = true;
          r.voltage_mV = mv;
        } else {
          r.rejected++;
        }
        break;
      }
      case IPSO_TEMPERATURE: {
        const int16_t t = (int16_t)readU16(v, big);
        if (t >= -400 && t <= 1250) {          // -40.0 .. 125.0 degC
          r.hasTemp = true;
          r.temp_decideg = t;
        } else {
          r.rejected++;
        }
        break;
      }
      case IPSO_BATT_STATUS:
        if (r.statusCount < 2) {
          r.statusChannel[r.statusCount] = f.data[off];
          r.statusValue[r.statusCount]   = readU16(v, big);
          r.statusCount++;
        }
        break;
      default:
        break;                                 // known size, not interesting
    }
    off += 2u + size;
  }
  return DecodeStatus::OK;
}

bool decodeProvision(const Frame& f, ProvisionInfo& info) {
  if (f.hubType != HUB_PROVISION || f.payloadType != PROV_VER3 || f.dataLen < PROV_MIN_SIZE) return false;
  const uint8_t* d = f.data;

  info.hw = d[PROV_OFF_HW];
  memcpy(info.sw, &d[PROV_OFF_SW], 3);
  for (size_t i = 0; i < 20; i++) {
    const uint8_t c = d[PROV_OFF_MODEL + i];
    info.model[i] = (c >= 0x20 && c < 0x7F) ? (char)c : (c == 0 ? '\0' : '?');
  }
  info.model[20] = '\0';

  const uint8_t count = d[PROV_OFF_SNSRNUM];
  if ((size_t)count * 4 > (size_t)f.dataLen - PROV_OFF_SNSRLIST) return false;
  info.sensorCount = count;
  for (uint8_t i = 0; i < count && i < sizeof(info.sid); i++) {
    info.sid[i]  = d[PROV_OFF_SNSRLIST + (size_t)i * 4];
    info.ipso[i] = d[PROV_OFF_SNSRLIST + (size_t)i * 4 + 1];
  }
  return true;
}

}  // namespace rak9154

#endif  // RAK9154_BATTERY || RAK9154_HOST_TEST
