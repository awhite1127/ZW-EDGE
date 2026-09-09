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

// 单消费者保持已接受样本的顺序；满载拒绝新任务，绝不阻塞实时采集。
// 下一项接受的任务携带缺口标记，消费方不得跨缺口累计连续样本。
class OrderedTaskQueue {
public:
    ~OrderedTaskQueue() { stop(); }
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = true;
        try { worker_ = std::thread([this] { run(); }); }
        catch (...) { accepting_ = false; throw; }
    }
    bool submit(std::function<void(bool)> task, std::size_t points) {
        const auto weight = std::max<std::size_t>(1, points);
        std::unique_lock<std::mutex> lock(mutex_);
        if (!accepting_) return false;
        if (tasks_.size() >= 128 || weight > 16384 - queued_points_) {
            gap_pending_ = true;
            return false;
        }
        const bool gap = gap_pending_;
        tasks_.push_back({[task = std::move(task), gap] { task(gap); }, weight});
        gap_pending_ = false;
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
    bool gap_pending_{false};
    std::thread worker_;
};

}  // namespace edge_controller
