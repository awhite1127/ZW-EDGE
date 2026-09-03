// 执行受模板约束的保持寄存器写操作，并校验地址、数量和响应。
// 边界：只执行模板约束下的采集或写入，不负责轮询调度。

#pragma once

#include <cstdint>
#include <vector>

#include "data/datastore/communication_store.h"
#include "application/manager/channel_manager.h"
#include "data/model/master_node_config.h"
#include "communication/protocol/i_modbus_client.h"

namespace edge_controller {

class ModbusWriteExecutor {
public:
    // 绑定通道管理器和通讯报文记录器，用于执行写寄存器命令。
    ModbusWriteExecutor(
        ChannelManager* channel_manager,
        CommunicationTraceStore* communication_trace_store = nullptr);

    // 对指定主站执行一次 Modbus 写多个保持寄存器操作。
    ModbusWriteMultipleRegistersResult write_multiple_holding_registers(
        const MasterNodeConfig& master_config,
        std::uint16_t start_register,
        const std::vector<std::uint16_t>& values);

private:
    ChannelManager* channel_manager_{nullptr};
    CommunicationTraceStore* communication_trace_store_{nullptr};
};

}  // namespace edge_controller
