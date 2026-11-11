// Hardware / OS abstraction interfaces.
// The core logic only talks to these interfaces. The firmware provides
// FreeRTOS + Arduino implementations (src/platform/), the host tests provide
// fakes and std::thread-based implementations (host_tests/support/).
#pragma once

#include <cstddef>
#include <cstdint>

#include "sm/types.h"

namespace sm {

// Mirrors FreeRTOS queue semantics: copy in, copy out, bounded, with timeouts.
template <typename T>
class IQueue {
public:
    virtual ~IQueue() = default;
    virtual bool send(const T& item, uint32_t timeout_ms) = 0;
    virtual bool receive(T& item, uint32_t timeout_ms) = 0;
    virtual size_t size() const = 0;
    virtual size_t capacity() const = 0;
};

// A mutex that can only be taken with a timeout (never wait forever).
class ILock {
public:
    virtual ~ILock() = default;
    virtual bool try_lock_for(uint32_t timeout_ms) = 0;
    virtual void unlock() = 0;
};

// RAII guard: the lock is released on every return path.
class LockGuard {
public:
    LockGuard(ILock& lock, uint32_t timeout_ms) : lock_(lock), owns_(lock.try_lock_for(timeout_ms)) {}
    ~LockGuard() {
        if (owns_) lock_.unlock();
    }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
    bool owns() const { return owns_; }

private:
    ILock& lock_;
    bool owns_;
};

class II2cBus {
public:
    virtual ~II2cBus() = default;
    virtual bool write(uint8_t addr, const uint8_t* data, size_t len) = 0;
    virtual bool read(uint8_t addr, uint8_t* data, size_t len) = 0;
};

class IClock {
public:
    virtual ~IClock() = default;
    virtual uint32_t now_ms() const = 0;
    virtual void delay_ms(uint32_t ms) = 0;  // yields to other tasks
};

class ISensor {
public:
    virtual ~ISensor() = default;
    virtual SensorStatus measure(Reading& out) = 0;
    virtual SensorStatus reset() = 0;
};

class INetwork {
public:
    virtual ~INetwork() = default;
    virtual bool connected() const = 0;
    virtual void reconnect() = 0;
};

class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    // Returns the HTTP status code, or a negative value on transport error.
    virtual int post_json(const char* body, size_t len) = 0;
};

class IPlatform {
public:
    virtual ~IPlatform() = default;
    virtual void restart() = 0;
};

}  // namespace sm
