// 执行单主站采集事务，协调协议请求、寄存器映射和采集结果归一化。
// 边界：只执行模板约束下的采集或写入，不负责轮询调度。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "communication/collect/block_read_plan.h"
#include "communication/collect/device_read_result.h"
#include "data/datastore/communication_store.h"
#include "application/manager/channel_manager.h"
#include "data/model/device_config.h"
#include "data/model/device_template.h"
#include "data/model/diagnosis_status.h"
#include "data/model/master_node_config.h"
#include "data/model/master_node_status.h"

namespace edge_controller {

struct MasterCollectResult {
    // success 表示本轮所有设备的所有读取区块均成功；any_success 表示至少一个区块成功。
    bool success{false};
    bool any_success{false};
    DataQuality communication_quality{DataQuality::kUnknown};
    std::size_t total_block_count{0};
    std::size_t successful_block_count{0};
    std::size_t failed_block_count{0};
    std::vector<DeviceReadResult> device_results;
    DiagnosisErrorCode diagnosis_error_code{DiagnosisErrorCode::kNone};
    std::string error_message;
    TimestampMs timestamp_ms{0};
};

// 配置世代内稳定的主站采集计划；模板注册表更新时由轮询 worker 按需替换。
struct MasterCollectorRuntimePlan {
    std::shared_ptr<const DeviceTemplateDefinition> device_template;
    std::vector<BlockReadPlan> read_plans;
    std::string preparation_error;
};

// 负责对单个总控执行一次完整的请求和响应采集。

class MasterCollector {
public:
    // 绑定通道管理器和通讯报文记录器，用于执行主站采集。
    explicit MasterCollector(
        ChannelManager* channel_manager,
        CommunicationTraceStore* communication_trace_store = nullptr);

    // 解析模板并预生成读取计划；正常轮询只在配置世代变化时调用。
    static MasterCollectorRuntimePlan build_runtime_plan(
        const MasterNodeConfig& master_config,
        const std::vector<const DeviceConfig*>& devices);

    // 对单个主站按设备和读取区块执行一轮顺序 Modbus 读取。
    MasterCollectResult collect_once(
        const MasterNodeConfig& master_config,
        const std::vector<const DeviceConfig*>& devices,
        MasterNodeStatus* master_status = nullptr,
        const std::atomic<bool>* cancel_requested = nullptr);

    // 使用预生成的模板快照、读取计划和 RTU 请求帧执行采集。
    MasterCollectResult collect_once(
        const MasterNodeConfig& master_config,
        const std::vector<const DeviceConfig*>& devices,
        const MasterCollectorRuntimePlan& runtime_plan,
        MasterNodeStatus* master_status = nullptr,
        const std::atomic<bool>* cancel_requested = nullptr);

private:
    ChannelManager* channel_manager_{nullptr};
    CommunicationTraceStore* communication_trace_store_{nullptr};
};

}  // namespace edge_controller
