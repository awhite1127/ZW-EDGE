#pragma once

#include <condition_variable>
#include <algorithm>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include "shared/common/logger.h"

namespace edge_controller {

// 单消费者保持样本顺序；容量耗尽时背压，关闭时排空已接受任务。
class OrderedTaskQueue {
public:
    ~OrderedTaskQueue() { stop(); }
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = true;
        try { worker_ = std::thread([this] { run(); }); }
        catch (...) { accepting_ = false; throw; }
    }
    bool submit(std::function<void()> task, std::size_t points) {
        const auto weight = std::min<std::size_t>(16384, std::max<std::size_t>(1, points));
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this, weight] { return !accepting_ || (tasks_.size() < 128 && queued_points_ + weight <= 16384); });
        if (!accepting_) return false;
        tasks_.push_back({std::move(task), weight});
        queued_points_ += weight;
        changed_.notify_all();
        return true;
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
        }
        changed_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] { return !accepting_ || !tasks_.empty(); });
                if (tasks_.empty()) return;
                task = std::move(tasks_.front().first);
                queued_points_ -= tasks_.front().second;
                tasks_.pop_front();
            }
            changed_.notify_all();
            try { task(); }
            catch (const std::exception& error) { Logger::error("采集持久化任务异常：" + std::string(error.what())); }
            catch (...) { Logger::error("采集持久化任务发生未知异常"); }
        }
    }
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::pair<std::function<void()>, std::size_t>> tasks_;
    std::size_t queued_points_{0};
    bool accepting_{false};
    std::thread worker_;
};

}  // namespace edge_controller
