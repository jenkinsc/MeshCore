// SPDX-License-Identifier: MIT
#if defined(RAK9154_BATTERY) || defined(RAK9154_HOST_TEST)

#include "RAK9154Battery.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

using namespace rak9154;

namespace {

constexpr uint32_t MIN_POLL_MS        = 15000;
constexpr uint32_t MAX_POLL_MS        = 60000;
constexpr uint32_t FORCE_MIN_GAP_MS   = 1000;
constexpr uint32_t PROV_QUIET_MS      = 60000;   // while data is fresh, answer a repeated announcement at most this often
constexpr uint32_t POST_PROVISION_MS  = 300;     // ask for data shortly after answering a provision announcement
constexpr size_t   MAX_RX_PER_LOOP    = 256;     // bound on work done per loop() iteration
constexpr int32_t  CHARGING_MIN_mA    = 20;      // below this we do not claim "charging" (noise floor guess)

#ifdef RAK9154_DEBUG
void logHex(const char* tag, const uint8_t* d, size_t n) {
  char line[3 * MAX_TX_FRAME + 1];
  size_t o = 0;
  for (size_t i = 0; i < n && o + 4 < sizeof(line); i++) o += snprintf(&line[o], sizeof(line) - o, "%02X ", d[i]);
  line[o] = 0;
  RAK9154_LOG("%s: %s", tag, line);
}
#else
inline void logHex(const char*, const uint8_t*, size_t) {}
#endif

// Bounded printf appender for the CLI strings.
struct Out {
  char* p; size_t left;
  Out(char* b, size_t cap) : p(b), left(cap) { if (cap) b[0] = 0; }
  void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (left < 2) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(p, left, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t w = (size_t)n < left - 1 ? (size_t)n : left - 1;
    p += w; left -= w;
  }
  size_t used(const char* base) const { return (size_t)(p - base); }
};

}  // namespace

RAK9154Battery::RAK9154Battery(Transport& transport, ClockFn clock, const Config& cfg)
    : _t(transport), _clock(clock), _cfg(cfg) {
  setPollIntervalMs(_cfg.pollIntervalMs);   // clamp whatever was configured
}

bool RAK9154Battery::begin() {
  if (_begun) return _transportOk;
  _begun = true;
  _transportOk = _t.begin();
  _nextPollAt = _clock() + _cfg.firstPollDelayMs;
  if (!_transportOk) RAK9154_LOG("transport init failed; driver inert");
  return _transportOk;
}

void RAK9154Battery::setPollIntervalMs(uint32_t ms) {
  if (ms < MIN_POLL_MS) ms = MIN_POLL_MS;
  if (ms > MAX_POLL_MS) ms = MAX_POLL_MS;
  _cfg.pollIntervalMs = ms;
}

void RAK9154Battery::requestPollNow() {
  const uint32_t now = _clock();
  if (_haveForced && (uint32_t)(now - _lastForced) < FORCE_MIN_GAP_MS) return;
  _haveForced = true;
  _lastForced = now;
  _nextPollAt = now;
}

void RAK9154Battery::loop() {
  if (!_begun || !_transportOk) return;
  const uint32_t now = _clock();

  _t.service();
  drainRx(now);

  if (_awaiting && (uint32_t)(now - _sentAt) >= _cfg.responseTimeoutMs) {
    _awaiting = false;
    _stats.timeouts++;
    RAK9154_LOG("timeout (%u in a row)", (unsigned)_consecFail + 1u);
    if (_consecFail < 255) _consecFail++;
    if (_consecFail >= _cfg.reprovisionAfter && _state == ST_POLLING) {
      _state = ST_PROVISIONING;      // probe may have restarted and lost its PID: introduce ourselves again
      _consecFail = 0;
    }
  }

  if (!_awaiting && (int32_t)(now - _nextPollAt) >= 0) sendRequest(now);
}

// ---- transmit ----------------------------------------------------------------------------------

void RAK9154Battery::transmit(const uint8_t* buf, size_t len) {
  logHex("TX", buf, len);
  const uint32_t t0 = _clock();
  _t.write(buf, len);
  const uint32_t dt = (uint32_t)(_clock() - t0);
  if (dt > _stats.maxTxMs) _stats.maxTxMs = dt;
}

void RAK9154Battery::sendRequest(uint32_t now) {
  uint8_t buf[MAX_TX_FRAME];
  size_t n = (_state == ST_PROVISIONING) ? buildProvisionBoot(++_seq, buf, sizeof(buf))
                                         : buildGetData(_pid, ++_seq, buf, sizeof(buf));
  _nextPollAt = now + _cfg.pollIntervalMs;
  if (n == 0) return;
  transmit(buf, n);
  _stats.requestsSent++;
  _awaiting = true;
  _sentAt = _clock();
}

