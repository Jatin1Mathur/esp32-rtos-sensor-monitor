// Exponential backoff for retrying the network: 500 ms, 1 s, 2 s, ... capped.
// Prevents a dead server from making the device hammer Wi-Fi and the CPU.
#pragma once

#include <cstdint>

namespace sm {

class Backoff {
public:
    Backoff(uint32_t base_ms, uint32_t max_ms) : base_ms_(base_ms), max_ms_(max_ms) {}

    uint32_t next_delay_ms() {
        uint32_t delay = base_ms_;
        for (uint32_t i = 0; i < attempts_ && delay < max_ms_; ++i) delay *= 2;
        if (delay > max_ms_) delay = max_ms_;
        if (attempts_ < 32) ++attempts_;
        return delay;
    }
    void reset() { attempts_ = 0; }
    uint32_t attempts() const { return attempts_; }

private:
    uint32_t base_ms_;
    uint32_t max_ms_;
    uint32_t attempts_ = 0;
};

}  // namespace sm
