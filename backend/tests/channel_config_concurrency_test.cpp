#include "application/service/backend_service.h"
#include "application/service/backend_service_internal.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
namespace edge_controller {
struct BackendChannelConfigTestAccess {
    static void run(bool persistence_available) {
        BackendService service;
        ChannelConfig channel;
        channel.channel_id = "busy";
        channel.channel_type = ChannelType::kModbusTcp;
        channel.enabled = false;
        service.system_config_.channels = {channel};
        if (!is_ok(service.channel_manager_.initialize({channel}, nullptr)))
            throw std::runtime_error("channel initialization failed");
        if (persistence_available && !is_ok(service.config_store_.initialize(":memory:")))
            throw std::runtime_error("database initialization failed");
        service.initialized_ = true;
        auto next = channel;
        next.enabled = true;
        std::promise<void> entered;
        std::future<StatusCode> applying;
        std::future<RealtimeViewSnapshot> reading;
        bool responsive = false;
        bool old_snapshot = false;
        {
            // 模拟现场采集持有通信租约；不依赖网络和定时 sleep。
            auto lease = service.channel_manager_.acquire_communication_lease("busy");
            applying = std::async(std::launch::async, [&] {
                std::lock_guard<std::mutex> channel_lock(service.channel_operation_mutex_);
                std::lock_guard<std::mutex> command_lock(service.manual_modbus_mutex_);
                std::unique_lock<std::shared_mutex> lock(service.service_mutex_);
                backend_internal::ScopedConfigApplyFlag guard(service.config_apply_in_progress_);
                entered.set_value();
                std::string error;
                return service.apply_channel_configs_locked(lock, {next}, &error);
            });
            entered.get_future().wait();
            reading = std::async(std::launch::async, [&] { return service.get_realtime_view_snapshot(); });
            responsive = reading.wait_for(500ms) == std::future_status::ready;
            if (responsive) old_snapshot = !reading.get().channels.front().enabled;
            // 无论断言是否成功，都先释放租约，避免旧实现让测试死锁。
        }
        const auto status = applying.get();
        if (reading.valid()) reading.get();
        if (!responsive || !old_snapshot) throw std::runtime_error("query blocked by channel I/O or saw uncommitted config");
        if (is_ok(status) != persistence_available) throw std::runtime_error("unexpected persist outcome");
        if (service.get_channels().front().enabled != persistence_available)
            throw std::runtime_error("commit/rollback changed configuration incorrectly");
        if (service.config_apply_in_progress_.load()) throw std::runtime_error("apply flag not restored");
    }
};
}
int main() {
    try {
        edge_controller::BackendChannelConfigTestAccess::run(true);
        edge_controller::BackendChannelConfigTestAccess::run(false);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
