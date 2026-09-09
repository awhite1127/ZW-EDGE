#include "application/service/polling_service.h"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace edge_controller;
using namespace std::chrono_literals;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void test_queue_overload() {
    OrderedTaskQueue queue;
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto started = entered.get_future();
    queue.start();
    queue.submit([&](bool) { entered.set_value(); released.wait(); }, 1);
    if (started.wait_for(2s) != std::future_status::ready) {
        release.set_value();
        throw std::runtime_error("consumer did not start");
    }
    // 阻塞消费者模拟持久化卡顿，填满队列后提交必须立即返回。
    std::atomic<int> completed{0};
    bool filled = true;
    for (int i = 0; i < 128; ++i)
        filled = queue.submit([&](bool gap) { if (!gap) ++completed; }, 1) && filled;
    auto rejected = std::async(std::launch::async, [&] { return queue.submit([](bool) {}, 1); });
    const bool nonblocking = rejected.wait_for(200ms) == std::future_status::ready;
    // 即使旧实现阻塞，也先释放消费者，保证测试能够正常退出。
    release.set_value();
    const bool accepted = rejected.get();
    queue.stop();
    require(filled && nonblocking && !accepted, "full queue blocked or accepted excess work");
    require(completed == 128, "accepted samples lost FIFO continuity");

    queue.start();
    std::promise<bool> gap;
    auto gap_result = gap.get_future();
    require(queue.submit([&](bool missing) { gap.set_value(missing); }, 1), "queue did not recover");
    queue.stop();
    require(gap_result.get(), "overflow gap was not delivered after recovery");
    queue.start();
    require(!queue.submit([](bool) {}, 16385), "oversized batch bypassed capacity");
    queue.stop();
}
}

namespace edge_controller {
struct PollingServiceTestAccess {
    static void test_generation() {
        SystemConfig config;
        ChannelConfig channel;
        channel.channel_id = "tcp";
        channel.channel_type = ChannelType::kModbusTcp;
        config.channels.push_back(channel);
        ChannelManager manager;
        require(is_ok(manager.initialize(config.channels, nullptr)), "channel initialization failed");
        DataStore store;
        PollingService polling(config, manager, store, nullptr, nullptr, nullptr, 1000);
        DeviceConfig device;
        device.device_id = "device";
        device.master_id = "master";
        PollingService::MasterPollingTarget target;
        target.master.master_id = "master";
        target.master.channel_id = "tcp";
        target.devices.push_back(&device);
        target.channel_generation = manager.generation("tcp");
        DeviceStatus current;
        current.device_id = "device";
        current.master_id = "master";
        current.last_error_message = "new configuration";
        store.update_device_status(current);
        MasterNodeStatus master;
        master.master_id = "master";
        master.last_error_message = "new configuration";
        store.update_master_status(master);
        channel.enabled = false;
        require(is_ok(manager.apply_channels({channel}, [] { return StatusCode::kOk; }, nullptr)), "reconfigure failed");
        int callbacks = 0;
        polling.set_device_status_update_callback([&](const auto&) { ++callbacks; });
        const auto stale = polling.mark_master_collection_failed_without_io(
            target, "old failure", DiagnosisErrorCode::kChannelOpenFailed);
        require(stale.discarded, "stale exception result was not discarded");
        polling.mark_devices_collect_failed(target, 100, "old failure", DiagnosisErrorCode::kChannelOpenFailed);
        require(!polling.publish_device_statuses({current}, "tcp", target.channel_generation), "stale success accepted");
        require(callbacks == 0, "stale sample reached northbound callback");
        require(store.get_device_status("device")->last_error_message == "new configuration", "stale failure overwrote device");
        require(store.get_master_status("master")->last_error_message == "new configuration", "stale failure overwrote master");
        target.channel_generation = manager.generation("tcp");
        polling.mark_devices_collect_failed(target, 101, "current failure", DiagnosisErrorCode::kChannelOpenFailed);
        require(callbacks == 1, "current generation failure was incorrectly discarded");
    }

    static void test_short_period() {
        SystemConfig config;
        ChannelConfig channel;
        channel.channel_id = "tcp";
        channel.channel_type = ChannelType::kModbusTcp;
        channel.enabled = false; // 不使用网络，只执行真实调度及停用通道路径。
        config.channels.push_back(channel);
        ChannelManager manager;
        require(is_ok(manager.initialize(config.channels, nullptr)), "channel initialization failed");
        DataStore store;
        PollingService polling(config, manager, store, nullptr, nullptr, nullptr, 1000);
        PollingService::MasterPollingTarget target;
        target.master.master_id = "master";
        target.master.channel_id = "tcp";
        target.master.enabled = true;
        target.master.protocol = MasterProtocol::kModbusTcp;
        target.master.poll_interval_ms = 100;
        std::thread worker([&] { polling.channel_worker_loop("tcp", {target}); });
        TimestampMs previous = 0;
        int cycles = 0;
        const auto deadline = std::chrono::steady_clock::now() + 750ms;
        while (std::chrono::steady_clock::now() < deadline && cycles < 4) {
            const auto summary = polling.get_last_cycle_summary();
            if (summary.last_cycle_started_at_ms != 0 && summary.last_cycle_started_at_ms != previous) {
                previous = summary.last_cycle_started_at_ms;
                ++cycles;
            }
            std::this_thread::sleep_for(2ms);
        }
        polling.stop_requested_.store(true);
        polling.wait_cv_.notify_all();
        worker.join();
        require(cycles >= 4, "100ms master remained limited by the 1000ms global period");
    }
};
}

int main() {
    try {
        test_queue_overload();
        PollingServiceTestAccess::test_generation();
        PollingServiceTestAccess::test_short_period();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
