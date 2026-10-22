// SPDX-License-Identifier: MIT
//
// Publishes the extra RAK9154 values into MeshCore's Cayenne LPP telemetry reply. Header-only template so it can be
// unit tested with a recording stand-in for CayenneLPP (it only needs addPercentage/addCurrent/addTemperature).
//
// MeshCore's repeater already writes the battery VOLTAGE on channel 1 from getBattMilliVolts(), which is the cached
// RAK9154 value in the custom target, so voltage is intentionally not added here.
//
//   channel 1 (self): state of charge (LPP percentage), charge current (LPP current)
//   next free channel: battery temperature  (channel 1 already carries the nRF52 chip temperature; a second
//                     temperature on the same channel would collide with it in the app)
//
// LPP "current" is an UNSIGNED 0.001 A value, so only charging current can be represented: discharge is sent as 0 A.
// The sign convention of the probe's current field is UNVERIFIED (charging = positive is assumed); compare
// "rak9154 status" at night/daytime before trusting it.
#pragma once

#include <stdint.h>

#include "RAK9154Battery.h"

namespace rak9154 {

template <class LPP>
void appendTelemetry(const RAK9154Battery& b, LPP& lpp, uint8_t selfChannel, uint8_t extraChannel) {
  if (b.hasStateOfCharge()) lpp.addPercentage(selfChannel, b.getStateOfCharge());
  if (b.hasCurrent()) {
    const int32_t mA = b.getCurrentMilliAmps();
    lpp.addCurrent(selfChannel, mA > 0 ? (float)mA / 1000.0f : 0.0f);
  }
  if (b.hasTemperature()) lpp.addTemperature(extraChannel, b.getTemperatureC());
}

}  // namespace rak9154
