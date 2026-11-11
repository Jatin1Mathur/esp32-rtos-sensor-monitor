#include <gtest/gtest.h>

#include "sm/backoff.h"
#include "sm/supervisor.h"

using sm::Action;
using sm::ErrorCode;
using sm::ErrorEvent;
using sm::ErrorSource;
using sm::TaskId;

namespace {

ErrorEvent sensor_err() { return {ErrorSource::Sensor, ErrorCode::SensorNack, 0}; }
ErrorEvent comm_err() { return {ErrorSource::Comm, ErrorCode::HttpFailed, 0}; }

TEST(Supervisor, ResetsSensorAfterConsecutiveErrors) {
    sm::Supervisor sup;
    EXPECT_EQ(sup.on_error(sensor_err()), Action::None);
    EXPECT_EQ(sup.on_error(sensor_err()), Action::None);
    EXPECT_EQ(sup.on_error(sensor_err()), Action::ResetSensor);
}

TEST(Supervisor, SuccessClearsTheErrorCount) {
    sm::Supervisor sup;
    sup.on_error(sensor_err());
    sup.on_error(sensor_err());
    sup.on_sensor_ok();
    EXPECT_EQ(sup.on_error(sensor_err()), Action::None);
}

TEST(Supervisor, EscalatesToRestartWhenResetsDoNotHelp) {
    sm::Supervisor sup;
    Action last = Action::None;
    for (int i = 0; i < 9; ++i) last = sup.on_error(sensor_err());
    EXPECT_EQ(last, Action::RestartDevice);
    EXPECT_EQ(sup.sensor_resets(), 3);
}

TEST(Supervisor, ReconnectsWifiAfterRepeatedCommErrors) {
    sm::Supervisor sup;
    sup.on_error(comm_err());
    sup.on_error(comm_err());
    EXPECT_EQ(sup.on_error(comm_err()), Action::ReconnectWifi);
    EXPECT_EQ(sup.on_error(comm_err()), Action::None);  // counter starts again
}

TEST(Supervisor, QueueOverflowIsCountedButNotEscalated) {
    sm::Supervisor sup;
    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(sup.on_error({ErrorSource::Sensor, ErrorCode::QueueOverflow, 0}), Action::None);
    EXPECT_EQ(sup.dropped_samples_reported(), 10u);
}

TEST(Heartbeats, NoAlarmBeforeFirstBeat) {
    sm::Supervisor sup;
    sm::HeartbeatTable table;
    EXPECT_EQ(sup.check_heartbeats(table, 100000), Action::None);
}

TEST(Heartbeats, DetectsStalledTask) {
    sm::Supervisor sup;  // timeout 5000 ms
    sm::HeartbeatTable table;
    table.beat(TaskId::Sensor, 1000);
    table.beat(TaskId::Comm, 1000);
    EXPECT_EQ(sup.check_heartbeats(table, 6000), Action::None);
    table.beat(TaskId::Sensor, 6000);
    TaskId stalled = TaskId::Sensor;
    EXPECT_EQ(sup.check_heartbeats(table, 6001, &stalled), Action::RestartDevice);
    EXPECT_EQ(stalled, TaskId::Comm);
}

TEST(Heartbeats, SurvivesTimestampWrapAround) {
    sm::Supervisor sup;
    sm::HeartbeatTable table;
    const uint32_t before_wrap = 0x7FFFFF00u;  // just before the 31-bit wrap
    table.beat(TaskId::Sensor, before_wrap);
    table.beat(TaskId::Comm, before_wrap);
    EXPECT_EQ(sup.check_heartbeats(table, before_wrap + 1000u), Action::None);
    EXPECT_EQ(sup.check_heartbeats(table, before_wrap + 6000u), Action::RestartDevice);
}

TEST(Backoff, DoublesUpToTheCap) {
    sm::Backoff b(500, 4000);
    EXPECT_EQ(b.next_delay_ms(), 500u);
    EXPECT_EQ(b.next_delay_ms(), 1000u);
    EXPECT_EQ(b.next_delay_ms(), 2000u);
    EXPECT_EQ(b.next_delay_ms(), 4000u);
    EXPECT_EQ(b.next_delay_ms(), 4000u);
    b.reset();
    EXPECT_EQ(b.next_delay_ms(), 500u);
}

TEST(Backoff, DoesNotOverflowAfterManyAttempts) {
    sm::Backoff b(500, 30000);
    for (int i = 0; i < 1000; ++i) EXPECT_LE(b.next_delay_ms(), 30000u);
}

}  // namespace
