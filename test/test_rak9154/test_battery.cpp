// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <deque>
#include <string>
#include <vector>

#include "RAK9154Battery.h"
#include "RAK9154Telemetry.h"
#include "fixtures.h"

using namespace rak9154;
using namespace fx;

static uint32_t g_now = 0;
static uint32_t clk() { return g_now; }

struct FakeTransport : Transport {
  bool beginResult = true;
  bool begun = false;
  std::deque<uint8_t> rx;
  std::vector<Bytes> tx;
  int reads = 0, services = 0;

  bool begin() override { begun = true; return beginResult; }
  void write(const uint8_t* d, size_t n) override { tx.emplace_back(d, d + n); }
  int read() override { reads++; if (rx.empty()) return -1; uint8_t b = rx.front(); rx.pop_front(); return b; }
  void service() override { services++; }
  void inject(const Bytes& wire) { rx.insert(rx.end(), wire.begin(), wire.end()); }
};

struct Rig {
  FakeTransport t;
  RAK9154Battery b;
  explicit Rig(Config c = Config()) : b(t, clk, c) { g_now = 0; }
  void run(uint32_t ms, uint32_t step = 50) {   // advance time, calling loop() like the main loop would
    for (uint32_t i = 0; i < ms; i += step) { g_now += step; b.loop(); }
  }
};

static Bytes bootFrame(uint8_t seq) { uint8_t o[16]; size_t n = buildProvisionBoot(seq, o, sizeof o); return Bytes(o, o + n); }
static Bytes getFrame(uint8_t pid, uint8_t seq) { uint8_t o[16]; size_t n = buildGetData(pid, seq, o, sizeof o); return Bytes(o, o + n); }

// Bring a rig to the normal "provisioned and polling with data" state.
static void bringUp(Rig& r) {
  ASSERT_TRUE(r.b.begin());
  r.run(3100);                                  // first poll (provision BOOT) goes out after the 3 s start delay
  ASSERT_EQ(r.t.tx.size(), 1u);
  r.t.inject(provisionAnnouncement({{0, IPSO_VOLT}, {0, IPSO_CUR}, {0, IPSO_SOC}}));
  r.run(100);                                   // driver answers the announcement
  r.run(500);                                   // then asks for data
  ASSERT_GE(r.t.tx.size(), 3u);
  r.t.inject(typicalResponse(false, 1231, 185, 94));
  r.run(100);
}

TEST(Battery, StartsWithoutDataAndReportsUnavailable) {
  Rig r;
  EXPECT_FALSE(r.b.hasValidTelemetry());
  EXPECT_EQ(r.b.getVoltageMilliVolts(), 0u);
  EXPECT_FALSE(r.b.detected());
  r.b.begin();
  EXPECT_TRUE(r.t.begun);
  EXPECT_FALSE(r.b.hasValidTelemetry());
}

TEST(Battery, FullProvisioningThenPollingFlow) {
  Rig r;
  bringUp(r);
  EXPECT_EQ(r.t.tx[0], bootFrame(1));                       // broadcast BOOT first
  Frame f; Bytes rsp(r.t.tx[1].begin() + 1, r.t.tx[1].end());
  ASSERT_EQ(parseFrame(rsp.data(), rsp.size(), f), FrameStatus::OK);
  EXPECT_EQ(f.flag, FLAG_RSP);                              // provision answered
  EXPECT_EQ(f.data[22], PID_ASSIGNED);
  EXPECT_EQ(r.t.tx[2], getFrame(PID_ASSIGNED, 2));          // then a data request to the assigned PID

  EXPECT_TRUE(r.b.hasValidTelemetry());
  EXPECT_EQ(r.b.getVoltageMilliVolts(), 12310u);
  EXPECT_EQ(r.b.getCurrentMilliAmps(), 185);
  EXPECT_EQ(r.b.getStateOfCharge(), 94);
  EXPECT_TRUE(r.b.isCharging());
  EXPECT_FALSE(r.b.hasTemperature());                       // never reported -> not invented
  EXPECT_TRUE(r.b.detected());
  EXPECT_EQ(r.b.stats().okFrames, 1u);
  EXPECT_EQ(r.b.stats().provisions, 1u);
  ASSERT_NE(r.b.provisionInfo(), nullptr);
  EXPECT_STREQ(r.b.provisionInfo()->model, "RAK9154-TEST");

  RAK9154Telemetry s = r.b.snapshot();
  EXPECT_TRUE(s.valid); EXPECT_EQ(s.voltage_mV, 12310u); EXPECT_EQ(s.stateOfCharge, 94);
}