// ---- receive -----------------------------------------------------------------------------------

void RAK9154Battery::drainRx(uint32_t now) {
  size_t count = 0;
  int c;
  while (count < MAX_RX_PER_LOOP && (c = _t.read()) >= 0) {
    count++;
    _stats.rxBytes++;
    if (_asm.push((uint8_t)c, now)) {
      handleCandidate(_asm.data(), _asm.size(), now);
      _asm.clearFrame();
    }
  }
  _stats.framingErrors = _asm.framingErrors();
}

void RAK9154Battery::handleCandidate(const uint8_t* data, size_t len, uint32_t now) {
  _stats.rxFrames++;
#if RAK9154_RAW_CAPTURE
  _rawLen = len < sizeof(_raw) ? len : sizeof(_raw);
  memcpy(_raw, data, _rawLen);
  _rawAt = now;
#endif
  logHex("RX", data, len);

  Frame f;
  const FrameStatus st = parseFrame(data, len, f);
  if (st == FrameStatus::BAD_CHECKSUM) { _stats.badChecksums++; RAK9154_LOG("bad checksum"); return; }
  if (st != FrameStatus::OK)           { _stats.invalidFrames++; RAK9154_LOG("invalid frame (%u)", (unsigned)st); return; }
  RAK9154_LOG("checksum OK");

  if (f.flag == FLAG_REQ && f.hubType == HUB_PROVISION && f.payloadType == PROV_VER3) {
    // Real probes keep announcing themselves every few seconds even while they deliver data. Each answer is a ~130 ms
    // blocking transmit, so once data flows only answer occasionally.
    if (_haveAnswered && hasValidTelemetry() && (uint32_t)(now - _lastProvAnswer) < PROV_QUIET_MS) {
      _stats.provSkipped++;
      return;
    }
    ProvisionInfo info;
    uint8_t out[MAX_TX_FRAME];
    size_t n = decodeProvision(f, info) ? buildProvisionResponse(data, len, PID_ASSIGNED, out, sizeof(out)) : 0;
    if (n == 0) { _stats.invalidFrames++; return; }
    _prov = info;
    _haveProv = true;
    transmit(out, n);                 // ~85-130 ms transmit, see header comment
    _haveAnswered = true;
    _lastProvAnswer = _clock();
    _pid = PID_ASSIGNED;
    _state = ST_POLLING;
    _awaiting = false;
    _consecFail = 0;
    _stats.provisions++;
    // A probe that keeps announcing itself after we answered did not accept the answer. Do not hammer it: ask for
    // data quickly the first few times, then back off (2 s, 4 s, ... up to the poll interval).
    if (_provStreak < 0xFFFF) _provStreak++;
    uint32_t wait = POST_PROVISION_MS;
    if (_provStreak > 3) {
      const uint32_t shift = _provStreak - 4 > 5 ? 5 : (uint32_t)(_provStreak - 4);
      wait = 2000UL << shift;
      if (wait > _cfg.pollIntervalMs) wait = _cfg.pollIntervalMs;
    }
    _nextPollAt = _clock() + wait;
    return;
  }

  if (f.hubType == HUB_SENDDATA) {
    if (f.dest != PID_MASTER && f.dest != PID_UNKNOWN) { _stats.otherFrames++; return; }   // addressed to someone else
    Reading r;
    const DecodeStatus ds = decodeSensorData(f, _cfg.decode, r);
    _stats.unknownChannels += r.unknownIds;
    _stats.rangeRejects    += r.rejected;
    if (ds != DecodeStatus::OK) { _stats.invalidFrames++; RAK9154_LOG("malformed sensor payload"); return; }
    if (f.flag == FLAG_RSP) _awaiting = false;
    applyReading(f, r, now);
    return;
  }

  _stats.otherFrames++;
}

