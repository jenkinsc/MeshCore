// SPDX-License-Identifier: MIT
//
// Test fixtures for the RAK9154 protocol/driver tests.
//
// IMPORTANT: these frames are SYNTHETIC. They are produced by an encoder written independently of the driver
// (below) from the wire format documented in RAK9154Protocol.h; they are NOT captures from a real RAK9154.
// When real captures become available (build with -D RAK9154_DEBUG and copy the "RX:" lines), add them to
// kCaptured[] in test_protocol.cpp so regressions against real traffic are caught.
#pragma once

#include <stdint.h>
#include <initializer_list>
#include <utility>
#include <vector>

namespace fx {

using Bytes = std::vector<uint8_t>;

struct FrameSpec {
  uint8_t flag = 1;            // RSP by default
  uint8_t dest = 0x00;
  uint8_t source = 0x01;
  uint8_t seq = 1;
  uint8_t hubType = 3;         // send data
  uint8_t payloadType = 2;
  Bytes   payload;
  uint8_t type = 2;
  bool    wakeup = true;
  int     lenAdj = 0;          // added to LEN to build inconsistent frames
  int     plenAdj = 0;         // added to the header payload-length byte
  int     chkAdj = 0;          // added to the checksum
  bool    omitChecksum = false;
};

inline uint8_t pc(uint8_t v) { return (uint8_t)__builtin_popcount(v); }

// Independent encoder (written separately from the driver's builders on purpose).
inline Bytes encode(const FrameSpec& s) {
  Bytes hub = {s.dest, s.source, s.seq, s.hubType, (uint8_t)(s.payload.size() + s.plenAdj), s.payloadType};
  hub.insert(hub.end(), s.payload.begin(), s.payload.end());
  const unsigned len = (unsigned)hub.size() + s.lenAdj;
  uint8_t sum = pc(s.type) + pc(s.flag);
  for (uint8_t b : hub) sum += pc(b);
  sum += s.chkAdj;
  Bytes out;
  if (s.wakeup) out.push_back(0xFF);
  out.push_back(0x7E);
  out.push_back((uint8_t)(len >> 8));
  out.push_back((uint8_t)len);
  out.push_back(s.type);
  out.push_back(s.flag);
  out.insert(out.end(), hub.begin(), hub.end());
  if (!s.omitChecksum) out.push_back(sum);
  return out;
}

inline Bytes entry(uint8_t sid, uint8_t ipso, const Bytes& value) {
  Bytes e = {sid, ipso};
  e.insert(e.end(), value.begin(), value.end());
  return e;
}
inline Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

// Value encoders
inline Bytes u16(uint16_t v, bool big) {
  return big ? Bytes{(uint8_t)(v >> 8), (uint8_t)v} : Bytes{(uint8_t)v, (uint8_t)(v >> 8)};
}

constexpr uint8_t IPSO_SOC = 0xB8, IPSO_CUR = 0xB9, IPSO_VOLT = 0xBA, IPSO_TEMP = 0x67;

// A typical "get data" response: 12.31 V, +185 mA, 94 %. RSP frames default to little-endian values.
inline Bytes typicalResponse(bool big = false, uint16_t volt10mV = 1231, int16_t mA = 185, uint8_t soc = 94, uint8_t seq = 7) {
  FrameSpec s;
  s.flag = 1; s.seq = seq;
  s.payload = cat({entry(0, IPSO_VOLT, u16(volt10mV, big)),
                   entry(0, IPSO_CUR, u16((uint16_t)mA, big)),
                   entry(0, IPSO_SOC, {soc})});
  return encode(s);
}

// Unsolicited report (flag REQ), big-endian values by default.
inline Bytes typicalReport(uint16_t volt10mV = 1231, int16_t mA = 185, uint8_t soc = 94, bool big = true) {
  FrameSpec s;
  s.flag = 0; s.dest = 0x00; s.source = 0x01;
  s.payload = cat({entry(0, IPSO_VOLT, u16(volt10mV, big)),
                   entry(0, IPSO_CUR, u16((uint16_t)mA, big)),
                   entry(0, IPSO_SOC, {soc})});
  return encode(s);
}

// Provision announcement from a probe (hub type 1, payload type 3), with n sensors.
inline Bytes provisionAnnouncement(const std::vector<std::pair<uint8_t, uint8_t>>& sensors, uint8_t source = 0xFF) {
  Bytes p(55, 0);
  p[0] = 2;                       // hw
  p[1] = 1; p[2] = 2; p[3] = 3;   // sw
  const char* model = "RAK9154-TEST";
  for (int i = 0; model[i]; i++) p[30 + i] = (uint8_t)model[i];
  p[54] = (uint8_t)sensors.size();
  for (auto& s : sensors) { p.push_back(s.first); p.push_back(s.second); p.push_back(0x08); p.push_back(0x00); }
  FrameSpec f;
  f.flag = 0; f.dest = 0x00; f.source = source; f.hubType = 1; f.payloadType = 3; f.payload = p;
  return encode(f);
}

}  // namespace fx
