// ESP32 sensor monitor, FreeRTOS version.
//
// Task layout (priorities: higher number = more important):
//
//   task         core  prio  period / trigger        owns
//   ----------   ----  ----  ----------------------  ---------------------
//   sensor        1     5    every 1000 ms (exact)    SHT3x sensor
//   supervisor    1     4    error queue / 500 ms     error policy
//   comm          0     3    sample queue             Wi-Fi + HTTP client
//
// The Wi-Fi stack itself runs on core 0, so the network work stays there and
// the time-critical sampling runs undisturbed on core 1.
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>

#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

#include "platform/freertos_adapters.h"
#include "sm/sht3x.h"
#include "sm/supervisor.h"
#include "sm/tasks.h"

namespace {

constexpr uint32_t kSamplePeriodMs = 1000;
constexpr uint32_t kSupervisorWaitMs = 500;
constexpr uint16_t kHttpTimeoutMs = 3000;   // < heartbeat timeout (5000 ms)
constexpr size_t kSampleQueueLength = 60;   // ~1 minute of data survives an outage
constexpr size_t kErrorQueueLength = 16;

// Stack sizes in bytes (ESP-IDF counts bytes, not words). Checked at runtime
// with uxTaskGetStackHighWaterMark() and logged by the supervisor.
constexpr uint32_t kSensorStack = 3072;
constexpr uint32_t kCommStack = 8192;  // HTTPClient + TLS-free HTTP needs more
constexpr uint32_t kSupervisorStack = 3072;

// ---- shared objects: all created statically, before the scheduler runs tasks
platform::StaticQueue<sm::Sample, kSampleQueueLength> g_samples;
platform::StaticQueue<sm::ErrorEvent, kErrorQueueLength> g_errors;
platform::StaticMutex g_i2c_mutex;  // guards the shared I2C bus
platform::RtosClock g_clock;
platform::WireBus g_bus(Wire);
platform::WifiNetwork g_network;
platform::ArduinoHttpClient g_http(BRIDGE_URL, kHttpTimeoutMs);
platform::Esp32Platform g_platform;

sm::Sht3x g_sensor(g_bus, g_i2c_mutex, g_clock);
sm::HeartbeatTable g_heartbeats;
sm::Requests g_requests;
sm::Stats g_stats;
sm::Supervisor g_supervisor;

TaskHandle_t g_sensor_handle = nullptr;
TaskHandle_t g_comm_handle = nullptr;

void sensor_task(void*) {
    sm::SensorTask task{g_sensor, g_samples, g_errors, g_clock, g_heartbeats, g_requests, g_stats};
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        sm::sensor_step(task);
        // vTaskDelayUntil keeps a fixed period: the time spent measuring does
        // not accumulate as drift (vTaskDelay would).
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kSamplePeriodMs));
    }
}

void comm_task(void*) {
    static sm::CommTask task{g_samples, g_errors, g_network, g_http, g_clock,
                             g_heartbeats, g_requests, g_stats, DEVICE_ID};
    for (;;) {
        const uint32_t wait_ms = sm::comm_step(task);
        if (wait_ms > 0) sm::sleep_with_heartbeat(g_clock, g_heartbeats, sm::TaskId::Comm, wait_ms);
    }
}

void supervisor_task(void*) {
    sm::SupervisorTask task{g_errors, g_supervisor, g_heartbeats, g_requests, g_platform, g_clock, g_stats};
    uint32_t last_report = 0;
    for (;;) {
        sm::supervisor_step(task, kSupervisorWaitMs);
        if (task.last_action != sm::Action::None) {
            log_w("supervisor action: %s", sm::to_string(task.last_action));
            task.last_action = sm::Action::None;
        }
        const uint32_t now = g_clock.now_ms();
        if (now - last_report >= 10000) {
            last_report = now;
            log_i("produced=%u sent=%u dropped_total=%u queued=%u errors=%u | stack free: sensor=%u comm=%u",
                  static_cast<unsigned>(g_stats.produced.load()), static_cast<unsigned>(g_stats.sent.load()),
                  static_cast<unsigned>(g_stats.dropped_total.load()), static_cast<unsigned>(g_samples.size()),
                  static_cast<unsigned>(g_supervisor.total_errors()),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_sensor_handle)),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_comm_handle)));
        }
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // non-blocking; comm task waits for the link

    xTaskCreatePinnedToCore(sensor_task, "sensor", kSensorStack, nullptr, 5, &g_sensor_handle, 1);
    xTaskCreatePinnedToCore(supervisor_task, "supervisor", kSupervisorStack, nullptr, 4, nullptr, 1);
    xTaskCreatePinnedToCore(comm_task, "comm", kCommStack, nullptr, 3, &g_comm_handle, 0);
}

// Arduino's loop() is itself a FreeRTOS task. We do not need it anymore,
// so it deletes itself instead of wasting CPU time and stack.
void loop() { vTaskDelete(nullptr); }
