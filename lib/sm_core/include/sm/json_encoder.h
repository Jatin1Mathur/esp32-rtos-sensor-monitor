// Encodes a batch of samples into a caller-provided buffer.
// No heap allocation (no String, no std::string): avoids heap fragmentation
// on a device that runs for weeks.
#pragma once

#include <cstddef>
#include <cstdint>

#include "sm/types.h"

namespace sm {

// Output:
// {"device":"m5-01","dropped":0,"samples":[{"seq":1,"t":1000,"temp":21.50,"rh":40.00}]}
// Returns the number of bytes written (without '\0'), or 0 if it does not fit.
size_t encode_batch(const char* device_id, const Sample* samples, size_t count, uint32_t dropped, char* buf,
                    size_t cap);

}  // namespace sm
