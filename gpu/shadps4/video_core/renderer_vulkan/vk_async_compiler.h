// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <memory>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Vulkan {

// gow3: bounded compiler queue. Stop drains accepted urgent and normal work before Vulkan
// resources are released.
class AsyncCompiler {
public:
    /// gow3: a worker takes urgent work first, then normal, then low. Low work (optimised
    /// rebuilds of pipelines already drawing) never takes a slot from the other lanes, runs on
    /// at most half the workers (a worker does not leave a task it started, so the others stay
    /// free for new work) and is dropped on Stop: nothing waits for it.
    enum class Lane { Normal, Urgent, Low };

    explicit AsyncCompiler(unsigned count, size_t capacity = 64,
                           std::function<void(unsigned)> initialize = {})
        : capacity{capacity}, low_limit{std::max(1u, count / 2)} {
        for (unsigned i = 0; i < count; ++i) {
            workers.emplace_back([this, i, initialize] {
                worker_index = i;
                if (initialize) {
                    initialize(i);
                }
                for (;;) {
                    std::function<void()> task;
                    bool is_low = false;
                    {
                        std::unique_lock lock{mutex};
                        const auto low_ready = [&] {
                            return !low.empty() && low_running < low_limit;
                        };
                        cv.wait(lock, [&] {
                            return stopping || !tasks.empty() || !urgent.empty() || low_ready();
                        });
                        if (tasks.empty() && urgent.empty() && !low_ready()) {
                            return; // stopping (Stop empties the low lane)
                        }
                        is_low = urgent.empty() && tasks.empty();
                        auto& queue = !urgent.empty() ? urgent : (!is_low ? tasks : low);
                        auto next = std::move(queue.front());
                        queue.pop_front();
                        if (is_low) {
                            ++low_running; // its long waits stay out of the queue wait stat
                        } else {
                            queue_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - next.enqueued).count();
                        }
                        task = std::move(next.function);
                    }
                    task();
                    if (is_low) {
                        bool more = false;
                        {
                            std::scoped_lock lock{mutex};
                            --low_running;
                            more = !low.empty();
                        }
                        if (more) {
                            cv.notify_one(); // a worker that found the low lane full
                        }
                    }
                }
            });
        }
    }
    ~AsyncCompiler() { Stop(); }

    bool Submit(std::function<void()> task, Lane lane = Lane::Normal) {
        {
            std::scoped_lock lock{mutex};
            auto& queue = lane == Lane::Urgent ? urgent : (lane == Lane::Low ? low : tasks);
            const size_t used = lane == Lane::Low ? low.size() : tasks.size() + urgent.size();
            if (stopping || used >= capacity) {
                return false;
            }
            queue.push_back({std::move(task), std::chrono::steady_clock::now()});
        }
        cv.notify_one();
        return true;
    }
    bool HasCapacity() {
        std::scoped_lock lock{mutex};
        return !stopping && tasks.size() + urgent.size() < capacity;
    }
    static unsigned WorkerIndex() { return worker_index; }
    size_t Workers() const { return workers.size(); }
    size_t Depth() {
        std::scoped_lock lock{mutex};
        return tasks.size() + urgent.size();
    }
    uint64_t QueueWaitNs() { return queue_wait_ns.exchange(0); }
    void Stop() {
        std::deque<Task> dropped;
        {
            std::scoped_lock lock{mutex};
            stopping = true;
            dropped.swap(low);
        }
        dropped.clear(); // their jobs report a broken promise, a failed Poll
        cv.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    size_t capacity;
    unsigned low_limit;
    unsigned low_running = 0;
    std::mutex mutex;
    std::condition_variable cv;
    struct Task {
        std::function<void()> function;
        std::chrono::steady_clock::time_point enqueued;
    };
    std::deque<Task> tasks, urgent, low;
    static inline thread_local unsigned worker_index = 0;
    std::atomic<uint64_t> queue_wait_ns{};
    bool stopping = false;
    std::vector<std::thread> workers;
};

// gow3: one job per cache entry; failures are terminal until the next session.
template <typename T>
class AsyncJob {
public:
    template <typename F>
    bool Start(AsyncCompiler& compiler, F&& function,
               AsyncCompiler::Lane lane = AsyncCompiler::Lane::Normal) {
        if (started) {
            return false;
        }
        auto task = std::make_shared<std::packaged_task<T()>>(std::forward<F>(function));
        auto completion = task->get_future();
        if (!compiler.Submit([task] { (*task)(); }, lane)) {
            return false;
        }
        future = std::move(completion);
        started = true;
        return true;
    }
    bool Pending() const { return future.valid(); }
    bool Failed() const { return failed; }
    const std::string& Error() const { return error; }
    void Wait() const {
        if (future.valid()) {
            future.wait();
        }
    }
    /// Done within `timeout` (#154: the GPU thread waits a little before it skips a draw).
    bool WaitFor(std::chrono::nanoseconds timeout) const {
        return future.valid() && future.wait_for(timeout) == std::future_status::ready;
    }
    std::optional<T> Poll() {
        if (!future.valid() || future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return std::nullopt;
        }
        try {
            return future.get();
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Unknown compiler error";
        }
        failed = true;
        return std::nullopt;
    }

private:
    std::future<T> future;
    bool started = false;
    bool failed = false;
    std::string error;
};

// gow3: IR retains its pools until emission finishes, then returns them for another translation.
template <typename T>
class CompilerPool {
public:
    explicit CompilerPool(size_t capacity) : capacity{capacity} {}
    std::pair<std::unique_ptr<T>, bool> Acquire() {
        std::scoped_lock lock{mutex};
        if (available.empty()) {
            return {std::make_unique<T>(), false};
        }
        auto pool = std::move(available.back());
        available.pop_back();
        return {std::move(pool), true};
    }
    void Recycle(std::unique_ptr<T> pool) noexcept {
        try {
            pool->ReleaseContents();
            std::scoped_lock lock{mutex};
            if (available.size() < capacity) {
                available.push_back(std::move(pool));
            }
        } catch (...) {
            // Reuse is optional; discard the pool if resetting or retaining it fails.
        }
    }
    size_t Retained() {
        std::scoped_lock lock{mutex};
        return available.size();
    }
private:
    size_t capacity;
    std::mutex mutex;
    std::vector<std::unique_ptr<T>> available;
};

} // namespace Vulkan
