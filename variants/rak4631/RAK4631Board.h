#pragma once

#include <MeshCore.h>
#include <Arduino.h>
#include <helpers/NRF52Board.h>

#ifdef RAK9154_BATTERY
  // RAK9154 Solar Battery telemetry over its Output 1 SensorHub one-wire interface. See docs/RAK9154.md
  #include <rak9154/RAK9154Battery.h>
  #include <rak9154/RAK9154HalfSerial.h>
#endif

// built-ins
#define  PIN_VBAT_READ    5
#define  ADC_MULTIPLIER   (3 * 1.73 * 1.187 * 1000)

class RAK4631Board : public NRF52BoardDCDC {
protected:
#ifdef NRF52_POWER_MANAGEMENT
  void initiateShutdown(uint8_t reason) override;
#endif

public:
  RAK4631Board() : NRF52Board("RAK4631_OTA") {}
  void begin();

  #define BATTERY_SAMPLES 8

  uint16_t getBattMilliVolts() override {
#ifdef RAK9154_BATTERY
    // Cached value only, O(1): never touches the one-wire line from here (the poll happens in loop()).
    if (rak9154.hasValidTelemetry()) {
      uint32_t mv = rak9154.getVoltageMilliVolts();
      return mv > 0xFFFF ? 0xFFFF : (uint16_t)mv;
    }
    // no fresh RAK9154 data: fall through to the stock ADC method below
#endif
    analogReadResolution(12);

    uint32_t raw = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
      raw += analogRead(PIN_VBAT_READ);
    }
    raw = raw / BATTERY_SAMPLES;

    return (ADC_MULTIPLIER * raw) / 4096;
  }

  const char* getManufacturerName() const override {
    return "RAK 4631";
  }

#ifdef RAK9154_BATTERY
  void loop() override { rak9154.loop(); }
  bool isChargerActive() override { return rak9154.isCharging(); }
  bool handleCommand(const char* command, uint32_t sender_timestamp, char* reply) override;
  RAK9154Battery& rak9154Battery() { return rak9154; }

private:
  static uint32_t rak9154Millis() { return (uint32_t)millis(); }
  RAK9154HalfSerial rak9154_port{RAK9154_ONEWIRE_PIN};
  RAK9154Battery rak9154{rak9154_port, &RAK4631Board::rak9154Millis};
#endif
};
