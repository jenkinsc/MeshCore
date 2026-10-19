// SPDX-License-Identifier: MIT
//
// Runs the real RAK9154HalfSerial.cpp against a simulated cycle counter and pin. Time advances 4 cycles every time
// the code reads the counter or the pin (roughly a few instructions), which is enough to validate the bit timing
// arithmetic, the framing, the guards and the attach/detach sequencing. It does NOT replace a scope check on
// real hardware (interrupt latency, SoftDevice/USB jitter, electrical levels).
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "Arduino.h"
#include "nrf_gpio.h"
#include "RAK9154HalfSerial.h"

// ---------------------------------------------------------------- simulated world ---------------------
namespace sim {
uint32_t cyc = 0;
bool frozen = false;
uint32_t millisNow = 0;
struct Edge { uint32_t t; int level; };
std::vector<Edge> rxWave;      // what the probe drives onto the pin (initially high)
std::vector<Edge> txLog;       // what the driver drives
std::vector<std::string> events;
voidFuncPtr isr = nullptr;
bool attached = false;
int attachFails = 0;
bool outputMode = false;

int levelAt(const std::vector<Edge>& w, uint32_t t) {
  int lv = 1;
  for (auto& e : w) { if ((int32_t)(t - e.t) >= 0) lv = e.level; else break; }
  return lv;
}
void reset() {
  cyc = 0; frozen = false; millisNow = 0; rxWave.clear(); txLog.clear(); events.clear();
  isr = nullptr; attached = false; attachFails = 0; outputMode = false;
}
// UART 8N1 waveform for one byte whose start bit begins at `t0`.
void addByte(uint32_t t0, uint8_t v, uint32_t bit, bool goodStop = true) {
  rxWave.push_back({t0, 0});
  for (int i = 0; i < 8; i++) rxWave.push_back({t0 + bit * (1 + i), (v >> i) & 1});
  rxWave.push_back({t0 + bit * 9, goodStop ? 1 : 0});
  if (!goodStop) rxWave.push_back({t0 + bit * 10, 1});
}
}  // namespace sim

const uint32_t g_ADigitalPinMap[PINS_COUNT] = {
  0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
  32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47};
uint32_t SystemCoreClock = 64000000;
FakeCoreDebug g_fake_coredebug;
FakeDWT* fake_dwt() {
  static FakeDWT d;
  if (!sim::frozen) sim::cyc += 4;
  d.CYCCNT = sim::cyc;
  return &d;
}
void pinMode(uint32_t, uint32_t) { sim::events.push_back("pinMode"); }
int attachInterrupt(uint32_t, voidFuncPtr cb, uint32_t) {
  if (sim::attachFails > 0) { sim::attachFails--; return 0; }
  sim::isr = cb; sim::attached = true; sim::events.push_back("attach"); return 1;
}
void detachInterrupt(uint32_t) { sim::attached = false; sim::events.push_back("detach"); }
unsigned long millis() { return sim::millisNow; }
uint32_t nrf_gpio_pin_read(uint32_t) { sim::cyc += 4; return (uint32_t)sim::levelAt(sim::rxWave, sim::cyc); }
void nrf_gpio_pin_set(uint32_t) { sim::txLog.push_back({sim::cyc, 1}); }
void nrf_gpio_pin_clear(uint32_t) { sim::txLog.push_back({sim::cyc, 0}); }
void nrf_gpio_pin_write(uint32_t, uint32_t v) { sim::txLog.push_back({sim::cyc, (int)v}); }
void nrf_gpio_cfg_output(uint32_t) { sim::outputMode = true; sim::events.push_back("output"); }
void nrf_gpio_cfg_input(uint32_t, int) { sim::outputMode = false; sim::events.push_back("input"); }

// ---------------------------------------------------------------- helpers ---------------------------
static const uint32_t BIT = 64000000 / 9600;   // 6666 cycles

struct Rig {
  RAK9154HalfSerial hs{15};
  Rig() { sim::reset(); }
  bool up() { return hs.begin(); }
  // Deliver one byte the way the hardware does: ISR starts `latency` cycles after the falling edge.
  void receiveByte(uint8_t v, uint32_t latency, uint32_t edgeAt, bool goodStop = true) {
    sim::addByte(edgeAt, v, BIT, goodStop);
    sim::cyc = edgeAt + latency;
    ASSERT_NE(sim::isr, nullptr);
    sim::isr();
  }
};

// ---------------------------------------------------------------- RX ---------------------------------
TEST(HalfSerialRx, DecodesEveryByteValueAcrossInterruptLatencies) {
  for (uint32_t latency : {0u, 100u, 316u, 320u, 600u, 1500u, 2500u}) {
    Rig r; ASSERT_TRUE(r.up());
    uint32_t edge = 100000;
    for (int v = 0; v < 256; v++) {
      r.receiveByte((uint8_t)v, latency, edge);
      EXPECT_EQ(r.hs.read(), v) << "latency " << latency;
      edge += 20 * BIT;
    }
    EXPECT_EQ(r.hs.read(), -1);
    EXPECT_EQ(r.hs.framingErrorCount(), 0u);
  }
}