void RAK9154Battery::applyReading(const Frame& f, const Reading& r, uint32_t now) {
  bool any = false;
  if (r.hasVoltage) { _haveVolt = true; _voltage_mV = r.voltage_mV; _voltageAt = now; any = true; }
  if (r.hasCurrent) { _haveCur  = true; _current_mA = r.current_mA; _currentAt = now; any = true; }
  if (r.hasSoc)     { _haveSoc  = true; _soc = r.soc;               _socAt     = now; any = true; }
  if (r.hasTemp)    { _haveTemp = true; _temp_dC = r.temp_decideg;  _tempAt    = now; any = true; }
  if (r.statusCount) {
    _extraCount = r.statusCount;
    for (uint8_t i = 0; i < r.statusCount; i++) { _extraCh[i] = r.statusChannel[i]; _extraVal[i] = r.statusValue[i]; }
    _extraAt = now;
  }
  if (!any) return;

  _stats.okFrames++;
  _provStreak = 0;
  _everGood = true;
  _consecFail = 0;
  if (f.source != PID_MASTER && f.source != PID_UNKNOWN) _pid = f.source;   // follow the probe's own address
  if (_state == ST_PROVISIONING) _state = ST_POLLING;                       // it talks without needing provisioning
  if (r.hasVoltage) RAK9154_LOG("voltage: %u mV", (unsigned)r.voltage_mV);
  if (r.hasCurrent) RAK9154_LOG("current: %d mA", (int)r.current_mA);
  if (r.hasSoc)     RAK9154_LOG("soc: %u", (unsigned)r.soc);
  if (r.hasTemp)    RAK9154_LOG("temperature: %d.%d C", r.temp_decideg / 10, abs(r.temp_decideg % 10));
}

// ---- cached accessors --------------------------------------------------------------------------

bool RAK9154Battery::hasValidTelemetry() const {
  return _haveVolt && fresh(_clock(), _voltageAt, _cfg.staleMs);
}
uint32_t RAK9154Battery::getVoltageMilliVolts() const { return hasValidTelemetry() ? _voltage_mV : 0; }

bool    RAK9154Battery::hasCurrent() const { return _haveCur && fresh(_clock(), _currentAt, _cfg.staleMs); }
int32_t RAK9154Battery::getCurrentMilliAmps() const { return hasCurrent() ? _current_mA : 0; }
bool    RAK9154Battery::hasStateOfCharge() const { return _haveSoc && fresh(_clock(), _socAt, _cfg.staleMs); }
uint8_t RAK9154Battery::getStateOfCharge() const { return hasStateOfCharge() ? _soc : 0; }
bool    RAK9154Battery::hasTemperature() const { return _haveTemp && fresh(_clock(), _tempAt, _cfg.staleMs); }
float   RAK9154Battery::getTemperatureC() const { return hasTemperature() ? (float)_temp_dC / 10.0f : 0.0f; }
// A 0xF3 word is the fault word when it is zero or uses only the documented fault bits 6..11; anything else is
// the firmware version (e.g. 0x0004 = V00.04). The same classification drives "rak9154 extra".
bool RAK9154Battery::hasFaultStatus() const {
  if (_extraCount == 0 || !fresh(_clock(), _extraAt, _cfg.staleMs)) return false;
  for (uint8_t i = 0; i < _extraCount; i++)
    if ((_extraVal[i] & ~0x0FC0u) == 0) return true;
  return false;
}
uint8_t RAK9154Battery::getFaultBits() const {
  if (!hasFaultStatus()) return 0;
  for (uint8_t i = 0; i < _extraCount; i++)
    if ((_extraVal[i] & ~0x0FC0u) == 0) return (uint8_t)(_extraVal[i] >> 6);
  return 0;
}
bool    RAK9154Battery::isCharging() const { return hasCurrent() && _current_mA > CHARGING_MIN_mA; }

RAK9154Telemetry RAK9154Battery::snapshot() const {
  RAK9154Telemetry t;
  t.valid         = hasValidTelemetry();
  t.voltage_mV    = getVoltageMilliVolts();
  t.hasCurrent    = hasCurrent();       t.current_mA    = getCurrentMilliAmps();
  t.hasSoc        = hasStateOfCharge(); t.stateOfCharge = getStateOfCharge();
  t.hasTemp       = hasTemperature();   t.temperatureC  = getTemperatureC();
  t.lastUpdateMs  = _voltageAt;
  return t;
}

// ---- CLI text ----------------------------------------------------------------------------------

