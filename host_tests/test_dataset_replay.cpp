// Feeds the REAL recorded dataset (data/office_room_2015.csv) through the
// sensor -> queue -> comm -> HTTP pipeline, including a network outage, and
// checks that every value the server receives is exactly the recorded value.
#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "fakes.h"
#include "sm/replay_sensor.h"
#include "sm/tasks.h"
#include "std_port.h"

#ifndef DATASET_CSV
#error "DATASET_CSV must point to data/office_room_2015.csv"
#endif

namespace {

std::vector<sm::Reading> load_dataset() {
    std::ifstream file(DATASET_CSV);
    std::vector<sm::Reading> rows;
    std::string line;
    std::getline(file, line);  // header
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string stamp, temp, rh;
        std::getline(ss, stamp, ',');
        std::getline(ss, temp, ',');
        std::getline(ss, rh, ',');
        rows.push_back({std::stof(temp), std::stof(rh)});
    }
    return rows;
}

// Extracts seq -> (temp, rh) from the JSON bodies the "server" received.
std::map<unsigned long, sm::Reading> parse_bodies(const std::vector<std::string>& bodies) {
    std::map<unsigned long, sm::Reading> out;
    for (const auto& body : bodies) {
        for (size_t p = body.find("{\"seq\":"); p != std::string::npos; p = body.find("{\"seq\":", p + 1)) {
            unsigned long seq = 0, t = 0;
            float temp = 0, rh = 0;
            if (std::sscanf(body.c_str() + p, "{\"seq\":%lu,\"t\":%lu,\"temp\":%f,\"rh\":%f}", &seq, &t, &temp, &rh) ==
                4)
                out[seq] = {temp, rh};
        }
    }
    return out;
}

TEST(ReplaySensor, PlaysBackInOrderAndLoops) {
    const sm::Reading data[2] = {{20.0f, 30.0f}, {21.0f, 31.0f}};
    sm::ReplaySensor sensor(data, 2);
    sm::Reading r{};
    for (float expected : {20.0f, 21.0f, 20.0f}) {
        ASSERT_EQ(sensor.measure(r), sm::SensorStatus::Ok);
        EXPECT_FLOAT_EQ(r.temperature_c, expected);
    }
}

TEST(ReplaySensor, ReportsNackWhenDataEndsWithoutLoop) {
    const sm::Reading data[1] = {{20.0f, 30.0f}};
    sm::ReplaySensor sensor(data, 1, /*loop=*/false);
    sm::Reading r{};
    EXPECT_EQ(sensor.measure(r), sm::SensorStatus::Ok);
    EXPECT_EQ(sensor.measure(r), sm::SensorStatus::Nack);
    EXPECT_TRUE(sensor.finished());
}

TEST(DatasetReplay, DatasetIsLoadedAndPlausible) {
    const auto rows = load_dataset();
    ASSERT_EQ(rows.size(), 2665u);
    for (const auto& r : rows) {
        EXPECT_GT(r.temperature_c, 15.0f);
        EXPECT_LT(r.temperature_c, 30.0f);
        EXPECT_GE(r.humidity_pct, 0.0f);
        EXPECT_LE(r.humidity_pct, 100.0f);
    }
}

TEST(DatasetReplay, RealDataArrivesUnchangedThroughAnOutage) {
    const auto rows = load_dataset();
    sm::ReplaySensor sensor(rows.data(), rows.size(), /*loop=*/false);

    host::StdQueue<sm::Sample> samples(60);
    host::StdQueue<sm::ErrorEvent> errors(16);
    host::FakeClock clock;
    host::FakeNetwork network;
    host::FakeHttp server;
    sm::HeartbeatTable heartbeats;
    sm::Requests requests;
    sm::Stats stats;

    sm::SensorTask sensor_task{sensor, samples, errors, clock, heartbeats, requests, stats};
    sm::CommTask comm{samples, errors, network, server, clock, heartbeats, requests, stats, "replay"};
    comm.batch_wait_ms = 0;

    // Simulated time: one sample per tick; comm runs every 5 ticks.
    // Wi-Fi is down for 200 ticks (more than the 60-slot queue can hold).
    for (uint32_t tick = 0; !sensor.finished(); ++tick) {
        clock.now = tick * 1000;
        network.up = !(tick >= 1000 && tick < 1200);
        sm::sensor_step(sensor_task);
        if (tick % 5 == 0) sm::comm_step(comm);
    }
    network.up = true;
    for (int i = 0; i < 1000 && (samples.size() > 0 || comm.batch_len > 0); ++i) sm::comm_step(comm);

    const auto received = parse_bodies(server.bodies);
    const uint32_t generated = sensor_task.next_seq - 1;
    ASSERT_EQ(generated, rows.size());

    // Nothing lost without being counted.
    EXPECT_EQ(received.size() + stats.dropped_total.load(), rows.size());
    EXPECT_GT(stats.dropped_total.load(), 0u);  // the outage really overflowed the queue

    // Every delivered value is exactly the recorded one (seq n = CSV row n).
    for (const auto& [seq, reading] : received) {
        const auto& expected = rows.at(seq - 1);
        EXPECT_NEAR(reading.temperature_c, expected.temperature_c, 0.005f) << "seq " << seq;
        EXPECT_NEAR(reading.humidity_pct, expected.humidity_pct, 0.005f) << "seq " << seq;
    }
    // The newest data is what survives an outage, and the last row arrives.
    EXPECT_EQ(received.rbegin()->first, rows.size());
}

}  // namespace
