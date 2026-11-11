#include "sm/supervisor.h"

namespace sm {

const char* to_string(Action action) {
    switch (action) {
        case Action::None: return "None";
        case Action::ResetSensor: return "ResetSensor";
        case Action::ReconnectWifi: return "ReconnectWifi";
        case Action::RestartDevice: return "RestartDevice";
    }
    return "Unknown";
}

Supervisor::Supervisor(SupervisorConfig config) : config_(config) {}

Action Supervisor::on_error(const ErrorEvent& event) {
    ++total_errors_;

    if (event.code == ErrorCode::QueueOverflow) {
        // Expected while offline: the drop-oldest policy already handled it.
        ++overflow_events_;
        return Action::None;
    }
    if (event.code == ErrorCode::TaskStalled) {
        // A stalled task may hold a mutex; killing just that task could leave
        // the bus locked forever. A clean restart is the safe recovery.
        return Action::RestartDevice;
    }

    if (event.source == ErrorSource::Sensor) {
        if (++sensor_consecutive_errors_ < config_.sensor_errors_before_reset) return Action::None;
        sensor_consecutive_errors_ = 0;
        if (++sensor_resets_ >= config_.sensor_resets_before_restart) return Action::RestartDevice;
        return Action::ResetSensor;
    }

    if (event.source == ErrorSource::Comm) {
        if (++comm_consecutive_errors_ < config_.comm_errors_before_reconnect) return Action::None;
        comm_consecutive_errors_ = 0;
        return Action::ReconnectWifi;
    }

    return Action::None;
}

void Supervisor::on_sensor_ok() {
    sensor_consecutive_errors_ = 0;
    sensor_resets_ = 0;
}

void Supervisor::on_comm_ok() { comm_consecutive_errors_ = 0; }

Action Supervisor::check_heartbeats(const HeartbeatTable& table, uint32_t now_ms, TaskId* stalled) const {
    for (int i = 0; i < static_cast<int>(TaskId::Count); ++i) {
        const TaskId task = static_cast<TaskId>(i);
        uint32_t last = 0;
        if (!table.last_beat(task, last)) continue;  // not started yet: no false alarm
        // Timestamps are 31-bit; mask the difference so wrap-around works.
        const uint32_t age = (now_ms - last) & 0x7FFFFFFFu;
        if (age > config_.heartbeat_timeout_ms) {
            if (stalled) *stalled = task;
            return Action::RestartDevice;
        }
    }
    return Action::None;
}

}  // namespace sm
