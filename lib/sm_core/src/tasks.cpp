#include "sm/tasks.h"

#include "sm/json_encoder.h"

namespace sm {

namespace {

void report(IQueue<ErrorEvent>& errors, ErrorSource source, ErrorCode code, uint32_t now_ms) {
    // Error reporting must never block the reporting task either.
    push_drop_oldest(errors, ErrorEvent{source, code, now_ms});
}

ErrorCode to_error(SensorStatus status) {
    switch (status) {
        case SensorStatus::BusBusy: return ErrorCode::SensorBusBusy;
        case SensorStatus::CrcError: return ErrorCode::SensorCrc;
        case SensorStatus::Nack:
        case SensorStatus::Ok: break;
    }
    return ErrorCode::SensorNack;
}

}  // namespace

// ---------------------------------------------------------------- Sensor task
void sensor_step(SensorTask& t) {
    t.heartbeats.beat(TaskId::Sensor, t.clock.now_ms());

    if (t.requests.reset_sensor.exchange(false)) {
        const SensorStatus status = t.sensor.reset();
        if (status != SensorStatus::Ok) report(t.errors, ErrorSource::Sensor, to_error(status), t.clock.now_ms());
    }

    Reading reading{};
    const uint32_t acquired_at = t.clock.now_ms();
    const SensorStatus status = t.sensor.measure(reading);
    if (status != SensorStatus::Ok) {
        report(t.errors, ErrorSource::Sensor, to_error(status), t.clock.now_ms());
        return;
    }

    const Sample sample{t.next_seq++, acquired_at, reading.temperature_c, reading.humidity_pct};
    const PushResult result = push_drop_oldest(t.samples, sample);
    if (result != PushResult::Failed) t.stats.produced.fetch_add(1, std::memory_order_relaxed);

    if (result == PushResult::DroppedOldest || result == PushResult::Failed) {
        t.stats.dropped_total.fetch_add(1, std::memory_order_relaxed);
        // Report only the FIRST drop of an outage, not every single one,
        // otherwise a long outage would flood the error queue.
        if (t.stats.dropped.fetch_add(1, std::memory_order_relaxed) == 0)
            report(t.errors, ErrorSource::Sensor, ErrorCode::QueueOverflow, t.clock.now_ms());
    }
}

// ------------------------------------------------------------------ Comm task
uint32_t comm_step(CommTask& t) {
    t.heartbeats.beat(TaskId::Comm, t.clock.now_ms());

    if (t.requests.reconnect_wifi.exchange(false)) t.network.reconnect();

    if (!t.network.connected()) {
        report(t.errors, ErrorSource::Comm, ErrorCode::WifiDown, t.clock.now_ms());
        return t.backoff.next_delay_ms();
    }

    if (t.batch_len == 0) {
        Sample s{};
        if (!t.samples.receive(s, t.batch_wait_ms)) return 0;  // nothing to send yet
        t.batch[t.batch_len++] = s;
        while (t.batch_len < CommTask::kMaxBatch && t.samples.receive(s, 0)) t.batch[t.batch_len++] = s;
        t.batch_dropped = t.stats.dropped.exchange(0, std::memory_order_relaxed);
    }

    const size_t len = encode_batch(t.device_id, t.batch, t.batch_len, t.batch_dropped, t.buffer, sizeof(t.buffer));
    if (len == 0) {  // cannot happen with kMaxBatch/kBufferSize as configured; never get stuck on it
        t.batch_len = 0;
        return 0;
    }

    const int code = t.http.post_json(t.buffer, len);
    if (code >= 200 && code < 300) {
        t.stats.sent.fetch_add(static_cast<uint32_t>(t.batch_len), std::memory_order_relaxed);
        t.batch_len = 0;
        t.batch_dropped = 0;
        t.backoff.reset();
        return 0;
    }

    report(t.errors, ErrorSource::Comm, ErrorCode::HttpFailed, t.clock.now_ms());
    return t.backoff.next_delay_ms();  // batch is kept and retried
}

void sleep_with_heartbeat(IClock& clock, HeartbeatTable& heartbeats, TaskId task, uint32_t total_ms,
                          uint32_t slice_ms) {
    while (total_ms > 0) {
        const uint32_t step = total_ms < slice_ms ? total_ms : slice_ms;
        clock.delay_ms(step);
        total_ms -= step;
        heartbeats.beat(task, clock.now_ms());
    }
}

// ------------------------------------------------------------ Supervisor task
namespace {
void apply(SupervisorTask& t, Action action) {
    if (action == Action::None) return;
    t.last_action = action;
    switch (action) {
        case Action::ResetSensor: t.requests.reset_sensor.store(true); break;
        case Action::ReconnectWifi: t.requests.reconnect_wifi.store(true); break;
        case Action::RestartDevice: t.platform.restart(); break;
        case Action::None: break;
    }
}
}  // namespace

void supervisor_step(SupervisorTask& t, uint32_t wait_ms) {
    const uint32_t produced = t.stats.produced.load(std::memory_order_relaxed);
    if (produced != t.last_produced) {
        t.last_produced = produced;
        t.supervisor.on_sensor_ok();
    }
    const uint32_t sent = t.stats.sent.load(std::memory_order_relaxed);
    if (sent != t.last_sent) {
        t.last_sent = sent;
        t.supervisor.on_comm_ok();
    }

    ErrorEvent event{};
    if (t.errors.receive(event, wait_ms)) apply(t, t.supervisor.on_error(event));

    apply(t, t.supervisor.check_heartbeats(t.heartbeats, t.clock.now_ms()));
}

}  // namespace sm
