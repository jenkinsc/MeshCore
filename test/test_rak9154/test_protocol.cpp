// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <stdlib.h>
#include <string.h>

#include "RAK9154Protocol.h"
#include "fixtures.h"

using namespace rak9154;
using namespace fx;

// Strip the wakeup byte so the buffer starts at the delimiter, as the assembler delivers it.
static Bytes body(const Bytes& wire) { return Bytes(wire.begin() + (wire[0] == 0xFF ? 1 : 0), wire.end()); }

// Frame::data points into the buffer it was parsed from, so keep the last few parsed buffers alive.
static FrameStatus parse(const Bytes& wire, Frame& f) {
  static Bytes keep[8];
  static unsigned next = 0;
  Bytes& b = keep[next++ % 8];
  b = body(wire);
  return parseFrame(b.data(), b.size(), f);
}

// ---------------------------------------------------------------- builders (golden bytes) -------

TEST(Builders, ProvisionBootGolden) {
  uint8_t out[32];
  ASSERT_EQ(buildProvisionBoot(1, out, sizeof(out)), 13u);
  const uint8_t expect[] = {0xFF, 0x7E, 0x00, 0x06, 0x02, 0x00, 0xFF, 0x00, 0x01, 0x01, 0x00, 0x02, 0x0C};
  EXPECT_EQ(0, memcmp(out, expect, sizeof(expect)));
}

TEST(Builders, GetDataGolden) {
  uint8_t out[32];
  ASSERT_EQ(buildGetData(1, 5, out, sizeof(out)), 13u);
  const uint8_t expect[] = {0xFF, 0x7E, 0x00, 0x06, 0x02, 0x00, 0x01, 0x00, 0x05, 0x03, 0x00, 0x02, 0x07};
  EXPECT_EQ(0, memcmp(out, expect, sizeof(expect)));
}

TEST(Builders, AgreeWithIndependentEncoder) {
  uint8_t out[32];
  FrameSpec s; s.flag = 0; s.dest = 0xFF; s.source = 0; s.seq = 9; s.hubType = 1; s.payloadType = 2;
  Bytes e = encode(s);
  ASSERT_EQ(buildProvisionBoot(9, out, sizeof(out)), e.size());
  EXPECT_EQ(0, memcmp(out, e.data(), e.size()));
}

TEST(Builders, RejectSmallBuffer) {
  uint8_t out[12];
  EXPECT_EQ(buildProvisionBoot(1, out, sizeof(out)), 0u);
  EXPECT_EQ(buildGetData(1, 1, out, sizeof(out)), 0u);
  EXPECT_EQ(buildGetData(1, 1, nullptr, 100), 0u);
}

TEST(Builders, BuiltFramesParse) {
  uint8_t out[32];
  size_t n = buildGetData(3, 77, out, sizeof(out));
  Frame f;
  ASSERT_EQ(parseFrame(out + 1, n - 1, f), FrameStatus::OK);
  EXPECT_EQ(f.dest, 3);
  EXPECT_EQ(f.source, 0);
  EXPECT_EQ(f.sequence, 77);
  EXPECT_EQ(f.hubType, HUB_SENDDATA);
  EXPECT_EQ(f.dataLen, 0);
}

// ---------------------------------------------------------------- parser -------------------------

TEST(Parser, ValidFrame) {
  Frame f;
  ASSERT_EQ(parse(typicalResponse(), f), FrameStatus::OK);
  EXPECT_EQ(f.flag, FLAG_RSP);
  EXPECT_EQ(f.hubType, HUB_SENDDATA);
  EXPECT_EQ(f.dataLen, 3 * 2 + 2 + 2 + 1);
}

TEST(Parser, ShortFrames) {
  Frame f;
  Bytes full = body(typicalResponse());
  for (size_t n = 0; n < full.size(); n++) {
    EXPECT_EQ(parseFrame(full.data(), n, f), FrameStatus::TOO_SHORT) << "n=" << n;
  }
  EXPECT_EQ(parseFrame(nullptr, 0, f), FrameStatus::TOO_SHORT);
}

