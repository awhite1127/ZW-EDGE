#include "data/datastore/data_store.h"

#include <atomic>
#include <iostream>
#include <thread>
#include <utility>

namespace {

edge_controller::DeviceStatus make_status(edge_controller::TimestampMs sample_time)
{
    edge_controller::DeviceStatus status;
    status.device_id = "device-1";
    status.device_name = "Device 1";
    status.master_id = "master-1";
    status.template_id = "template-1";
    status.template_name = "Template 1";
    status.online = true;
    status.last_collect_success = true;
    status.communication_quality = edge_controller::DataQuality::kGood;
    status.updated_at_ms = sample_time;
    status.last_success_time_ms = sample_time;

    edge_controller::PointValue point;
    point.key = "value";
    point.name = "Value";
    point.value = static_cast<double>(sample_time);
    point.raw_value = point.value;
    point.quality = edge_controller::DataQuality::kGood;
    point.valid = true;
    point.sample_time_ms = sample_time;
    status.points.push_back(std::move(point));
    return status;
}

}  // namespace

int main()
{
    edge_controller::DataStore store;
    store.update_device_status(make_status(1));

    const auto initial = store.get_realtime_page_snapshot();
    if (initial.system_status.device_status_list.size() != 1 ||
        !initial.system_status.device_status_list.front().points.empty() ||
        initial.device_realtime_snapshots.size() != 1 ||
        initial.device_realtime_snapshots.front().points.size() != 1) {
        std::cerr << "initial realtime projection is invalid\n";
        return 1;
    }

    // 普通 SystemStatus 接口仍保留完整点位，实时页面的轻量投影不改变其它调用方。
    const auto full_status = store.get_system_status();
    if (full_status.device_status_list.size() != 1 ||
        full_status.device_status_list.front().points.size() != 1) {
        std::cerr << "full system status lost device points\n";
        return 1;
    }

    std::atomic<bool> writer_done{false};
    std::thread writer([&]() {
        for (edge_controller::TimestampMs sample = 2; sample <= 5000; ++sample) {
            store.update_device_status(make_status(sample));
        }
        writer_done.store(true, std::memory_order_release);
    });

    bool snapshot_invalid = false;
    do {
        const auto snapshot = store.get_realtime_page_snapshot();
        if (snapshot.system_status.device_status_list.size() != 1 ||
            snapshot.device_realtime_snapshots.size() != 1) {
            snapshot_invalid = true;
            break;
        }
        const auto& health = snapshot.system_status.device_status_list.front();
        const auto& realtime = snapshot.device_realtime_snapshots.front();
        if (!health.points.empty() || realtime.points.empty() ||
            health.updated_at_ms != realtime.sample_time_ms ||
            realtime.points.front().sample_time_ms != realtime.sample_time_ms) {
            snapshot_invalid = true;
            break;
        }
    } while (!writer_done.load(std::memory_order_acquire));

    writer.join();
    if (snapshot_invalid) {
        std::cerr << "realtime health and point snapshots came from different updates\n";
        return 1;
    }
    return 0;
}
