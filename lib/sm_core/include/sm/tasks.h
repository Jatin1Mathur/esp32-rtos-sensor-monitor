// The bodies of the three RTOS tasks, written against the interfaces only.
//
// Each *_step() function is ONE iteration of a task loop. The firmware calls
// them from FreeRTOS tasks (src/main.cpp); the host tests call them from
// std::threads and run them under ThreadSanitizer.
//
//   Sensor task  --(sample queue)-->  Comm task  --HTTP-->  Flask bridge
//        |                               |
//        +-------(error queue)-----------+---->  Supervisor task
//                                                  | (atomic request flags)
//        <---------- reset sensor / reconnect -----+
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "sm/backoff.h"
#include "sm/interfaces.h"
#include "sm/supervisor.h"
#include "sm/types.h"

namespace sm {

enum class PushResult : uint8_t { Ok, DroppedOldest, Failed };

// Never blocks (timeout 0): the producer must keep its timing even when the
// consumer is stuck. On a full queue the OLDEST item is discarded, because the
// newest reading is the most valuable one for monitoring.
template <typename T>
PushResult push_drop_oldest(IQueue<T>& queue, const T& item) {
    if (queue.send(item, 0)) return PushResult::Ok;
    T discarded;
    const bool dropped = queue.receive(discarded, 0);  // consumer may have emptied a slot meanwhile
    if (!queue.send(item, 0)) return PushResult::Failed;
    return dropped ? PushResult::DroppedOldest : PushResult::Ok;
}

// Cross-task requests. The supervisor never touches the sensor or Wi-Fi
// itself: it asks the OWNING task, so each resource has exactly one owner.
struct Requests {
    std::atomic<bool> reset_sensor{false};
    std::atomic<bool> reconnect_wifi{false};
};

// Counters other tasks may read (e.g. for a status display).
struct Stats {
    std::atomic<uint32_t> produced{0};
    std::atomic<uint32_t> dropped{0};  // dropped on the device, not yet reported to the server
    std::atomic<uint32_t> dropped_total{0};
    std::atomic<uint32_t> sent{0};
};

// ---------------------------------------------------------------- Sensor task
struct SensorTask {
    ISensor& sensor;
    IQueue<Sample>& samples;
    IQueue<ErrorEvent>& errors;
    IClock& clock;
    HeartbeatTable& heartbeats;
    Requests& requests;
    Stats& stats;
    uint32_t next_seq = 1;
};

void sensor_step(SensorTask& t);

// ------------------------------------------------------------------ Comm task
struct CommTask {
    static constexpr size_t kMaxBatch = 10;
    static constexpr size_t kBufferSize = 1024;

    IQueue<Sample>& samples;
    IQueue<ErrorEvent>& errors;
    INetwork& network;
    IHttpClient& http;
    IClock& clock;
    HeartbeatTable& heartbeats;
    Requests& requests;
    Stats& stats;
    const char* device_id;
    uint32_t batch_wait_ms = 1000;  // max wait for the first sample of a batch
    Backoff backoff{500, 30000};

    // A batch that failed to send is kept and retried (store-and-forward),
    // so a short outage loses nothing. Meanwhile the queue absorbs new data.
    Sample batch[kMaxBatch] = {};
    size_t batch_len = 0;
    uint32_t batch_dropped = 0;
    char buffer[kBufferSize] = {};
};

// Returns how long the task should sleep before the next step (0 = go on).
uint32_t comm_step(CommTask& t);

// ------------------------------------------------------------ Supervisor task
struct SupervisorTask {
    IQueue<ErrorEvent>& errors;
    Supervisor& supervisor;
    HeartbeatTable& heartbeats;
    Requests& requests;
    IPlatform& platform;
    IClock& clock;
    Stats& stats;
    Action last_action = Action::None;
    // Success is detected from the progress counters, so the healthy path
    // costs the other tasks nothing (no "I'm OK" messages in the queue).
    uint32_t last_produced = 0;
    uint32_t last_sent = 0;
};

// Waits up to wait_ms for an error event, applies the policy, checks heartbeats.
void supervisor_step(SupervisorTask& t, uint32_t wait_ms);

// Sleeps in slices of at most slice_ms and keeps sending heartbeats, so a long
// (but intended) backoff is never mistaken for a stalled task.
void sleep_with_heartbeat(IClock& clock, HeartbeatTable& heartbeats, TaskId task, uint32_t total_ms,
                          uint32_t slice_ms = 1000);

}  // namespace sm
