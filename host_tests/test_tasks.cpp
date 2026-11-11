// Single-threaded tests of one iteration of each task, with fakes.
#include <gtest/gtest.h>

#include "fakes.h"
#include "sm/json_encoder.h"
#include "sm/tasks.h"
#include "std_port.h"

using sm::Action;
using sm::ErrorCode;
using sm::PushResult;

namespace {

// ------------------------------------------------------------ queue policy
TEST(PushDropOldest, KeepsNewestWhenFull) {
    host::StdQueue<int> q(2);
    EXPECT_EQ(sm::push_drop_oldest(q, 1), PushResult::Ok);
    EXPECT_EQ(sm::push_drop_oldest(q, 2), PushResult::Ok);
    EXPECT_EQ(sm::push_drop_oldest(q, 3), PushResult::DroppedOldest);
    int v = 0;
    ASSERT_TRUE(q.receive(v, 0));
    EXPECT_EQ(v, 2);
    ASSERT_TRUE(q.receive(v, 0));
    EXPECT_EQ(v, 3);
}

// ------------------------------------------------------------ JSON
TEST(JsonEncoder, ProducesExpectedFormat) {
    const sm::Sample s[2] = {{1, 1000, 21.5f, 40.0f}, {2, 2000, -3.25f, 99.5f}};
    char buf[256];
    const size_t n = sm::encode_batch("m5-01", s, 2, 3, buf, sizeof(buf));
    EXPECT_EQ(std::string(buf, n),
              "{\"device\":\"m5-01\",\"dropped\":3,\"samples\":["
              "{\"seq\":1,\"t\":1000,\"temp\":21.50,\"rh\":40.00},"
              "{\"seq\":2,\"t\":2000,\"temp\":-3.25,\"rh\":99.50}]}");
}

TEST(JsonEncoder, ReturnsZeroInsteadOfTruncating) {
    const sm::Sample s{1, 1000, 21.5f, 40.0f};
    char buf[30];
    EXPECT_EQ(sm::encode_batch("m5-01", &s, 1, 0, buf, sizeof(buf)), 0u);
}

TEST(JsonEncoder, FullBatchFitsTheCommBuffer) {
    sm::Sample s[sm::CommTask::kMaxBatch];
    for (auto& x : s) x = {4000000000u, 4000000000u, -45.0f, 100.0f};  // widest values
    char buf[sm::CommTask::kBufferSize];
    EXPECT_GT(sm::encode_batch("device-with-long-name-32-chars!!", s, sm::CommTask::kMaxBatch, 4000000000u, buf,
                               sizeof(buf)),
              0u);
}

// ------------------------------------------------------------ fixtures
struct World {
    host::StdQueue<sm::Sample> samples{3};
    host::StdQueue<sm::ErrorEvent> errors{8};
    host::FakeClock clock;
    host::FakeSensor sensor;
    host::FakeNetwork network;
    host::FakeHttp http;
    host::FakePlatform platform;
    sm::HeartbeatTable heartbeats;
    sm::Requests requests;
    sm::Stats stats;
    sm::Supervisor supervisor;

    sm::SensorTask sensor_task{sensor, samples, errors, clock, heartbeats, requests, stats};
    sm::CommTask comm_task{samples, errors, network, http, clock, heartbeats, requests, stats, "m5-01"};
    sm::SupervisorTask sup_task{errors, supervisor, heartbeats, requests, platform, clock, stats};

    World() { comm_task.batch_wait_ms = 0; }