TEST(Battery, GettersNeverTouchTheTransport) {
  Rig r;
  bringUp(r);
  const size_t tx = r.t.tx.size(); const int reads = r.t.reads;
  for (int i = 0; i < 1000; i++) {
    (void)r.b.hasValidTelemetry(); (void)r.b.getVoltageMilliVolts(); (void)r.b.getCurrentMilliAmps();
    (void)r.b.getStateOfCharge(); (void)r.b.getTemperatureC(); (void)r.b.snapshot();
  }
  EXPECT_EQ(r.t.tx.size(), tx);
  EXPECT_EQ(r.t.reads, reads);
}

TEST(Battery, PollsOncePerIntervalNotContinuously) {
  Rig r;
  bringUp(r);
  size_t before = r.t.tx.size();
  r.t.inject(typicalResponse());
  r.run(29000);                         // less than one interval since the last request
  EXPECT_EQ(r.t.tx.size(), before);
  r.run(2000);
  EXPECT_EQ(r.t.tx.size(), before + 1); // exactly one request for the next interval
  r.run(10000);
  EXPECT_EQ(r.t.tx.size(), before + 1);
}

TEST(Battery, PollIntervalIsClamped) {
  Rig r;
  r.b.setPollIntervalMs(1000);   EXPECT_EQ(r.b.getPollIntervalMs(), 15000u);
  r.b.setPollIntervalMs(600000); EXPECT_EQ(r.b.getPollIntervalMs(), 60000u);
  r.b.setPollIntervalMs(45000);  EXPECT_EQ(r.b.getPollIntervalMs(), 45000u);
  Config c; c.pollIntervalMs = 100;
  Rig r2(c);
  EXPECT_EQ(r2.b.getPollIntervalMs(), 15000u);
}

TEST(Battery, DataGoesStaleAfterThreshold) {
  Rig r;
  bringUp(r);
  ASSERT_TRUE(r.b.hasValidTelemetry());
  uint32_t t0 = g_now;
  g_now = t0 + 299000;  EXPECT_TRUE(r.b.hasValidTelemetry());
  g_now = t0 + 300500;  // loop() not even called: staleness is evaluated against the clock on every read
  EXPECT_FALSE(r.b.hasValidTelemetry());
  EXPECT_EQ(r.b.getVoltageMilliVolts(), 0u);
  EXPECT_FALSE(r.b.hasCurrent());
  EXPECT_TRUE(r.b.detected());          // "was seen once" is different from "currently valid"
}

TEST(Battery, DisconnectedProbeTimesOutAndReprovisions) {
  Rig r;
  bringUp(r);
  r.t.tx.clear();
  r.run(320000, 100);                   // probe silent for longer than the 5 minute stale window
  EXPECT_GE(r.b.stats().timeouts, 3u);
  bool sawBoot = false;
  for (auto& f : r.t.tx) if (f.size() == 13 && f[9] == HUB_PROVISION) sawBoot = true;
  EXPECT_TRUE(sawBoot) << "should fall back to a provision BOOT after repeated timeouts";
  EXPECT_FALSE(r.b.hasValidTelemetry());
}

TEST(Battery, ProbeThatNeverAcceptsProvisioningDoesNotCauseATightLoop) {
  // Real-hardware observation: the probe answered EVERY request with a new provision announcement. The driver used to
  // answer each one 300 ms later, i.e. ~2 requests/s forever (259 in a few minutes).
  Rig r;
  ASSERT_TRUE(r.b.begin());
  size_t seen = 0, requests = 0;
  for (uint32_t ms = 0; ms < 600000; ms += 50) {
    g_now += 50;
    r.b.loop();
    while (seen < r.t.tx.size()) {                    // a REQUEST (not our provision response) -> announces itself again
      const Bytes& f = r.t.tx[seen++];
      if (f.size() == 13) { requests++; r.t.inject(provisionAnnouncement({{0, IPSO_VOLT}, {0, IPSO_CUR}, {0, IPSO_SOC}})); }
    }
  }
  EXPECT_LT(requests, 40u) << "10 minutes must not produce hundreds of requests";
  EXPECT_GT(requests, 8u) << "but it must keep trying";
  EXPECT_GE(r.b.stats().provisions, 5u);
  // and the first attempts still happen quickly so a probe that needs two tries is not slowed down
}