TEST(Parser, BadChecksum) {
  Frame f;
  FrameSpec s; s.payload = entry(0, IPSO_VOLT, u16(1231, false)); s.chkAdj = 1;
  EXPECT_EQ(parse(encode(s), f), FrameStatus::BAD_CHECKSUM);
  // every single-bit corruption of a good frame must be rejected (popcount sums change or LEN/type break)
  Bytes good = body(typicalResponse());
  for (size_t i = 0; i < good.size(); i++)
    for (int bit = 0; bit < 8; bit++) {
      Bytes bad = good;
      bad[i] ^= (uint8_t)(1u << bit);
      FrameStatus st = parseFrame(bad.data(), bad.size(), f);
      // Flipping a bit in the wire LEN or delimiter yields other errors; what matters is it is never OK
      // with different content. A flip that keeps popcount parity and the same byte count is the known
      // weakness of a popcount checksum, so only assert on positions the checksum covers.
      if (i >= 5 && i + 1 < good.size()) {
        EXPECT_NE(st, FrameStatus::OK) << "byte " << i << " bit " << bit;
      }
    }
}

TEST(Parser, BadDelimiterTypeAndLength) {
  Frame f;
  Bytes b = body(typicalResponse());
  b[0] = 0x7F;
  EXPECT_EQ(parseFrame(b.data(), b.size(), f), FrameStatus::BAD_DELIMITER);

  FrameSpec s; s.payload = entry(0, IPSO_SOC, {50}); s.type = 1;       // AT command type, valid checksum
  EXPECT_EQ(parse(encode(s), f), FrameStatus::BAD_TYPE);
  s.type = 2; s.flag = 2;
  EXPECT_EQ(parse(encode(s), f), FrameStatus::BAD_TYPE);

  s = FrameSpec(); s.payload = entry(0, IPSO_SOC, {50}); s.plenAdj = 3;  // header payload length disagrees with LEN
  EXPECT_EQ(parse(encode(s), f), FrameStatus::BAD_LENGTH);
  s = FrameSpec(); s.payload = entry(0, IPSO_SOC, {50}); s.plenAdj = -1;
  EXPECT_EQ(parse(encode(s), f), FrameStatus::BAD_LENGTH);               // only the "one byte less" quirk is tolerated
  s = FrameSpec(); s.payload = entry(0, IPSO_SOC, {50}); s.lenAdj = 3;   // LEN promises bytes that never arrive
  EXPECT_EQ(parse(encode(s), f), FrameStatus::TOO_SHORT);

  Bytes huge = {0x7E, 0xFF, 0xFF, 2, 1};
  EXPECT_EQ(parseFrame(huge.data(), huge.size(), f), FrameStatus::TOO_SHORT);   // too short to even judge...
  huge.resize(20, 0);
  EXPECT_EQ(parseFrame(huge.data(), huge.size(), f), FrameStatus::BAD_LENGTH);
}

TEST(Parser, OldProbeLengthQuirkAccepted) {
  // RAK lib tolerates LEN == 6 + payloadLen - 1 ("old version will add crc byte")
  Frame f;
  FrameSpec s; s.payload = entry(0, IPSO_SOC, {77}); s.plenAdj = 1;
  Bytes w = encode(s);
  // encode() computes LEN from the true payload, header byte says one more -> quirk shape
  ASSERT_EQ(parse(w, f), FrameStatus::OK);
  EXPECT_EQ(f.dataLen, 3);
  EXPECT_EQ(f.payloadLen, 4);
}

// ---------------------------------------------------------------- decoder ------------------------

static Reading decodeOk(const Bytes& wire, const DecodeOptions& o = DecodeOptions()) {
  Frame f;
  EXPECT_EQ(parse(wire, f), FrameStatus::OK);
  Reading r;
  EXPECT_EQ(decodeSensorData(f, o, r), DecodeStatus::OK);
  return r;
}

