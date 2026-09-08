#pragma once
#include "application/service/polling_service.h"
#include <memory>
#include <utility>
namespace edge_controller {
// 所有成员访问由调用者的配置锁串行化。detach 后返回的租约负责锁外 stop/join；
// 校时通过弱引用获取临时所有权，不再依赖跨解锁区间的裸指针。
class PollingRuntime {
public:
    PollingService* get() const { return active_.get(); }
    void install(std::shared_ptr<PollingService> service) { active_ = std::move(service); }
    std::shared_ptr<PollingService> detach() {
        auto service = std::move(active_);
        stopping_ = service;
        return service;
    }
    void reset() { active_.reset(); }
    void finish_stop(const PollingService* service) {
        if (auto stopping = stopping_.lock(); stopping.get() == service) stopping_.reset();
    }
    std::shared_ptr<PollingService> history_service() const {
        return active_ ? active_ : stopping_.lock();
    }
    static void stop(const std::shared_ptr<PollingService>& service) {
        if (service) service->stop();
    }
private:
    std::shared_ptr<PollingService> active_;
    std::weak_ptr<PollingService> stopping_;
};
}  // namespace edge_controller