    std::vector<ErrorCode> drain_errors() {
        std::vector<ErrorCode> out;
        sm::ErrorEvent e{};
        while (errors.receive(e, 0)) out.push_back(e.code);
        return out;
    }
};

// ------------------------------------------------------------ sensor task
TEST(SensorTask, QueuesSampleWithSequenceNumber) {
    World w;
    w.clock.now = 1234;
    sm::sensor_step(w.sensor_task);
    sm::Sample s{};
    ASSERT_TRUE(w.samples.receive(s, 0));
    EXPECT_EQ(s.seq, 1u);
    EXPECT_EQ(s.timestamp_ms, 1234u);
    EXPECT_EQ(w.stats.produced.load(), 1u);
}

TEST(SensorTask, ReportsSensorErrorAndQueuesNothing) {
    World w;
    w.sensor.next_status = sm::SensorStatus::CrcError;
    sm::sensor_step(w.sensor_task);
    EXPECT_EQ(w.samples.size(), 0u);
    EXPECT_EQ(w.drain_errors(), std::vector<ErrorCode>{ErrorCode::SensorCrc});
}

TEST(SensorTask, ReportsOnlyFirstOverflowOfAnOutage) {
    World w;
    for (int i = 0; i < 10; ++i) sm::sensor_step(w.sensor_task);  // queue holds 3
    EXPECT_EQ(w.stats.dropped_total.load(), 7u);
    EXPECT_EQ(w.drain_errors(), std::vector<ErrorCode>{ErrorCode::QueueOverflow});
}

TEST(SensorTask, PerformsResetRequestedBySupervisor) {
    World w;
    w.requests.reset_sensor = true;
    sm::sensor_step(w.sensor_task);
    EXPECT_EQ(w.sensor.reset_calls, 1);
    EXPECT_FALSE(w.requests.reset_sensor.load());
}

// ------------------------------------------------------------ comm task
TEST(CommTask, SendsQueuedSamplesAsOneBatch) {
    World w;
    for (int i = 0; i < 3; ++i) sm::sensor_step(w.sensor_task);
    EXPECT_EQ(sm::comm_step(w.comm_task), 0u);
    ASSERT_EQ(w.http.bodies.size(), 1u);
    EXPECT_NE(w.http.bodies[0].find("\"seq\":3"), std::string::npos);
    EXPECT_EQ(w.stats.sent.load(), 3u);
}

TEST(CommTask, BacksOffWhileWifiIsDown) {
    World w;
    w.network.up = false;
    EXPECT_EQ(sm::comm_step(w.comm_task), 500u);
    EXPECT_EQ(sm::comm_step(w.comm_task), 1000u);
    EXPECT_TRUE(w.http.bodies.empty());
    EXPECT_EQ(w.drain_errors(), (std::vector<ErrorCode>{ErrorCode::WifiDown, ErrorCode::WifiDown}));
}

TEST(CommTask, KeepsAndResendsBatchAfterHttpFailure) {
    World w;
    sm::sensor_step(w.sensor_task);
    w.http.status = 503;
    EXPECT_GT(sm::comm_step(w.comm_task), 0u);
    w.http.status = 200;
    sm::sensor_step(w.sensor_task);  // new sample waits in the queue
    EXPECT_EQ(sm::comm_step(w.comm_task), 0u);
    ASSERT_EQ(w.http.bodies.size(), 2u);
    EXPECT_EQ(w.http.bodies[0], w.http.bodies[1]);  // identical retry, seq 1
    EXPECT_EQ(w.stats.sent.load(), 1u);
    EXPECT_EQ(w.samples.size(), 1u);  // seq 2 goes in the next batch
}

TEST(CommTask, ReportsDroppedCountToServer) {
    World w;
    for (int i = 0; i < 5; ++i) sm::sensor_step(w.sensor_task);  // 2 dropped
    sm::comm_step(w.comm_task);
    EXPECT_NE(w.http.bodies.at(0).find("\"dropped\":2"), std::string::npos);
    EXPECT_EQ(w.stats.dropped.load(), 0u);
}

// ------------------------------------------------------------ supervisor task
TEST(SupervisorTask, AsksSensorTaskToResetInsteadOfTouchingTheBus) {
    World w;
    w.sensor.next_status = sm::SensorStatus::Nack;
    for (int i = 0; i < 3; ++i) {
        sm::sensor_step(w.sensor_task);
        sm::supervisor_step(w.sup_task, 0);
    }
    EXPECT_EQ(w.sup_task.last_action, Action::ResetSensor);
    EXPECT_TRUE(w.requests.reset_sensor.load());
    EXPECT_EQ(w.sensor.reset_calls, 0);  // the supervisor itself never calls the sensor
}

TEST(SupervisorTask, SuccessfulSamplesClearSensorErrors) {
    World w;
    w.sensor.next_status = sm::SensorStatus::Nack;
    for (int i = 0; i < 2; ++i) {
        sm::sensor_step(w.sensor_task);
        sm::supervisor_step(w.sup_task, 0);
    }
    w.sensor.next_status = sm::SensorStatus::Ok;
    sm::sensor_step(w.sensor_task);
    sm::supervisor_step(w.sup_task, 0);
    w.sensor.next_status = sm::SensorStatus::Nack;
    sm::sensor_step(w.sensor_task);
    sm::supervisor_step(w.sup_task, 0);
    EXPECT_EQ(w.sup_task.last_action, Action::None);
}

TEST(SupervisorTask, RestartsDeviceWhenATaskStalls) {
    World w;
    sm::sensor_step(w.sensor_task);
    sm::comm_step(w.comm_task);
    w.clock.now = 10000;
    sm::sensor_step(w.sensor_task);  // only the sensor task is alive
    sm::supervisor_step(w.sup_task, 0);
    EXPECT_EQ(w.platform.restarts, 1);
}

TEST(SleepWithHeartbeat, LongBackoffKeepsTaskAlive) {
    World w;
    sm::sleep_with_heartbeat(w.clock, w.heartbeats, sm::TaskId::Comm, 30000);
    uint32_t last = 0;
    ASSERT_TRUE(w.heartbeats.last_beat(sm::TaskId::Comm, last));
    EXPECT_EQ(last, 30000u);
    EXPECT_EQ(w.clock.now, 30000u);
}

}  // namespace