TEST(Decoder, TypicalResponseLittleEndian) {
  Bytes w = typicalResponse(false);
  Bytes keep = body(w);
  Frame f; ASSERT_EQ(parseFrame(keep.data(), keep.size(), f), FrameStatus::OK);
  Reading r;
  ASSERT_EQ(decodeSensorData(f, DecodeOptions(), r), DecodeStatus::OK);
  EXPECT_TRUE(r.hasVoltage); EXPECT_EQ(r.voltage_mV, 12310u);
  EXPECT_TRUE(r.hasCurrent); EXPECT_EQ(r.current_mA, 185);
  EXPECT_TRUE(r.hasSoc);     EXPECT_EQ(r.soc, 94);
  EXPECT_FALSE(r.hasTemp);
}

TEST(Decoder, ReportIsBigEndianByDefault) {
  Reading r = decodeOk(typicalReport());
  EXPECT_EQ(r.voltage_mV, 12310u);
  EXPECT_EQ(r.current_mA, 185);
}

TEST(Decoder, ByteOrderIsConfigurable) {
  DecodeOptions o; o.responseBigEndian = true;
  Reading r = decodeOk(typicalResponse(true), o);
  EXPECT_EQ(r.voltage_mV, 12310u);
  // wrong order for the data is caught by the plausibility window, not cached
  Reading wrong = decodeOk(typicalResponse(true));    // decoded little-endian: 0xCF04 * 10 = 530,... mV
  EXPECT_FALSE(wrong.hasVoltage);
  EXPECT_EQ(wrong.rejected, 1);
}

TEST(Decoder, ReorderedAndMissingFields) {
  FrameSpec s;
  s.payload = cat({entry(0, IPSO_SOC, {55}), entry(0, IPSO_CUR, u16((uint16_t)-120, false)), entry(0, IPSO_VOLT, u16(1100, false))});
  Reading r = decodeOk(encode(s));
  EXPECT_EQ(r.soc, 55); EXPECT_EQ(r.current_mA, -120); EXPECT_EQ(r.voltage_mV, 11000u);

  s.payload = entry(0, IPSO_VOLT, u16(1250, false));       // voltage only
  r = decodeOk(encode(s));
  EXPECT_TRUE(r.hasVoltage); EXPECT_FALSE(r.hasCurrent); EXPECT_FALSE(r.hasSoc);

  s.payload = entry(0, IPSO_CUR, u16(5, false));           // no voltage at all
  r = decodeOk(encode(s));
  EXPECT_FALSE(r.hasVoltage); EXPECT_TRUE(r.hasCurrent);
}

TEST(Decoder, NegativeAndExtremeCurrent) {
  FrameSpec s;
  s.payload = entry(0, IPSO_CUR, u16(0x8000, false));
  EXPECT_EQ(decodeOk(encode(s)).current_mA, -32768);
  s.payload = entry(0, IPSO_CUR, u16(0x7FFF, false));
  EXPECT_EQ(decodeOk(encode(s)).current_mA, 32767);
  s.payload = entry(0, IPSO_CUR, u16(0xFFFF, false));
  EXPECT_EQ(decodeOk(encode(s)).current_mA, -1);
}

TEST(Decoder, VoltageLimits) {
  FrameSpec s;
  auto volt = [&](uint16_t raw) { s.payload = entry(0, IPSO_VOLT, u16(raw, false)); return decodeOk(encode(s)); };
  EXPECT_TRUE(volt(100).hasVoltage);            // 1.00 V = lower bound
  EXPECT_FALSE(volt(99).hasVoltage);            // 0.99 V rejected
  EXPECT_FALSE(volt(0).hasVoltage);
  EXPECT_TRUE(volt(6000).hasVoltage);           // 60.00 V = upper bound
  EXPECT_FALSE(volt(6001).hasVoltage);
  EXPECT_FALSE(volt(0xFFFF).hasVoltage);        // 655.35 V: never cached (also would overflow uint16 mV)
}

TEST(Decoder, SocClampedTo100) {
  FrameSpec s;
  s.payload = entry(0, IPSO_SOC, {255});
  EXPECT_EQ(decodeOk(encode(s)).soc, 100);
  s.payload = entry(0, IPSO_SOC, {0});
  Reading r = decodeOk(encode(s));
  EXPECT_TRUE(r.hasSoc); EXPECT_EQ(r.soc, 0);
}

