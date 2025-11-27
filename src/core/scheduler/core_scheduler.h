#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <iostream>

class CoreScheduler {
public:
    using Clock      = std::chrono::steady_clock;
    using Duration   = std::chrono::milliseconds;
    using TimePoint  = Clock::time_point;
    using TaskFn     = std::function<void()>;

    struct Task {
        std::string name;
        Duration interval;
        TaskFn fn;
        TimePoint next_run;
    };

    CoreScheduler()
        : running_(false) {}

    ~CoreScheduler() {
        stop();
    }

    // Add a periodic task (thread-safe, can be called before or after start())
    void addTask(const std::string& name, Duration interval, TaskFn fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        Task t;
        t.name     = name;
        t.interval = interval;
        t.fn       = std::move(fn);
        t.next_run = Clock::now() + interval;
        tasks_.push_back(std::move(t));
        cv_.notify_all();
    }

    // Start background thread if not already running
    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return; // already running
        }

        worker_ = std::thread([this]() {
            runLoop();
        });
    }

    // Stop background thread and wait for it to exit
    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return; // not running
        }

        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void runLoop() {
        std::unique_lock<std::mutex> lock(mutex_);

        while (running_) {
            if (tasks_.empty()) {
                cv_.wait(lock, [this]() {
                    return !running_ || !tasks_.empty();
                });
                if (!running_) break;
            }

            // find next due time
            TimePoint now = Clock::now();
            TimePoint next_due = now + std::chrono::hours(24);
            for (auto& t : tasks_) {
                if (t.next_run < next_due) {
                    next_due = t.next_run;
                }
            }

            // wait until next_due or stop
            if (cv_.wait_until(lock, next_due, [this]() { return !running_; })) {
                break; // stopped
            }

            now = Clock::now();
            for (auto& t : tasks_) {
                if (t.next_run <= now) {
                    // run task without holding lock
                    auto fn   = t.fn;
                    auto name = t.name;
                    t.next_run = now + t.interval;

                    lock.unlock();
                    try {
                        fn();
                    } catch (const std::exception& e) {
                        std::cerr << "[CoreScheduler] Task '" << name
                                  << "' threw: " << e.what() << std::endl;
                    } catch (...) {
                        std::cerr << "[CoreScheduler] Task '" << name
                                  << "' threw unknown exception" << std::endl;
                    }
                    lock.lock();
                }
            }
        }
    }

    std::atomic<bool> running_;
    std::vector<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
};
