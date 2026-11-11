// Test doubles for single-threaded unit tests.
#pragma once

#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "sm/interfaces.h"

namespace host {

class MockI2cBus : public sm::II2cBus {
public:
    MOCK_METHOD(bool, write, (uint8_t addr, const uint8_t* data, size_t len), (override));
    MOCK_METHOD(bool, read, (uint8_t addr, uint8_t* data, size_t len), (override));
};

// Records lock/unlock/delay calls so tests can check the locking discipline.
struct EventLog {
    std::vector<std::string> events;
};

class FakeLock : public sm::ILock {
public:
    explicit FakeLock(EventLog& log) : log_(log) {}
    bool try_lock_for(uint32_t) override {
        if (busy) return false;
        log_.events.push_back("lock");
        held = true;
        return true;
    }
    void unlock() override {
        log_.events.push_back("unlock");
        held = false;
    }
    bool busy = false;
    bool held = false;

private:
    EventLog& log_;
};

class FakeClock : public sm::IClock {
public:
    explicit FakeClock(EventLog* log = nullptr) : log_(log) {}
    uint32_t now_ms() const override { return now; }
    void delay_ms(uint32_t ms) override {
        if (log_) log_->events.push_back("delay " + std::to_string(ms));
        now += ms;
    }
    uint32_t now = 0;

private:
    EventLog* log_;
};

class FakeSensor : public sm::ISensor {
public:
    sm::SensorStatus measure(sm::Reading& out) override {
        ++measure_calls;
        out = {21.5f, 40.0f};
        return next_status;
    }
    sm::SensorStatus reset() override {
        ++reset_calls;
        return sm::SensorStatus::Ok;
    }
    sm::SensorStatus next_status = sm::SensorStatus::Ok;
    int measure_calls = 0;
    int reset_calls = 0;
};

class FakeNetwork : public sm::INetwork {
public:
    bool connected() const override { return up; }
    void reconnect() override { ++reconnects; }
    bool up = true;
    int reconnects = 0;
};

class FakeHttp : public sm::IHttpClient {
public:
    int post_json(const char* body, size_t len) override {
        bodies.emplace_back(body, len);
        return status;
    }
    int status = 200;
    std::vector<std::string> bodies;
};

class FakePlatform : public sm::IPlatform {
public:
    void restart() override { ++restarts; }
    int restarts = 0;
};

}  // namespace host
