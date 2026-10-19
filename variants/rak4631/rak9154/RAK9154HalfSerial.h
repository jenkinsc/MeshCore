// SPDX-License-Identifier: MIT
//
// Minimal single-wire, half-duplex, 9600 8N1 bit-banged UART for nRF52 (Adafruit nRF52 Arduino core).
//
// Written independently for MeshCore; it deliberately does NOT reuse RAK's SoftwareHalfSerial (LGPL-2.1) or
// Meshtastic code (GPL-3.0). It uses the generic, well known technique: a falling-edge GPIOTE interrupt
// samples one byte at mid-bit timing, and transmit drives the line for one frame with the receive interrupt
// detached. Bit timing uses the Cortex-M4 DWT cycle counter instead of delayMicroseconds().
//
// Costs and limits (see docs/RAK9154.md):
//   * RX: ~1.04 ms inside the interrupt per received byte. A frame of N bytes holds the CPU in N slices of ~1 ms.
//   * TX: blocking, ~1.04 ms per byte, only while a request is being sent.
//   * Higher priority interrupts (SoftDevice, USB) can stretch bit timing; corrupted frames are rejected by the
//     protocol checksum and simply cost one poll.
//   * RX storm guard: if far more bytes arrive than the line rate allows (noise / wiring fault), reception is
//     suspended for a while instead of starving the radio loop.
#pragma once

#if defined(RAK9154_BATTERY) && defined(NRF52_PLATFORM)

#include <Arduino.h>

#include "RAK9154Battery.h"

// !!! UNVERIFIED !!! Which WisBlock/nRF52840 pin carries the RAK9154 Output 1 one-wire data line on the
// WisMesh repeater harness has NOT been established from public documentation. P0.15 (Arduino pin 15,
// PIN_SERIAL1_RX) is what RAK's own one-wire library and Meshtastic's RAK2560 variant use, so it is the default.
// Check your wiring, then override with  -D RAK9154_ONEWIRE_PIN=<arduino pin number>.
#ifndef RAK9154_ONEWIRE_PIN
#define RAK9154_ONEWIRE_PIN PIN_SERIAL1_RX
#endif

#ifndef RAK9154_BAUD
#define RAK9154_BAUD 9600
#endif

// CPU cycles between the falling edge on the pin and the moment the ISR body starts running (interrupt entry +
// core GPIOTE dispatcher). Used to centre the first sample. Tune with a scope if frames fail checksum.
#ifndef RAK9154_RX_LATENCY_CYCLES
#define RAK9154_RX_LATENCY_CYCLES 320
#endif

class RAK9154HalfSerial : public rak9154::Transport {
public:
  explicit RAK9154HalfSerial(uint8_t arduinoPin) : _pin(arduinoPin) {}   // constructor touches no hardware
  ~RAK9154HalfSerial() {                                   // never leave the ISR with a dangling instance pointer
    if (s_instance == this) {
      detachInterrupt(_pin);
      s_instance = nullptr;
    }
  }

  bool begin() override;
  void write(const uint8_t* data, size_t len) override;
  int  read() override;
  void service() override;

  uint32_t overflowCount() const { return _overflows; }
  uint32_t stormCount() const { return _storms; }
  uint32_t framingErrorCount() const { return _stopErrors; }

private:
  static void isrTrampoline();
  void onStartBit();
  bool attach();
  void txByte(uint8_t b, uint32_t& t, uint32_t bitCycles);

  static RAK9154HalfSerial* s_instance;

  static constexpr uint8_t RX_SIZE = 128;                  // power of two
  volatile uint8_t _rx[RX_SIZE];
  volatile uint8_t _head = 0;                              // written by ISR
  volatile uint8_t _tail = 0;                              // written by loop
  volatile uint32_t _overflows = 0, _stopErrors = 0;
  volatile bool     _stormed = false;
  volatile uint32_t _windowStart = 0;
  volatile uint16_t _windowBytes = 0;
  uint32_t _storms = 0;
  uint32_t _stormAt = 0;
  uint8_t  _pin;
  uint32_t _nrfPin = 0;
  uint32_t _bitCycles = 0;
  bool     _ready = false;
};

#endif  // RAK9154_BATTERY && NRF52_PLATFORM