TEST(Decoder, TemperatureWindow) {
  FrameSpec s;
  s.payload = entry(0, IPSO_TEMP, u16(268, false));
  Reading r = decodeOk(encode(s));
  EXPECT_TRUE(r.hasTemp); EXPECT_EQ(r.temp_decideg, 268);
  s.payload = entry(0, IPSO_TEMP, u16((uint16_t)-100, false));
  EXPECT_EQ(decodeOk(encode(s)).temp_decideg, -100);
  s.payload = entry(0, IPSO_TEMP, u16(2000, false));
  r = decodeOk(encode(s));
  EXPECT_FALSE(r.hasTemp); EXPECT_EQ(r.rejected, 1);
}

TEST(Decoder, UnknownChannelStopsWalkButKeepsEarlierFields) {
  FrameSpec s;
  s.payload = cat({entry(0, IPSO_VOLT, u16(1231, false)), entry(0, 0x42, {1, 2, 3}), entry(0, IPSO_SOC, {10})});
  Reading r = decodeOk(encode(s));
  EXPECT_TRUE(r.hasVoltage);
  EXPECT_FALSE(r.hasSoc);                      // after an unsizable entry we cannot know where the next starts
  EXPECT_EQ(r.unknownIds, 1);
}

TEST(Decoder, KnownButIgnoredChannelIsSkipped) {
  FrameSpec s;   // 0x71 accelerometer (6 bytes) between our fields
  s.payload = cat({entry(0, 0x71, {1, 2, 3, 4, 5, 6}), entry(0, IPSO_SOC, {42})});
  Reading r = decodeOk(encode(s));
  EXPECT_TRUE(r.hasSoc); EXPECT_EQ(r.soc, 42); EXPECT_EQ(r.unknownIds, 0);
}

TEST(Decoder, TruncatedEntriesAreMalformed) {
  Frame f;
  Reading r;
  FrameSpec s;
  s.payload = entry(0, IPSO_VOLT, {0x01});               // value needs 2 bytes
  ASSERT_EQ(parse(encode(s), f), FrameStatus::OK);
  EXPECT_EQ(decodeSensorData(f, DecodeOptions(), r), DecodeStatus::MALFORMED);

  s.payload = Bytes{0x00};                               // dangling single byte
  ASSERT_EQ(parse(encode(s), f), FrameStatus::OK);
  EXPECT_EQ(decodeSensorData(f, DecodeOptions(), r), DecodeStatus::MALFORMED);

  s.payload = Bytes{};                                   // empty data frame is fine, just yields nothing
  ASSERT_EQ(parse(encode(s), f), FrameStatus::OK);
  r = Reading();
  EXPECT_EQ(decodeSensorData(f, DecodeOptions(), r), DecodeStatus::OK);
  EXPECT_FALSE(r.hasVoltage);
}

TEST(Decoder, NonDataFrameIsNotData) {
  Frame f; Reading r;
  FrameSpec s; s.hubType = 8;                            // ACK
  ASSERT_EQ(parse(encode(s), f), FrameStatus::OK);
  EXPECT_EQ(decodeSensorData(f, DecodeOptions(), r), DecodeStatus::NOT_DATA);
}

// ---------------------------------------------------------------- provisioning -------------------

TEST(Provision, DecodeAnnouncement) {
  Bytes w = provisionAnnouncement({{0, IPSO_VOLT}, {1, IPSO_CUR}, {2, IPSO_SOC}});
  Frame f; ASSERT_EQ(parse(w, f), FrameStatus::OK);
  ProvisionInfo p;
  ASSERT_TRUE(decodeProvision(f, p));
  EXPECT_EQ(p.hw, 2);
  EXPECT_STREQ(p.model, "RAK9154-TEST");
  EXPECT_EQ(p.sensorCount, 3);
  EXPECT_EQ(p.ipso[0], IPSO_VOLT); EXPECT_EQ(p.ipso[1], IPSO_CUR); EXPECT_EQ(p.ipso[2], IPSO_SOC);
  EXPECT_EQ(p.sid[2], 2);
}

