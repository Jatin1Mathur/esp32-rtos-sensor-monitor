#include "sm/json_encoder.h"

#include <cstdio>

namespace sm {

namespace {
// Checks the snprintf result and advances the write position.
// Returns false if the output did not fit (snprintf truncated it).
bool advance(int written, size_t cap, size_t& pos) {
    if (written < 0 || static_cast<size_t>(written) >= cap - pos) return false;
    pos += static_cast<size_t>(written);
    return true;
}
}  // namespace

size_t encode_batch(const char* device_id, const Sample* samples, size_t count, uint32_t dropped, char* buf,
                    size_t cap) {
    if (buf == nullptr || cap == 0) return 0;
    size_t pos = 0;

    if (!advance(std::snprintf(buf, cap, "{\"device\":\"%s\",\"dropped\":%lu,\"samples\":[", device_id,
                               static_cast<unsigned long>(dropped)),
                 cap, pos))
        return 0;

    for (size_t i = 0; i < count; ++i) {
        const Sample& s = samples[i];
        if (!advance(std::snprintf(buf + pos, cap - pos, "%s{\"seq\":%lu,\"t\":%lu,\"temp\":%.2f,\"rh\":%.2f}",
                                   i ? "," : "", static_cast<unsigned long>(s.seq),
                                   static_cast<unsigned long>(s.timestamp_ms), static_cast<double>(s.temperature_c),
                                   static_cast<double>(s.humidity_pct)),
                     cap, pos))
            return 0;
    }

    if (!advance(std::snprintf(buf + pos, cap - pos, "]}"), cap, pos)) return 0;
    return pos;
}

}  // namespace sm
