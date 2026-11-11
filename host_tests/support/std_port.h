// Host (Linux/macOS) implementations of the core interfaces with the same
// semantics as their FreeRTOS counterparts, so the task code can run on real
// threads under ThreadSanitizer.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "sm/interfaces.h"

namespace host {

template <typename T>
class StdQueue : public sm::IQueue<T> {
public:
    explicit StdQueue(size_t capacity) : capacity_(capacity) {}

    bool send(const T& item, uint32_t timeout_ms) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_full_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [&] { return items_.size() < capacity_; }))
            return false;
        items_.push_back(item);
        not_empty_.notify_one();
        return true;
    }
    bool receive(T& item, uint32_t timeout_ms) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !items_.empty(); }))
            return false;
        item = items_.front();
        items_.pop_front();
        not_full_.notify_one();
        return true;
    }
    size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }
    size_t capacity() const override { return capacity_; }

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_, not_full_;
    std::deque<T> items_;
};

// Timed lock built from mutex + condition_variable. (std::timed_mutex would be
// simpler, but GCC implements it with pthread_mutex_clocklock, which
// ThreadSanitizer does not intercept -> false "unlock of unlocked mutex".)
class StdLock : public sm::ILock {
public:
    bool try_lock_for(uint32_t timeout_ms) override {
        std::unique_lock<std::mutex> guard(mutex_);
        if (!released_.wait_for(guard, std::chrono::milliseconds(timeout_ms), [&] { return !locked_; }))
            return false;
        locked_ = true;
        return true;
    }
    void unlock() override {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            locked_ = false;
        }
        released_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable released_;
    bool locked_ = false;
};

class SteadyClock : public sm::IClock {
public:
    uint32_t now_ms() const override {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - start_)
                                         .count());
    }
    void delay_ms(uint32_t ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

}  // namespace host