TEST(Provision, ClaimedSensorCountBeyondPayloadIsRejected) {
  Bytes w = provisionAnnouncement({{0, IPSO_VOLT}});
  Bytes b = body(w);
  // sensor count byte is payload[54] -> frame offset 5 (hdr start) + 6 + 54
  b[5 + 6 + 54] = 200;
  // fix the checksum so only decodeProvision's own bounds check is exercised
  size_t len = (b[1] << 8) | b[2];
  b[5 + len] = checksum(b[3], b[4], &b[5], len);
  Frame f; ASSERT_EQ(parseFrame(b.data(), b.size(), f), FrameStatus::OK);
  ProvisionInfo p;
  EXPECT_FALSE(decodeProvision(f, p));
}

TEST(Provision, ResponseEchoesAnnouncementWithAssignedPid) {
  Bytes w = provisionAnnouncement({{0, IPSO_VOLT}, {0, IPSO_CUR}}, 0xFF);
  Bytes b = body(w);
  uint8_t out[256];
  size_t n = buildProvisionResponse(b.data(), b.size(), 1, out, sizeof(out));
  ASSERT_EQ(n, b.size() + 1);
  EXPECT_EQ(out[0], 0xFF);
  Frame f; ASSERT_EQ(parseFrame(out + 1, n - 1, f), FrameStatus::OK);
  EXPECT_EQ(f.flag, FLAG_RSP);
  EXPECT_EQ(f.dest, 0xFF);      // old source
  EXPECT_EQ(f.source, 0x00);    // old dest
  EXPECT_EQ(f.hubType, HUB_PROVISION);
  EXPECT_EQ(f.data[22], 1);     // assigned PID
  // rest of payload untouched
  Frame orig; ASSERT_EQ(parseFrame(b.data(), b.size(), orig), FrameStatus::OK);
  for (size_t i = 0; i < f.dataLen; i++) {
    if (i == 22) continue;
    EXPECT_EQ(f.data[i], orig.data[i]) << i;
  }
}

TEST(Provision, ResponseRefusesBadInput) {
  uint8_t out[256];
  Bytes b = body(provisionAnnouncement({{0, IPSO_VOLT}}));
  EXPECT_EQ(buildProvisionResponse(b.data(), b.size(), 1, out, 10), 0u);         // buffer too small
  EXPECT_EQ(buildProvisionResponse(b.data(), b.size() - 1, 1, out, sizeof(out)), 0u);   // truncated
  Bytes d = body(typicalResponse());
  EXPECT_EQ(buildProvisionResponse(d.data(), d.size(), 1, out, sizeof(out)), 0u);      // not a provision frame
}

// ---------------------------------------------------------------- assembler ----------------------

static bool feed(FrameAssembler& a, const Bytes& wire, uint32_t& t, size_t* completeAt = nullptr) {
  bool got = false;
  for (size_t i = 0; i < wire.size(); i++) {
    if (a.push(wire[i], t++)) { got = true; if (completeAt) *completeAt = i; }
  }
  return got;
}

TEST(Assembler, AssemblesFrameAfterWakeupAndNoise) {
  FrameAssembler a; uint32_t t = 0;
  Bytes w = typicalResponse();
  Bytes noisy = {0x00, 0x12, 0xFF, 0xFF};
  noisy.insert(noisy.end(), w.begin(), w.end());
  size_t at = 0;
  ASSERT_TRUE(feed(a, noisy, t, &at));
  EXPECT_EQ(at, noisy.size() - 1);
  Frame f; EXPECT_EQ(parseFrame(a.data(), a.size(), f), FrameStatus::OK);
}

TEST(Assembler, BackToBackFrames) {
  FrameAssembler a; uint32_t t = 0;
  Bytes w1 = typicalResponse(false, 1200), w2 = typicalReport(1300);
  int frames = 0;
  Bytes all = w1; all.insert(all.end(), w2.begin(), w2.end());
  for (uint8_t b : all) {
    if (a.push(b, t++)) {
      Frame f; ASSERT_EQ(parseFrame(a.data(), a.size(), f), FrameStatus::OK);
      Reading r; decodeSensorData(f, DecodeOptions(), r);
      EXPECT_EQ(r.voltage_mV, frames == 0 ? 12000u : 13000u);
      frames++;
    }
  }
  EXPECT_EQ(frames, 2);
}