TEST(Battery, RepeatedAnnouncementsAreNotAnsweredWhileDataIsFresh) {
  Rig r;
  bringUp(r);                                          // announcement answered once, data flowing
  const uint32_t provBefore = r.b.stats().provisions;
  for (int i = 0; i < 5; i++) {                        // probe re-announces every ~8 s, as seen on hardware
    r.t.inject(provisionAnnouncement({{0, IPSO_VOLT}, {0, IPSO_CUR}, {0, IPSO_SOC}}));
    r.run(8000);
    r.t.inject(typicalResponse(false, 1231, 185, 94)); // data keeps arriving so telemetry stays fresh
    r.run(100);
  }
  EXPECT_EQ(r.b.stats().provisions, provBefore) << "no ~130 ms transmits while data flows";
  EXPECT_GE(r.b.stats().provSkipped, 4u);
  EXPECT_TRUE(r.b.hasValidTelemetry());
  r.t.inject(provisionAnnouncement({{0, IPSO_VOLT}, {0, IPSO_CUR}, {0, IPSO_SOC}}));
  r.run(30000);                                        // past the 60 s quiet window: answered again
  r.t.inject(provisionAnnouncement({{0, IPSO_VOLT}, {0, IPSO_CUR}, {0, IPSO_SOC}}));
  r.run(100);
  EXPECT_GT(r.b.stats().provisions, provBefore);
}

struct RecLpp {
  struct Rec { std::string kind; int ch; double v; };
  std::vector<Rec> r;
  void addPercentage(uint8_t ch, uint32_t v) { r.push_back({"pct", ch, (double)v}); }
  void addCurrent(uint8_t ch, float v) { r.push_back({"cur", ch, v}); }
  void addTemperature(uint8_t ch, float v) { r.push_back({"temp", ch, v}); }
};

TEST(Telemetry, PublishesSocCurrentAndBatteryTemperatureOnTheRightChannels) {
  Rig r;
  bringUp(r);                                          // typicalResponse: 12.31 V, 185 mA, 94 %
  RecLpp l;
  rak9154::appendTelemetry(r.b, l, 1, 3);
  ASSERT_GE(l.r.size(), 2u);
  EXPECT_EQ(l.r[0].kind, "pct"); EXPECT_EQ(l.r[0].ch, 1); EXPECT_EQ(l.r[0].v, 94);
  EXPECT_EQ(l.r[1].kind, "cur"); EXPECT_EQ(l.r[1].ch, 1); EXPECT_NEAR(l.r[1].v, 0.185, 1e-6);
  for (auto& x : l.r) if (x.kind == "temp") EXPECT_EQ(x.ch, 3);
}

TEST(Telemetry, NothingIsPublishedWithoutData) {
  Rig r;
  r.b.begin();
  RecLpp l;
  rak9154::appendTelemetry(r.b, l, 1, 2);
  EXPECT_TRUE(l.r.empty());
}

TEST(Telemetry, DischargeIsClampedToZeroBecauseLppCurrentIsUnsigned) {
  Rig r;
  bringUp(r);
  r.t.inject(typicalResponse(false, 1231, (uint16_t)-150, 94));    // -150 mA
  r.run(100);
  RecLpp l;
  rak9154::appendTelemetry(r.b, l, 1, 2);
  bool saw = false;
  for (auto& x : l.r) if (x.kind == "cur") { saw = true; EXPECT_EQ(x.v, 0.0); }
  EXPECT_TRUE(saw);
}

TEST(Telemetry, StaleValuesAreDropped) {
  Rig r;
  bringUp(r);
  r.run(400000, 1000);                                 // probe goes silent for > 5 min
  RecLpp l;
  rak9154::appendTelemetry(r.b, l, 1, 2);
  EXPECT_TRUE(l.r.empty());
}

TEST(Battery, SilenceNeverBlocksOrLoopsForever) {
  Rig r;
  r.b.begin();
  // 1 hour of main-loop iterations with no probe: must just keep ticking and sending rare requests
  r.run(3600000, 20);
  EXPECT_LE(r.t.tx.size(), 3600000u / 15000u + 2);
  EXPECT_GT(r.b.stats().timeouts, 0u);
}

TEST(Battery, UnsolicitedReportsWorkWithoutProvisioning) {
  Rig r;
  r.b.begin();
  r.run(500);
  r.t.inject(typicalReport(1180, -250, 41));
  r.run(100);
  EXPECT_TRUE(r.b.hasValidTelemetry());
  EXPECT_EQ(r.b.getVoltageMilliVolts(), 11800u);
  EXPECT_EQ(r.b.getCurrentMilliAmps(), -250);
  EXPECT_FALSE(r.b.isCharging());
}

TEST(Battery, FollowsProbeSourceAddress) {
  Rig r;
  r.b.begin();
  FrameSpec s; s.flag = 0; s.source = 0x05;
  s.payload = entry(0, IPSO_VOLT, u16(1250, true));
  r.t.inject(encode(s));
  r.run(3100);
  ASSERT_FALSE(r.t.tx.empty());
  EXPECT_EQ(r.t.tx.back()[6], 0x05);    // data request now goes to pid 5
}

