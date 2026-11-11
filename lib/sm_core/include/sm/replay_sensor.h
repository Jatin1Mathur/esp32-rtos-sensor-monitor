// A sensor that plays back recorded real-world readings instead of talking
// to hardware. It implements the same ISensor interface as the SHT3x driver,
// so the tasks, queues, supervisor and bridge run unchanged.
//
// Used when no M5Stack/SHT3x is available (firmware env "replay") and in the
// host tests, which feed the real dataset through the whole pipeline.
#pragma once

#include <cstddef>

#include "sm/interfaces.h"

namespace sm {

class ReplaySensor : public ISensor {
public:
    ReplaySensor(const Reading* readings, size_t count, bool loop = true)
        : readings_(readings), count_(count), loop_(loop) {}

    SensorStatus measure(Reading& out) override {
        if (count_ == 0 || (!loop_ && index_ >= count_)) return SensorStatus::Nack;  // "sensor gone"
        out = readings_[index_ % count_];
        ++index_;
        return SensorStatus::Ok;
    }

    SensorStatus reset() override { return SensorStatus::Ok; }

    size_t position() const { return index_; }
    bool finished() const { return !loop_ && index_ >= count_; }

private:
    const Reading* readings_;
    size_t count_;
    bool loop_;
    size_t index_ = 0;
};

}  // namespace sm
