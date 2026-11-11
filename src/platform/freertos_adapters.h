// FreeRTOS + Arduino-ESP32 implementations of the core interfaces.
#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "sm/interfaces.h"

namespace platform {

inline TickType_t to_ticks(uint32_t ms) { return pdMS_TO_TICKS(ms); }

// Queue with statically allocated storage: created once at boot, no heap
// fragmentation, and the memory use is visible at link time.
template <typename T, size_t N>
class StaticQueue : public sm::IQueue<T> {
public:
    StaticQueue() { handle_ = xQueueCreateStatic(N, sizeof(T), storage_, &control_); }
    bool send(const T& item, uint32_t timeout_ms) override {
        return xQueueSend(handle_, &item, to_ticks(timeout_ms)) == pdTRUE;
    }
    bool receive(T& item, uint32_t timeout_ms) override {
        return xQueueReceive(handle_, &item, to_ticks(timeout_ms)) == pdTRUE;
    }
    size_t size() const override { return uxQueueMessagesWaiting(handle_); }
    size_t capacity() const override { return N; }

private:
    StaticQueue_t control_{};
    uint8_t storage_[N * sizeof(T)]{};
    QueueHandle_t handle_ = nullptr;
};

// A FreeRTOS *mutex* (not a binary semaphore): it has an owner and
// priority inheritance, which bounds priority inversion on the I2C bus.
class StaticMutex : public sm::ILock {
public:
    StaticMutex() { handle_ = xSemaphoreCreateMutexStatic(&control_); }
    bool try_lock_for(uint32_t timeout_ms) override { return xSemaphoreTake(handle_, to_ticks(timeout_ms)) == pdTRUE; }
    void unlock() override { xSemaphoreGive(handle_); }

private:
    StaticSemaphore_t control_{};
    SemaphoreHandle_t handle_ = nullptr;
};

class WireBus : public sm::II2cBus {
public:
    explicit WireBus(TwoWire& wire) : wire_(wire) {}
    bool write(uint8_t addr, const uint8_t* data, size_t len) override {
        wire_.beginTransmission(addr);
        wire_.write(data, len);
        return wire_.endTransmission() == 0;
    }
    bool read(uint8_t addr, uint8_t* data, size_t len) override {
        if (wire_.requestFrom(addr, static_cast<uint8_t>(len)) != len) return false;
        for (size_t i = 0; i < len; ++i) data[i] = static_cast<uint8_t>(wire_.read());
        return true;
    }

private:
    TwoWire& wire_;
};

class RtosClock : public sm::IClock {
public:
    uint32_t now_ms() const override { return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS); }
    void delay_ms(uint32_t ms) override { vTaskDelay(to_ticks(ms)); }  // blocks THIS task only
};

class WifiNetwork : public sm::INetwork {
public:
    bool connected() const override { return WiFi.status() == WL_CONNECTED; }
    void reconnect() override { WiFi.reconnect(); }
};

class ArduinoHttpClient : public sm::IHttpClient {
public:
    ArduinoHttpClient(const char* url, uint16_t timeout_ms) : url_(url), timeout_ms_(timeout_ms) {}
    int post_json(const char* body, size_t len) override {
        HTTPClient http;
        http.setTimeout(timeout_ms_);  // must stay below the heartbeat timeout
        if (!http.begin(url_)) return -1;
        http.addHeader("Content-Type", "application/json");
        const int code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body)), len);
        http.end();
        return code;
    }

private:
    const char* url_;
    uint16_t timeout_ms_;
};

class Esp32Platform : public sm::IPlatform {
public:
    void restart() override {
        log_e("Supervisor requested restart");
        delay(100);  // let the log flush
        ESP.restart();
    }
};

}  // namespace platform
