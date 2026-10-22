// SPDX-License-Identifier: MIT
#if defined(RAK9154_BATTERY) && defined(NRF52_PLATFORM)

#include "RAK9154HalfSerial.h"

#include "nrf_gpio.h"

RAK9154HalfSerial* RAK9154HalfSerial::s_instance = nullptr;

namespace {

// Max start bits a 9600 baud line can physically deliver in 100 ms is ~96 (back-to-back 10 bit frames). Seeing more
// than this means noise or a wiring fault, so reception is suspended instead of starving the main loop.
constexpr uint16_t STORM_MAX_BYTES = 110;
constexpr uint32_t STORM_PAUSE_MS  = 5000;
constexpr uint32_t WAIT_SPIN_LIMIT = 400000;     // hard stop for any busy-wait (normal waits are < 5k iterations)

inline uint32_t cycles() { return DWT->CYCCNT; }

// Busy-wait until the cycle counter reaches `target` (wrap safe). Bounded so a stuck counter cannot hang the node.
inline void waitUntil(uint32_t target) {
  for (uint32_t i = 0; i < WAIT_SPIN_LIMIT && (int32_t)(cycles() - target) < 0; i++) {
  }
}

}  // namespace

bool RAK9154HalfSerial::begin() {
  if (_ready) return true;
  if (s_instance != nullptr && s_instance != this) return false;
  if (_pin >= PINS_COUNT) return false;
  _nrfPin = g_ADigitalPinMap[_pin];

  // The DWT cycle counter drives all bit timing. Make sure it exists and is actually counting.
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  const uint32_t a = cycles();
  for (volatile int i = 0; i < 64; i++) {
  }
  if (cycles() == a) return false;

  _bitCycles = SystemCoreClock / RAK9154_BAUD;
  _head = _tail = 0;
  _windowStart = cycles();
  _windowBytes = 0;
  _stormed = false;

  s_instance = this;
  pinMode(_pin, INPUT_PULLUP);
  if (!attach()) {                       // attachInterrupt() returns 0 when it cannot get a GPIOTE channel
    s_instance = nullptr;
    return false;
  }
  _ready = true;
  return true;
}

bool RAK9154HalfSerial::attach() { return attachInterrupt(_pin, isrTrampoline, FALLING) != 0; }

void RAK9154HalfSerial::isrTrampoline() {
  if (s_instance) s_instance->onStartBit();
}

// Runs in the GPIOTE interrupt on the falling edge of a start bit. ~1.04 ms per byte.
void RAK9154HalfSerial::onStartBit() {
  const uint32_t t0 = cycles();
  if (_stormed) return;

  // Count every edge interrupt (including runt pulses) so a flood of noise is caught before it starves the loop.
  if ((uint32_t)(t0 - _windowStart) > SystemCoreClock / 10) {   // 100 ms accounting window
    _windowStart = t0;
    _windowBytes = 0;
  }
  if (++_windowBytes > STORM_MAX_BYTES) {
    _stormed = true;
    _storms++;
    return;
  }
  if (nrf_gpio_pin_read(_nrfPin)) return;                       // glitch: line is already back high

  // Estimated instant of the falling edge, then the centre of data bit 0 is 1.5 bit times later.
  uint32_t t = t0 - RAK9154_RX_LATENCY_CYCLES + _bitCycles + _bitCycles / 2;
  uint8_t v = 0;
  for (uint8_t i = 0; i < 8; i++) {
    waitUntil(t);
    if (nrf_gpio_pin_read(_nrfPin)) v |= (uint8_t)(1u << i);
    t += _bitCycles;
  }
  waitUntil(t);                                                 // centre of the stop bit
  if (!nrf_gpio_pin_read(_nrfPin)) {                            // stop bit must be high: otherwise noise / misaligned
    _stopErrors++;
    return;
  }

  const uint8_t next = (uint8_t)((_head + 1) & (RX_SIZE - 1));
  if (next == _tail) {
    _overflows++;
    return;
  }
  _rx[_head] = v;
  _head = next;
}

int RAK9154HalfSerial::read() {
  if (_tail == _head) return -1;
  const uint8_t b = _rx[_tail];
  _tail = (uint8_t)((_tail + 1) & (RX_SIZE - 1));
  return b;
}

void RAK9154HalfSerial::service() {
  if (!_ready || !_stormed) {
    _stormAt = 0;
    return;
  }
  const uint32_t now = millis();
  if (_stormAt == 0) {
    _stormAt = now | 1u;                                        // start the pause (never 0)
    return;
  }
  if ((uint32_t)(now - _stormAt) >= STORM_PAUSE_MS) {
    _head = _tail = 0;
    _windowStart = cycles();
    _windowBytes = 0;
    _stormed = false;
    _stormAt = 0;
  }
}

void RAK9154HalfSerial::txByte(uint8_t b, uint32_t& t, uint32_t bitCycles) {
  waitUntil(t);                                                 // start bit
  nrf_gpio_pin_clear(_nrfPin);
  t += bitCycles;
  for (uint8_t i = 0; i < 8; i++) {                             // data bits, LSB first
    waitUntil(t);
    nrf_gpio_pin_write(_nrfPin, (b >> i) & 1u);
    t += bitCycles;
  }
  waitUntil(t);                                                 // stop bit
  nrf_gpio_pin_set(_nrfPin);
  t += bitCycles;
}

void RAK9154HalfSerial::write(const uint8_t* data, size_t len) {
  if (!_ready || data == nullptr || len == 0) return;

  detachInterrupt(_pin);                                        // we must not "hear" our own transmission
  nrf_gpio_pin_set(_nrfPin);                                    // idle level first, then enable the output: no glitch
  nrf_gpio_cfg_output(_nrfPin);

  uint32_t t = cycles() + _bitCycles;                           // one idle bit before the first start bit
  for (size_t i = 0; i < len; i++) {
    txByte(data[i], t, _bitCycles);
    t += (uint32_t)_gapBits * _bitCycles;                         // extra idle time between bytes (line stays high)
  }
  waitUntil(t);                                                 // finish the last stop bit

  nrf_gpio_cfg_input(_nrfPin, NRF_GPIO_PIN_PULLUP);             // release the line to the probe
  t += _bitCycles;
  waitUntil(t);                                                 // let it settle high before listening again

  if (!attach()) _ready = false;                                // could not re-arm the receiver: go inert, never hang
}

#endif  // RAK9154_BATTERY && NRF52_PLATFORM