TEST(HalfSerialRx, BackToBackBytes) {
  Rig r; ASSERT_TRUE(r.up());
  uint32_t edge = 50000;
  const uint8_t msg[] = {0xFF, 0x7E, 0x00, 0x06, 0xAA, 0x55};
  for (uint8_t b : msg) { r.receiveByte(b, 320, edge); edge += 10 * BIT; }     // no gap at all between frames bytes
  for (uint8_t b : msg) EXPECT_EQ(r.hs.read(), b);
}

TEST(HalfSerialRx, SmallBaudMismatchStillDecodes) {
  // probe clock 2 % fast and 2 % slow (cheap RC oscillators exist on probe MCUs)
  for (double scale : {0.98, 1.02}) {
    Rig r; ASSERT_TRUE(r.up());
    uint32_t pbit = (uint32_t)(BIT * scale);
    uint32_t edge = 80000;
    for (int v : {0x00, 0xFF, 0x5A, 0xA5, 0x7E, 0x81}) {
      sim::addByte(edge, (uint8_t)v, pbit);
      sim::cyc = edge + 320;
      sim::isr();
      EXPECT_EQ(r.hs.read(), v) << "scale " << scale;
      edge += 20 * BIT;
    }
  }
}

TEST(HalfSerialRx, GlitchWithLineAlreadyHighIsIgnored) {
  Rig r; ASSERT_TRUE(r.up());
  sim::cyc = 1000;                       // waveform is idle high: ISR fired on a runt pulse that already ended
  sim::isr();
  EXPECT_EQ(r.hs.read(), -1);
}

TEST(HalfSerialRx, BadStopBitIsDroppedAndCounted) {
  Rig r; ASSERT_TRUE(r.up());
  r.receiveByte(0x42, 320, 100000, /*goodStop=*/false);
  EXPECT_EQ(r.hs.read(), -1);
  EXPECT_EQ(r.hs.framingErrorCount(), 1u);
}

TEST(HalfSerialRx, RingOverflowDropsNewestAndCounts) {
  Rig r; ASSERT_TRUE(r.up());
  uint32_t edge = 100000;
  // keep each byte in its own 100 ms accounting window so the storm guard stays out of this test
  for (int i = 0; i < 140; i++) { r.receiveByte((uint8_t)i, 320, edge); edge += 7000000; }
  EXPECT_GT(r.hs.overflowCount(), 0u);
  int n = 0, first = r.hs.read();
  EXPECT_EQ(first, 0);                   // oldest data preserved
  while (r.hs.read() >= 0) n++;
  EXPECT_EQ(n + 1, 127);                 // 128-slot ring holds 127
}

TEST(HalfSerialRx, NoiseStormSuspendsReceptionThenRecovers) {
  Rig r; ASSERT_TRUE(r.up());
  // A flood of runt pulses (line already high when the ISR looks) well above what 9600 baud can deliver
  sim::cyc = 100000;
  for (int i = 0; i < 150; i++) { sim::isr(); sim::cyc += 500; }
  EXPECT_EQ(r.hs.stormCount(), 1u);
  r.receiveByte(0x11, 320, 400000);
  EXPECT_EQ(r.hs.read(), -1);            // suspended
  sim::millisNow = 1000; r.hs.service(); // starts the pause
  sim::millisNow = 3000; r.hs.service();
  r.receiveByte(0x22, 320, 800000);
  EXPECT_EQ(r.hs.read(), -1);            // 2 s later: still paused
  sim::millisNow = 7000; r.hs.service();
  r.receiveByte(0x33, 320, 1200000);
  EXPECT_EQ(r.hs.read(), 0x33);          // after the pause reception is back
}

// ---------------------------------------------------------------- TX ---------------------------------
// Independent decoder of the driver's output waveform.
static std::vector<uint8_t> decodeTx(const std::vector<sim::Edge>& log, uint32_t bit, uint32_t* maxBitErr = nullptr) {
  std::vector<uint8_t> out;
  if (maxBitErr) *maxBitErr = 0;
  size_t i = 0;
  while (i < log.size()) {
    while (i < log.size() && !(log[i].level == 0 && (i == 0 || sim::levelAt(log, log[i].t - 1) == 1))) i++;
    if (i >= log.size()) break;
    const uint32_t start = log[i].t;
    uint8_t v = 0;
    for (int b = 0; b < 8; b++) if (sim::levelAt(log, start + bit * (b + 1) + bit / 2)) v |= (uint8_t)(1u << b);
    EXPECT_EQ(sim::levelAt(log, start + bit * 9 + bit / 2), 1) << "stop bit must be high";
    out.push_back(v);
    // every transition must sit on a bit boundary of this byte
    for (auto& e : log) {
      if (e.t < start || e.t > start + bit * 10) continue;
      uint32_t rel = e.t - start, rem = rel % bit, err = std::min(rem, bit - rem);
      if (maxBitErr && err > *maxBitErr) *maxBitErr = err;
    }
    i++;
    while (i < log.size() && log[i].t < start + bit * 10) i++;
  }
  return out;
}