TEST(Assembler, ImplausibleLengthResyncsOnRealFrame) {
  FrameAssembler a; uint32_t t = 0;
  Bytes junk = {0x7E, 0x7E};                 // looks like delimiter + length high byte
  Bytes w = typicalResponse();
  Bytes stream = junk; stream.insert(stream.end(), w.begin(), w.end());
  EXPECT_TRUE(feed(a, stream, t));
  Frame f; EXPECT_EQ(parseFrame(a.data(), a.size(), f), FrameStatus::OK);
}

TEST(Assembler, OversizeLengthIsDropped) {
  FrameAssembler a; uint32_t t = 0;
  Bytes bad = {0x7E, 0xFF, 0xFF, 2, 1, 0, 0, 0};
  EXPECT_FALSE(feed(a, bad, t));
  EXPECT_GE(a.framingErrors(), 1u);
}

TEST(Assembler, StalledFrameTimesOutAndRecovers) {
  FrameAssembler a;
  Bytes w = body(typicalResponse());
  uint32_t t = 0;
  for (size_t i = 0; i < 6; i++) a.push(w[i], t++);       // partial frame
  t += 1000;                                               // probe went quiet
  bool got = false;
  for (uint8_t b : w) if (a.push(b, t++)) got = true;      // full frame afterwards
  EXPECT_TRUE(got);
  EXPECT_GE(a.framingErrors(), 1u);
  Frame f; EXPECT_EQ(parseFrame(a.data(), a.size(), f), FrameStatus::OK);
}

TEST(Assembler, MillisWraparoundDoesNotStallFrame) {
  FrameAssembler a;
  Bytes w = body(typicalResponse());
  uint32_t t = 0xFFFFFFF0u;
  bool got = false;
  for (uint8_t b : w) if (a.push(b, t++)) got = true;      // t wraps mid-frame
  EXPECT_TRUE(got);
  EXPECT_EQ(a.framingErrors(), 0u);
}

// ---------------------------------------------------------------- robustness ---------------------

TEST(Robustness, RandomBytesNeverCrashOrOverrun) {
  // Run under -fsanitize=address,undefined in CI. Deterministic PRNG so failures reproduce.
  uint32_t x = 0x12345678u;
  auto rnd = [&]() { x = x * 1664525u + 1013904223u; return (uint8_t)(x >> 24); };
  FrameAssembler a;
  uint32_t t = 0;
  uint8_t out[300];
  for (int i = 0; i < 400000; i++) {
    // bias toward delimiters and tiny lengths so the interesting paths get exercised
    uint8_t b = rnd();
    if ((b & 0x1F) == 0) b = 0x7E;
    if ((b & 0x1F) == 1) b = 0x00;
    if (a.push(b, t++)) {
      Frame f;
      if (parseFrame(a.data(), a.size(), f) == FrameStatus::OK) {
        Reading r; decodeSensorData(f, DecodeOptions(), r);
        ProvisionInfo p; decodeProvision(f, p);
        buildProvisionResponse(a.data(), a.size(), 1, out, sizeof(out));
      }
    }
  }
  SUCCEED();
}

TEST(Robustness, RandomFramesWithValidChecksumNeverCrashDecoders) {
  uint32_t x = 0xCAFEBABEu;
  auto rnd = [&]() { x = x * 1664525u + 1013904223u; return (uint8_t)(x >> 24); };
  uint8_t out[300];
  for (int i = 0; i < 60000; i++) {
    FrameSpec s;
    s.flag = rnd() & 1;
    s.hubType = (rnd() & 1) ? 3 : 1;
    s.payloadType = (rnd() & 1) ? 3 : 2;
    size_t n = rnd() % 100;
    for (size_t k = 0; k < n; k++) s.payload.push_back(rnd());
    Bytes w = encode(s);
    Bytes b = body(w);
    Frame f;
    if (parseFrame(b.data(), b.size(), f) == FrameStatus::OK) {
      Reading r; decodeSensorData(f, DecodeOptions(), r);
      ProvisionInfo p; decodeProvision(f, p);
      buildProvisionResponse(b.data(), b.size(), 1, out, sizeof(out));
    }
  }
  SUCCEED();
}
