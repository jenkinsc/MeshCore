// SPDX-License-Identifier: MIT
// Host-only stand-in for the few Arduino/nRF52 symbols RAK9154HalfSerial.cpp uses. Lets the REAL transport
// source run in a simulated world (fake cycle counter + scripted pin waveform) in test_halfserial.cpp.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PINS_COUNT 48
#define INPUT_PULLUP 2
#define FALLING 3
#define PIN_SERIAL1_RX 15
typedef void (*voidFuncPtr)(void);

extern const uint32_t g_ADigitalPinMap[PINS_COUNT];
void pinMode(uint32_t pin, uint32_t mode);
int attachInterrupt(uint32_t pin, voidFuncPtr cb, uint32_t mode);
void detachInterrupt(uint32_t pin);
unsigned long millis();
extern uint32_t SystemCoreClock;

struct FakeDWT { volatile uint32_t CTRL; volatile uint32_t CYCCNT; };
struct FakeCoreDebug { volatile uint32_t DEMCR; };
FakeDWT* fake_dwt();               // every access advances simulated time
extern FakeCoreDebug g_fake_coredebug;
#define DWT (fake_dwt())
#define CoreDebug (&g_fake_coredebug)
#define CoreDebug_DEMCR_TRCENA_Msk (1u << 24)
#define DWT_CTRL_CYCCNTENA_Msk 1u