size_t RAK9154Battery::formatStatus(char* out, size_t cap) const {
  if (cap == 0) return 0;
  Out o(out, cap);
  const uint32_t now = _clock();
  const bool valid = hasValidTelemetry();
  o.add("RAK9154 %s", !_transportOk ? "NO-PIN" : (valid ? "ok" : (_everGood ? "STALE" : "no-data")));
  if (valid) o.add(" V:%u.%02uV", (unsigned)(_voltage_mV / 1000), (unsigned)((_voltage_mV % 1000) / 10));
  if (hasCurrent()) o.add(" I:%ldmA", (long)_current_mA);
  if (hasStateOfCharge()) o.add(" SOC:%u%%", (unsigned)_soc);
  if (hasTemperature()) o.add(" T:%d.%dC", _temp_dC / 10, abs(_temp_dC % 10));
  if (_haveVolt) o.add(" age:%us", (unsigned)((uint32_t)(now - _voltageAt) / 1000));
  o.add(" ok:%u to:%u cs:%u inv:%u unk:%u rng:%u tx:%ums %s",
        (unsigned)_stats.okFrames, (unsigned)_stats.timeouts, (unsigned)_stats.badChecksums,
        (unsigned)(_stats.invalidFrames + _stats.framingErrors), (unsigned)_stats.unknownChannels,
        (unsigned)_stats.rangeRejects, (unsigned)_stats.maxTxMs, _state == ST_POLLING ? "POLL" : "PROV");
  return o.used(out);
}

size_t RAK9154Battery::formatInfo(char* out, size_t cap) const {
  if (cap == 0) return 0;
  Out o(out, cap);
  if (!_haveProv) {
    o.add("RAK9154 no provision announcement seen yet (pid:%u, %s)", (unsigned)_pid,
          _state == ST_POLLING ? "POLL" : "PROV");
  } else {
    o.add("RAK9154 model:%s hw:%u sw:%u.%u.%u pid:%u sensors(%u):", _prov.model, (unsigned)_prov.hw,
          (unsigned)_prov.sw[0], (unsigned)_prov.sw[1], (unsigned)_prov.sw[2], (unsigned)_pid,
          (unsigned)_prov.sensorCount);
    for (uint8_t i = 0; i < _prov.sensorCount && i < sizeof(_prov.ipso); i++) o.add(" %02X", (unsigned)_prov.ipso[i]);
  }
  return o.used(out);
}

size_t RAK9154Battery::formatExtra(char* out, size_t cap) const {
  if (cap == 0) return 0;
  Out o(out, cap);
  if (_extraCount == 0) {
    o.add("RAK9154 no 0xF3 battery status words received yet");
  } else {
    o.add("RAK9154 %us ago:", (unsigned)((uint32_t)(_clock() - _extraAt) / 1000));
    for (uint8_t i = 0; i < _extraCount; i++) {
      char d[40];
      rak9154::describeStatusWord(_extraVal[i], d, sizeof d);
      o.add(" ch%02X=%04X(%s)", (unsigned)_extraCh[i], (unsigned)_extraVal[i], d);
    }
  }
  return o.used(out);
}

size_t RAK9154Battery::formatDiag(char* out, size_t cap) const {
  if (cap == 0) return 0;
  Out o(out, cap);
  o.add("RAK9154 req:%u prov:%u skip:%u rxfr:%u rxb:%u asm:%u uart:%u to:%u/%u st:%s pid:%u wait:%u gap:%u pstreak:%u",
        (unsigned)_stats.requestsSent, (unsigned)_stats.provisions, (unsigned)_stats.provSkipped, (unsigned)_stats.rxFrames,
        (unsigned)_stats.rxBytes, (unsigned)_stats.framingErrors, (unsigned)_t.rxErrors(),
        (unsigned)_consecFail, (unsigned)_cfg.reprovisionAfter, _state == ST_POLLING ? "POLL" : "PROV",
        (unsigned)_pid, (unsigned)_awaiting, (unsigned)_t.txGapBits(), (unsigned)_provStreak);
  return o.used(out);
}

size_t RAK9154Battery::formatRaw(char* out, size_t cap, size_t offset) const {
  if (cap == 0) return 0;
  Out o(out, cap);
#if RAK9154_RAW_CAPTURE
  if (_rawLen == 0) {
    o.add("RAK9154 no frame received yet");
  } else if (offset >= _rawLen) {
    o.add("RAK9154 raw %uB: offset %u is past the end", (unsigned)_rawLen, (unsigned)offset);
  } else {
    o.add("RAK9154 raw %uB +%u %us ago:", (unsigned)_rawLen, (unsigned)offset,
          (unsigned)((uint32_t)(_clock() - _rawAt) / 1000));
    for (size_t i = offset; i < _rawLen && o.left > 5; i++) o.add(" %02X", (unsigned)_raw[i]);
  }
#else
  (void)offset;
  o.add("RAK9154 raw capture disabled");
#endif
  return o.used(out);
}

#endif  // RAK9154_BATTERY || RAK9154_HOST_TEST