TEST(Battery, CorruptFramesAreCountedAndIgnored) {
  Rig r;
  bringUp(r);
  const uint32_t v = r.b.getVoltageMilliVolts();

  FrameSpec bad; bad.payload = entry(0, IPSO_VOLT, u16(1500, false)); bad.chkAdj = 2;
  r.t.inject(encode(bad)); r.run(100);
  EXPECT_EQ(r.b.stats().badChecksums, 1u);
  EXPECT_EQ(r.b.getVoltageMilliVolts(), v);           // cache untouched

  FrameSpec trunc; trunc.payload = entry(0, IPSO_VOLT, {0x10});   // value cut short, checksum valid
  r.t.inject(encode(trunc)); r.run(100);
  EXPECT_EQ(r.b.stats().invalidFrames, 1u);
  EXPECT_EQ(r.b.getVoltageMilliVolts(), v);

  FrameSpec unk; unk.payload = cat({entry(0, 0x42, {1, 2}), entry(0, IPSO_VOLT, u16(1500, false))});
  r.t.inject(encode(unk)); r.run(100);
  EXPECT_EQ(r.b.stats().unknownChannels, 1u);
  EXPECT_EQ(r.b.getVoltageMilliVolts(), v);           // fields after an unsizable entry are not trusted

  Bytes noise; for (int i = 0; i < 300; i++) noise.push_back((uint8_t)(i * 37 + 11));
  r.t.inject(noise); r.run(500);
  EXPECT_EQ(r.b.getVoltageMilliVolts(), v);
}

TEST(Battery, WrongByteOrderIsRejectedNotCached) {
  Rig r;
  r.b.begin();
  r.run(500);
  r.t.inject(typicalResponse(true));       // big-endian bytes, but default expects little-endian for responses
  r.run(100);
  EXPECT_FALSE(r.b.hasValidTelemetry());
  EXPECT_EQ(r.b.stats().rangeRejects, 1u);

  Config c; c.decode.responseBigEndian = true;     // what the bench-derived compile flag would do
  Rig r2(c);
  r2.b.begin();
  r2.run(500);
  r2.t.inject(typicalResponse(true));
  r2.run(100);
  EXPECT_TRUE(r2.b.hasValidTelemetry());
  EXPECT_EQ(r2.b.getVoltageMilliVolts(), 12310u);
}

TEST(Battery, RxWorkPerLoopIsBounded) {
  Rig r;
  r.b.begin();
  Bytes flood(20000, 0x55);
  r.t.inject(flood);
  g_now += 50;
  int before = r.t.reads;
  r.b.loop();
  EXPECT_LE(r.t.reads - before, 257);      // one iteration never swallows an unbounded backlog
  EXPECT_GE(r.t.services, 1);
}

TEST(Battery, TransportFailureLeavesDriverInert) {
  Rig r;
  r.t.beginResult = false;
  EXPECT_FALSE(r.b.begin());
  r.run(120000);
  EXPECT_TRUE(r.t.tx.empty());
  EXPECT_FALSE(r.b.hasValidTelemetry());
  char buf[160];
  r.b.formatStatus(buf, sizeof buf);
  EXPECT_NE(std::string(buf).find("NO-PIN"), std::string::npos);
}

TEST(Battery, RequestPollNowIsRateLimited) {
  Rig r;
  bringUp(r);
  r.t.inject(typicalResponse());
  r.run(100);
  size_t n0 = r.t.tx.size();
  r.b.requestPollNow();
  r.run(100);
  EXPECT_EQ(r.t.tx.size(), n0 + 1);
  r.t.inject(typicalResponse()); r.run(100);
  r.b.requestPollNow(); r.b.requestPollNow();     // spam
  r.run(100);
  EXPECT_LE(r.t.tx.size(), n0 + 2);
}

TEST(Battery, MillisWraparound) {
  Rig r;
  ASSERT_TRUE(r.b.begin());
  g_now = 0xFFFFFFFFu - 4000;             // 4 s before millis() wraps
  r.b.begin();
  // re-create so the first poll is scheduled relative to the late start
  FakeTransport t2; RAK9154Battery b2(t2, clk);
  b2.begin();
  for (int i = 0; i < 200; i++) { g_now += 50; b2.loop(); }   // crosses the wrap
  EXPECT_GE(t2.tx.size(), 1u);
  t2.inject(typicalReport());
  for (int i = 0; i < 4; i++) { g_now += 50; b2.loop(); }
  EXPECT_TRUE(b2.hasValidTelemetry());
  g_now += 200000;                         // still fresh after crossing wrap
  EXPECT_TRUE(b2.hasValidTelemetry());
  g_now += 200000;
  EXPECT_FALSE(b2.hasValidTelemetry());
}

