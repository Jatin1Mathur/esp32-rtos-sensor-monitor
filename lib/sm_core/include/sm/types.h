// Shared data types passed between tasks.
// Everything crossing a task boundary is a small, trivially copyable struct,
// so it can be sent *by value* through an RTOS queue (no shared pointers,
// no ownership questions, no use-after-free between tasks).
#pragma once

#include <cstdint>
#include <type_traits>

namespace sm {

struct Reading {
    float temperature_c;
    float humidity_pct;
};

struct Sample {
    uint32_t seq;           // monotonically increasing; gaps reveal dropped samples
    uint32_t timestamp_ms;  // time of acquisition, not time of sending
    float temperature_c;
    float humidity_pct;
};

enum class SensorStatus : uint8_t { Ok, BusBusy, Nack, CrcError };

enum class ErrorSource : uint8_t { Sensor, Comm, System };

enum class ErrorCode : uint8_t {
    SensorBusBusy,   // could not get the I2C bus mutex in time
    SensorNack,      // sensor did not acknowledge
    SensorCrc,       // corrupted data on the bus
    QueueOverflow,   // sample queue full, oldest sample dropped
    WifiDown,        // no network connection
    HttpFailed,      // server unreachable or returned an error
    TaskStalled,     // a task stopped sending heartbeats
};

struct ErrorEvent {
    ErrorSource source;
    ErrorCode code;
    uint32_t timestamp_ms;
};

enum class TaskId : uint8_t { Sensor = 0, Comm = 1, Count = 2 };

const char* to_string(ErrorCode code);

static_assert(std::is_trivially_copyable<Sample>::value, "queue items must be copyable by memcpy");
static_assert(std::is_trivially_copyable<ErrorEvent>::value, "queue items must be copyable by memcpy");

}  // namespace sm
