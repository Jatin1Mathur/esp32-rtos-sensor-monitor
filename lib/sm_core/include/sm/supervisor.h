// Error-handling policy, kept in ONE place instead of scattered over tasks.
//
// The Supervisor itself is plain, single-threaded logic: it is only ever
// called from the supervisor task. Other tasks never call it directly;
// they send ErrorEvents through a queue and write heartbeats into the
// lock-free HeartbeatTable.
#pragma once

#include <atomic>
#include <cstdint>

#include "sm/types.h"

namespace sm {

enum class Action : uint8_t { None, ResetSensor, ReconnectWifi, RestartDevice };

const char* to_string(Action action);

struct SupervisorConfig {
    uint8_t sensor_errors_before_reset = 3;    // consecutive sensor errors -> soft reset sensor
    uint8_t sensor_resets_before_restart = 3;  // resets without success -> restart device
    uint8_t comm_errors_before_reconnect = 3;  // consecutive comm errors -> reconnect Wi-Fi
    uint32_t heartbeat_timeout_ms = 5000;      // no heartbeat for this long -> task stalled
};

// One slot per task. Each slot has exactly ONE writer (its task) and one
// reader (the supervisor), so a relaxed/acquire-release atomic is enough:
// no mutex, no priority inversion, safe to call from any task.
class HeartbeatTable {
public:
    void beat(TaskId task, uint32_t now_ms) {
        slots_[index(task)].store(now_ms | kSeenBit, std::memory_order_release);
    }
    // Returns false if the task has not sent its first heartbeat yet.
    bool last_beat(TaskId task, uint32_t& out_ms) const {
        const uint32_t v = slots_[index(task)].load(std::memory_order_acquire);
        if ((v & kSeenBit) == 0) return false;
        out_ms = v & ~kSeenBit;
        return true;
    }

private:
    static constexpr uint32_t kSeenBit = 0x80000000u;  // timestamps wrap at 2^31 ms (~24.8 days)
    static constexpr int index(TaskId t) { return static_cast<int>(t); }
    std::atomic<uint32_t> slots_[static_cast<int>(TaskId::Count)] = {};
};

class Supervisor {
public:
    explicit Supervisor(SupervisorConfig config = SupervisorConfig{});

    Action on_error(const ErrorEvent& event);
    void on_sensor_ok();
    void on_comm_ok();

    // Checks every task's heartbeat. Wrap-around safe (unsigned subtraction).
    Action check_heartbeats(const HeartbeatTable& table, uint32_t now_ms, TaskId* stalled = nullptr) const;

    uint32_t total_errors() const { return total_errors_; }
    uint32_t dropped_samples_reported() const { return overflow_events_; }
    uint8_t sensor_resets() const { return sensor_resets_; }

private:
    SupervisorConfig config_;
    uint8_t sensor_consecutive_errors_ = 0;
    uint8_t sensor_resets_ = 0;
    uint8_t comm_consecutive_errors_ = 0;
    uint32_t total_errors_ = 0;
    uint32_t overflow_events_ = 0;
};

}  // namespace sm
