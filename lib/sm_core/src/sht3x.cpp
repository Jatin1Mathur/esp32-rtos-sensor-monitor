#include "sm/sht3x.h"

namespace sm {

namespace {
constexpr uint16_t kCmdMeasureHighRepNoStretch = 0x2400;
constexpr uint16_t kCmdSoftReset = 0x30A2;
constexpr uint32_t kResetTimeMs = 2;  // datasheet: max 1.5 ms
}  // namespace

Sht3x::Sht3x(II2cBus& bus, ILock& bus_lock, IClock& clock, uint8_t address)
    : bus_(bus), bus_lock_(bus_lock), clock_(clock), address_(address) {}

uint8_t Sht3x::crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

float Sht3x::raw_to_celsius(uint16_t raw) { return -45.0f + 175.0f * (static_cast<float>(raw) / 65535.0f); }

float Sht3x::raw_to_humidity(uint16_t raw) { return 100.0f * (static_cast<float>(raw) / 65535.0f); }

SensorStatus Sht3x::send_command(uint16_t command) {
    LockGuard guard(bus_lock_, kBusTimeoutMs);
    if (!guard.owns()) return SensorStatus::BusBusy;
    const uint8_t cmd[2] = {static_cast<uint8_t>(command >> 8), static_cast<uint8_t>(command & 0xFF)};
    return bus_.write(address_, cmd, sizeof(cmd)) ? SensorStatus::Ok : SensorStatus::Nack;
}

SensorStatus Sht3x::measure(Reading& out) {
    SensorStatus status = send_command(kCmdMeasureHighRepNoStretch);
    if (status != SensorStatus::Ok) return status;

    // Bus is released while the sensor converts; the task sleeps (yields).
    clock_.delay_ms(kConversionTimeMs);

    uint8_t rx[6] = {};
    {
        LockGuard guard(bus_lock_, kBusTimeoutMs);
        if (!guard.owns()) return SensorStatus::BusBusy;
        if (!bus_.read(address_, rx, sizeof(rx))) return SensorStatus::Nack;
    }

    if (crc8(&rx[0], 2) != rx[2] || crc8(&rx[3], 2) != rx[5]) return SensorStatus::CrcError;

    const uint16_t raw_t = static_cast<uint16_t>((rx[0] << 8) | rx[1]);
    const uint16_t raw_rh = static_cast<uint16_t>((rx[3] << 8) | rx[4]);
    out.temperature_c = raw_to_celsius(raw_t);
    out.humidity_pct = raw_to_humidity(raw_rh);
    return SensorStatus::Ok;
}

SensorStatus Sht3x::reset() {
    SensorStatus status = send_command(kCmdSoftReset);
    if (status == SensorStatus::Ok) clock_.delay_ms(kResetTimeMs);
    return status;
}

const char* to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::SensorBusBusy: return "SensorBusBusy";
        case ErrorCode::SensorNack: return "SensorNack";
        case ErrorCode::SensorCrc: return "SensorCrc";
        case ErrorCode::QueueOverflow: return "QueueOverflow";
        case ErrorCode::WifiDown: return "WifiDown";
        case ErrorCode::HttpFailed: return "HttpFailed";
        case ErrorCode::TaskStalled: return "TaskStalled";
    }
    return "Unknown";
}

}  // namespace sm
