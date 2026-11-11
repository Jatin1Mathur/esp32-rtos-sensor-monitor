// Driver for the Sensirion SHT3x temperature/humidity sensor
// (used e.g. in the M5Stack ENV unit), on a *shared* I2C bus.
//
// Concurrency design:
//  - Every bus transaction takes the bus mutex with a timeout.
//  - The mutex is NOT held during the ~15 ms conversion time, so other
//    tasks can use the bus while the sensor is measuring.
#pragma once

#include <cstddef>
#include <cstdint>

#include "sm/interfaces.h"

namespace sm {

class Sht3x : public ISensor {
public:
    static constexpr uint8_t kDefaultAddress = 0x44;
    static constexpr uint32_t kBusTimeoutMs = 20;
    static constexpr uint32_t kConversionTimeMs = 16;  // datasheet max 15.5 ms (high repeatability)

    Sht3x(II2cBus& bus, ILock& bus_lock, IClock& clock, uint8_t address = kDefaultAddress);

    SensorStatus measure(Reading& out) override;
    SensorStatus reset() override;

    // CRC-8, polynomial 0x31, init 0xFF (SHT3x datasheet, section 4.12).
    static uint8_t crc8(const uint8_t* data, size_t len);
    static float raw_to_celsius(uint16_t raw);
    static float raw_to_humidity(uint16_t raw);

private:
    SensorStatus send_command(uint16_t command);

    II2cBus& bus_;
    ILock& bus_lock_;
    IClock& clock_;
    uint8_t address_;
};

}  // namespace sm