TEST(Battery, TemperatureIsExposedOnlyWhenReported) {
  Rig r;
  r.b.begin(); r.run(500);
  FrameSpec s; s.flag = 0;
  s.payload = cat({entry(0, IPSO_VOLT, u16(1231, true)), entry(0, IPSO_TEMP, u16((uint16_t)-52, true))});
  r.t.inject(encode(s)); r.run(100);
  EXPECT_TRUE(r.b.hasTemperature());
  EXPECT_FLOAT_EQ(r.b.getTemperatureC(), -5.2f);
}

TEST(BatteryText, StatusInfoRawAreBoundedAndSensible) {
  Rig r;
  bringUp(r);
  char buf[256];
  size_t n = r.b.formatStatus(buf, sizeof buf);
  EXPECT_EQ(n, strlen(buf));
  std::string st(buf);
  EXPECT_NE(st.find("V:12.31V"), std::string::npos) << st;
  EXPECT_NE(st.find("I:185mA"), std::string::npos) << st;
  EXPECT_NE(st.find("SOC:94%"), std::string::npos) << st;
  EXPECT_LT(n, 150u) << "must fit the 160 byte CLI reply buffer";

  n = r.b.formatInfo(buf, sizeof buf);
  st = buf;
  EXPECT_NE(st.find("RAK9154-TEST"), std::string::npos) << st;
  EXPECT_NE(st.find("BA B9 B8"), std::string::npos) << st;

  n = r.b.formatRaw(buf, 160);
  EXPECT_LT(n, 160u);
  EXPECT_NE(std::string(buf).find("7E"), std::string::npos);

  n = r.b.formatExtra(buf, sizeof buf);
  EXPECT_LT(n, 150u);
  EXPECT_NE(std::string(buf).find("0xF3"), std::string::npos) << buf;   // none received in this fixture

  n = r.b.formatDiag(buf, sizeof buf);
  st = buf;
  EXPECT_LT(n, 150u);
  EXPECT_NE(st.find("rxfr:"), std::string::npos) << st;
  EXPECT_NE(st.find("req:"), std::string::npos) << st;
  EXPECT_GT(r.b.stats().rxBytes, 0u);
  EXPECT_GT(r.b.stats().rxFrames, 0u);

  // paging through a captured frame: offset moves the window, a far offset is reported, not crashed on
  n = r.b.formatRaw(buf, 160, 5);
  EXPECT_NE(std::string(buf).find("+5"), std::string::npos) << buf;
  r.b.formatRaw(buf, 160, 5000);
  EXPECT_NE(std::string(buf).find("past the end"), std::string::npos) << buf;

  // every capacity, including degenerate ones, stays inside its buffer (run under ASan)
  for (size_t cap = 0; cap < 200; cap++) {
    std::vector<char> v(cap + 8, 'x');
    size_t a = r.b.formatStatus(v.data(), cap);
    size_t b2 = r.b.formatInfo(v.data(), cap);
    size_t c = r.b.formatRaw(v.data(), cap);
    size_t d = r.b.formatDiag(v.data(), cap);
    size_t e = r.b.formatRaw(v.data(), cap, 20);
    size_t x = r.b.formatExtra(v.data(), cap);
    if (cap > 0) { EXPECT_LT(a, cap); EXPECT_LT(b2, cap); EXPECT_LT(c, cap); EXPECT_LT(d, cap); EXPECT_LT(e, cap); EXPECT_LT(x, cap); }
    else { EXPECT_EQ(a, 0u); EXPECT_EQ(b2, 0u); EXPECT_EQ(c, 0u); EXPECT_EQ(d, 0u); EXPECT_EQ(e, 0u); EXPECT_EQ(x, 0u); }
    for (size_t i = cap; i < v.size(); i++) EXPECT_EQ(v[i], 'x') << "overrun at cap " << cap;
  }
}

TEST(BatteryText, StaleAndNoDataLabels) {
  Rig r;
  char buf[160];
  r.b.begin();
  r.b.formatStatus(buf, sizeof buf);
  EXPECT_NE(std::string(buf).find("no-data"), std::string::npos) << buf;
  Rig r2;
  bringUp(r2);
  g_now += 400000;
  r2.b.formatStatus(buf, sizeof buf);
  EXPECT_NE(std::string(buf).find("STALE"), std::string::npos) << buf;
}
