// 历史点位摘要、记录和设备历史页面查询结果模型。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "data/datastore/history_store.h"
#include "data/model/channel_config.h"
#include "data/model/device_config.h"
#include "data/model/master_node_config.h"

namespace edge_controller {

struct DeviceHistoryStats {
    std::size_t record_count{0};
    std::string earliest_date;
    std::string latest_date;
};

struct DeviceHistoryQuery {
    DeviceId device_id;
    std::uint32_t days{30};
    std::string start_date;
    std::string end_date;
    std::string point_key;
    std::string sample_period{"day"};
};

struct DeviceHistoryView {
    DeviceConfig device;
    MasterNodeConfig master;
    ChannelConfig channel;
    std::vector<HistoryRecord> history_records;
    std::vector<HistoryPointSummary> history_points;
    DeviceHistoryStats stats;
};

}  // namespace edge_controller