TEST(HalfSerialTx, WaveformDecodesToTheSentBytesWithCorrectBitTiming) {
  Rig r; ASSERT_TRUE(r.up());
  const uint8_t msg[] = {0xFF, 0x7E, 0x00, 0x06, 0x02, 0x00, 0xFF, 0x00, 0x01, 0x01, 0x00, 0x02, 0x0C};
  sim::txLog.clear();
  r.hs.write(msg, sizeof msg);
  uint32_t err = 0;
  auto got = decodeTx(sim::txLog, BIT, &err);
  ASSERT_EQ(got.size(), sizeof msg);
  for (size_t i = 0; i < sizeof msg; i++) EXPECT_EQ(got[i], msg[i]) << i;
  EXPECT_LE(err, 24u) << "bit edges must be within ~0.4% of ideal (cycles)";
}

TEST(HalfSerialTx, FrameDurationMatches9600Baud) {
  Rig r; ASSERT_TRUE(r.up());
  const uint8_t msg[13] = {0xFF, 0x7E};
  uint32_t t0 = sim::cyc;
  r.hs.write(msg, sizeof msg);
  double ms = (double)(sim::cyc - t0) / 64000.0;
  EXPECT_NEAR(ms, 13 * 10 * 1000.0 / 9600 + 2 * 1000.0 / 9600, 0.6);   // 13 bytes + idle/settle bits
  EXPECT_LT(ms, 16.0);
}

TEST(HalfSerialTx, ReceiverIsDetachedWhileTransmittingAndRearmedAfter) {
  Rig r; ASSERT_TRUE(r.up());
  sim::events.clear();
  const uint8_t b = 0x55;
  r.hs.write(&b, 1);
  ASSERT_GE(sim::events.size(), 4u);
  EXPECT_EQ(sim::events.front(), "detach");
  EXPECT_EQ(sim::events[1], "output");
  EXPECT_EQ(sim::events[sim::events.size() - 2], "input");
  EXPECT_EQ(sim::events.back(), "attach");
  EXPECT_TRUE(sim::attached);
  EXPECT_FALSE(sim::outputMode);          // line released to the probe
  EXPECT_EQ(sim::txLog.front().level, 1); // idle level is applied before the output is enabled: no glitch
}

TEST(HalfSerialTx, IgnoresEmptyOrNullWrites) {
  Rig r; ASSERT_TRUE(r.up());
  sim::events.clear();
  r.hs.write(nullptr, 5);
  uint8_t b = 1; r.hs.write(&b, 0);
  EXPECT_TRUE(sim::events.empty());
}

// ---------------------------------------------------------------- failure handling -----------------------
TEST(HalfSerialInit, FailsCleanlyWhenTimerIsDeadOrNoInterruptChannel) {
  { Rig r; sim::frozen = true; EXPECT_FALSE(r.up()); }          // cycle counter not running
  { Rig r; sim::attachFails = 1; EXPECT_FALSE(r.up()); }        // GPIOTE channels exhausted
  { Rig r; RAK9154HalfSerial bad(200); EXPECT_FALSE(bad.begin()); }   // pin outside the board's range
}

TEST(HalfSerialInit, StuckCycleCounterCannotHangTransmit) {
  Rig r; ASSERT_TRUE(r.up());
  sim::frozen = true;                    // counter dies after init: busy-waits must still end
  const uint8_t msg[4] = {1, 2, 3, 4};
  r.hs.write(msg, sizeof msg);           // returns (bounded spin) instead of hanging the node
  SUCCEED();
}

TEST(HalfSerialInit, ReattachFailureMakesDriverInertNotBroken) {
  Rig r; ASSERT_TRUE(r.up());
  sim::attachFails = 1;
  const uint8_t b = 0x55;
  r.hs.write(&b, 1);                     // cannot re-arm the receiver afterwards
  sim::events.clear();
  r.hs.write(&b, 1);                     // now inert: no pin activity at all
  EXPECT_TRUE(sim::events.empty());
  EXPECT_EQ(r.hs.read(), -1);
}

TEST(HalfSerialInit, SecondInstanceCannotStealTheInterrupt) {
  Rig r; ASSERT_TRUE(r.up());
  RAK9154HalfSerial other(15);
  EXPECT_FALSE(other.begin());
}
