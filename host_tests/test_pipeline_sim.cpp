// Runs the REAL task code on three std::threads, like the three FreeRTOS
// tasks, with a flaky network and a sensor on a shared, mutex-protected bus.
// Built with -fsanitize=thread in CI, so any data race fails the build.
#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "fakes.h"
#include "sm/tasks.h"
#include "std_port.h"

namespace {

// Thread-safe sensor that also uses the shared bus lock, like the real driver.
class BusSensor : public sm::ISensor {
public:
    explicit BusSensor(sm::ILock& bus) : bus_(bus) {}
    sm::SensorStatus measure(sm::Reading& out) override {
        sm::LockGuard guard(bus_, 20);
        if (!guard.owns()) return sm::SensorStatus::BusBusy;
        const int n = ++calls_;
        if (n % 7 == 0) return sm::SensorStatus::CrcError;  // occasional bus noise
        out = {20.0f + static_cast<float>(n % 10), 40.0f};
        return sm::SensorStatus::Ok;
    }
    sm::SensorStatus reset() override {
        sm::LockGuard guard(bus_, 20);
        return guard.owns() ? sm::SensorStatus::Ok : sm::SensorStatus::BusBusy;
    }

private:
    sm::ILock& bus_;
    std::atomic<int> calls_{0};
};

// Records every sequence number the "server" received; fails while offline.
class FlakyServer : public sm::IHttpClient {
public:
    int post_json(const char* body, size_t len) override {
        if (offline.load()) return -1;
        const std::string s(body, len);
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t pos = s.find("\"seq\":"); pos != std::string::npos; pos = s.find("\"seq\":", pos + 1))
            seqs_.push_back(std::stoul(s.substr(pos + 6)));
        return 200;
    }
    std::vector<unsigned long> seqs() {
        std::lock_guard<std::mutex> lock(mutex_);
        return seqs_;
    }
    std::atomic<bool> offline{false};

private:
    std::mutex mutex_;
    std::vector<unsigned long> seqs_;
};

class ThreadSafeNetwork : public sm::INetwork {
public:
    bool connected() const override { return true; }
    void reconnect() override { reconnects.fetch_add(1); }
    std::atomic<int> reconnects{0};
};

class CountingPlatform : public sm::IPlatform {
public:
    void restart() override { restarts.fetch_add(1); }
    std::atomic<int> restarts{0};
};

TEST(PipelineSimulation, NoSampleIsLostUnaccountedThroughAnOutage) {
    host::StdQueue<sm::Sample> samples(20);
    host::StdQueue<sm::ErrorEvent> errors(16);
    host::StdLock i2c;
    host::SteadyClock clock;
    BusSensor sensor(i2c);
    FlakyServer server;
    ThreadSafeNetwork network;
    CountingPlatform platform;
    sm::HeartbeatTable heartbeats;
    sm::Requests requests;
    sm::Stats stats;
    sm::Supervisor supervisor;

    std::atomic<bool> sensor_running{true}, others_running{true};

    sm::SensorTask sensor_task{sensor, samples, errors, clock, heartbeats, requests, stats};
    std::thread sensor_thread([&] {
        while (sensor_running.load()) {
            sm::sensor_step(sensor_task);
            clock.delay_ms(1);
        }
    });

    sm::CommTask comm_task{samples, errors, network, server, clock, heartbeats, requests, stats, "sim"};
    comm_task.batch_wait_ms = 5;
    comm_task.backoff = sm::Backoff(2, 10);
    std::thread comm_thread([&] {
        while (others_running.load()) {
            const uint32_t wait = sm::comm_step(comm_task);
            if (wait) sm::sleep_with_heartbeat(clock, heartbeats, sm::TaskId::Comm, wait, 5);
        }
    });

    sm::SupervisorTask sup_task{errors, supervisor, heartbeats, requests, platform, clock, stats};
    std::thread sup_thread([&] {
        while (others_running.load()) sm::supervisor_step(sup_task, 5);
    });

    // A second "task" competing for the I2C bus, e.g. a display or an EEPROM.
    std::thread bus_user([&] {
        while (others_running.load()) {
            {
                sm::LockGuard guard(i2c, 20);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            // Without this pause the thread re-takes the mutex immediately and
            // starves the sensor (mutexes are not FIFO-fair). The first version
            // of this test did exactly that: the sensor only got BusBusy, the
            // supervisor escalated to a restart. Kept as a regression guard.
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    server.offline = true;  // network outage: queue fills, drop-oldest kicks in
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    server.offline = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    sensor_running = false;
    sensor_thread.join();
    // Let the comm task drain the queue and the pending batch.
    for (int i = 0; i < 200 && (samples.size() > 0 || comm_task.batch_len > 0); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    others_running = false;
    comm_thread.join();
    sup_thread.join();
    bus_user.join();

    const auto received = server.seqs();
    const uint32_t generated = sensor_task.next_seq - 1;

    // 1. Ordering: sequence numbers arrive strictly increasing, no duplicates.
    for (size_t i = 1; i < received.size(); ++i) ASSERT_LT(received[i - 1], received[i]);

    // 2. Accounting: every generated sample was either delivered or counted as dropped.
    EXPECT_EQ(received.size() + stats.dropped_total.load(), generated);
    EXPECT_EQ(stats.sent.load(), received.size());

    // 3. The outage really caused drops, and the newest data survived.
    EXPECT_GT(stats.dropped_total.load(), 0u);
    ASSERT_FALSE(received.empty());
    EXPECT_EQ(received.back(), generated);

    // 4. Sensor errors were handled by the supervisor; nothing stalled.
    EXPECT_GT(supervisor.total_errors(), 0u);
    EXPECT_EQ(platform.restarts.load(), 0);
}

}  // namespace
