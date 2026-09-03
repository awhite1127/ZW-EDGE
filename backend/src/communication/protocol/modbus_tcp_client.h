// 通过 TCP 通道执行带事务号的 Modbus 请求响应。
// 边界：严格校验帧边界与响应一致性，不猜测修复异常报文。

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "communication/channel/i_channel.h"
#include "communication/protocol/i_modbus_client.h"

namespace edge_controller {

class ModbusTcpClient : public IModbusClient {
public:
    // 绑定一个 TCP 通道作为 Modbus TCP 收发载体。
    explicit ModbusTcpClient(
        IChannel* channel,
        const std::atomic<bool>* cancel_requested = nullptr);

    // 按指定起始地址、数量和 FC03/FC04 读取连续寄存器块。
    StatusCode read_registers(
        const MasterNodeConfig& master_config,
        std::uint8_t function_code,
        std::uint16_t start_register,
        std::uint16_t register_count,
        ModbusReadResult* result) override;

    // 按指定起始地址和数量读取保持寄存器。
    StatusCode read_holding_registers(
        const MasterNodeConfig& master_config,
        std::uint16_t start_register,
        std::uint16_t register_count,
        ModbusReadResult* result);

    // 向指定起始地址写入多个保持寄存器。
    StatusCode write_multiple_holding_registers(
        const MasterNodeConfig& master_config,
        std::uint16_t start_register,
        const std::vector<std::uint16_t>& values,
        ModbusWriteMultipleRegistersResult* result);

private:
    // 生成下一次 Modbus TCP 事务 ID。
    static std::uint16_t next_transaction_id();

    IChannel* channel_{nullptr};
    const std::atomic<bool>* cancel_requested_{nullptr};
};

}  // namespace edge_controller
