#include <gtest/gtest.h>

#include "fakes.h"
#include "sm/sht3x.h"

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;
using sm::SensorStatus;
using sm::Sht3x;

namespace {

class Sht3xTest : public ::testing::Test {
protected:
    host::EventLog log;
    host::FakeLock lock{log};
    host::FakeClock clock{&log};
    ::testing::StrictMock<host::MockI2cBus> bus;
    Sht3x sensor{bus, lock, clock};

    // Builds a valid 6-byte frame: T msb, T lsb, CRC, RH msb, RH lsb, CRC.
    static void frame(uint16_t raw_t, uint16_t raw_rh, uint8_t* out) {
        out[0] = raw_t >> 8;
        out[1] = raw_t & 0xFF;
        out[2] = Sht3x::crc8(out, 2);
        out[3] = raw_rh >> 8;
        out[4] = raw_rh & 0xFF;
        out[5] = Sht3x::crc8(out + 3, 2);
    }

    void expect_command(uint8_t msb, uint8_t lsb, bool ack = true) {
        EXPECT_CALL(bus, write(0x44, _, 2)).WillOnce(Invoke([=](uint8_t, const uint8_t* d, size_t) {
            EXPECT_EQ(d[0], msb);
            EXPECT_EQ(d[1], lsb);
            return ack;
        }));
    }
};

TEST(Sht3xCrc, MatchesDatasheetExample) {
    const uint8_t data[2] = {0xBE, 0xEF};
    EXPECT_EQ(Sht3x::crc8(data, 2), 0x92);
}

TEST(Sht3xConversion, UsesDatasheetFormulas) {
    EXPECT_FLOAT_EQ(Sht3x::raw_to_celsius(0), -45.0f);
    EXPECT_FLOAT_EQ(Sht3x::raw_to_celsius(0xFFFF), 130.0f);
    EXPECT_NEAR(Sht3x::raw_to_celsius(0x6666), 25.0f, 0.01f);
    EXPECT_NEAR(Sht3x::raw_to_humidity(0x8000), 50.0f, 0.01f);
}

TEST_F(Sht3xTest, MeasureSendsCommandWaitsAndConverts) {
    InSequence seq;
    expect_command(0x24, 0x00);
    EXPECT_CALL(bus, read(0x44, _, 6)).WillOnce(Invoke([](uint8_t, uint8_t* d, size_t) {
        frame(0x6666, 0x8000, d);
        return true;
    }));

    sm::Reading r{};
    ASSERT_EQ(sensor.measure(r), SensorStatus::Ok);
    EXPECT_NEAR(r.temperature_c, 25.0f, 0.01f);
    EXPECT_NEAR(r.humidity_pct, 50.0f, 0.01f);
}

TEST_F(Sht3xTest, BusIsReleasedDuringConversionTime) {
    expect_command(0x24, 0x00);
    EXPECT_CALL(bus, read(_, _, _)).WillOnce(Invoke([](uint8_t, uint8_t* d, size_t) {
        frame(1, 1, d);
        return true;
    }));
    sm::Reading r{};
    ASSERT_EQ(sensor.measure(r), SensorStatus::Ok);
    const std::vector<std::string> expected{"lock", "unlock", "delay 16", "lock", "unlock"};
    EXPECT_EQ(log.events, expected);
    EXPECT_FALSE(lock.held);
}

TEST_F(Sht3xTest, ReportsBusBusyWhenMutexTimesOut) {
    lock.busy = true;
    sm::Reading r{};
    EXPECT_EQ(sensor.measure(r), SensorStatus::BusBusy);  // StrictMock: bus never touched
}

TEST_F(Sht3xTest, NackOnCommandReleasesLock) {
    expect_command(0x24, 0x00, /*ack=*/false);
    sm::Reading r{};
    EXPECT_EQ(sensor.measure(r), SensorStatus::Nack);
    EXPECT_FALSE(lock.held);
}

TEST_F(Sht3xTest, CorruptedFrameIsRejected) {
    expect_command(0x24, 0x00);
    EXPECT_CALL(bus, read(_, _, _)).WillOnce(Invoke([](uint8_t, uint8_t* d, size_t) {
        frame(0x6666, 0x8000, d);
        d[4] ^= 0x01;  // single bit flip on the bus
        return true;
    }));
    sm::Reading r{};
    EXPECT_EQ(sensor.measure(r), SensorStatus::CrcError);
}

TEST_F(Sht3xTest, ResetSendsSoftResetCommand) {
    expect_command(0x30, 0xA2);
    EXPECT_EQ(sensor.reset(), SensorStatus::Ok);
    EXPECT_FALSE(lock.held);
}

}  // namespace
