// 北向 Modbus 的预编码只读寄存器 Bank。
// 配置、写入持独占锁，连续读取持共享锁；调用者不获得内部引用或锁生命周期。
// Bank 拥有寄存器快照；网络层只能从这里读取，不能在请求路径访问 SQLite 或采集服务。
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/status_code.h"
#include "common/types.h"
#include "model/modbus_server.h"
#include "modbus_server/modbus_register_encoder.h"

namespace edge_controller {

class ModbusRegisterBank {
public:
    // 校验配置并重建寄存器映射状态。
    StatusCode configure(
        const std::vector<ModbusRegisterMapping>& mappings,
        std::string* error_message = nullptr);
    // 写入已编码的寄存器值。
    StatusCode write_encoded_value(
        const std::string& mapping_id,
        const EncodedModbusRegisters& encoded,
        ModbusExportQuality quality,
        std::string* error_message = nullptr);
    // 返回映射当前数据寄存器的值副本；热重载可据此保留语义未变化映射的最后有效数据。
    StatusCode read_encoded_value_snapshot(
        const std::string& mapping_id,
        EncodedModbusRegisters* encoded,
        std::string* error_message = nullptr) const;
    // 仅用于新 Bank 发布前迁移最后数据值；不改变当前质量码，也不绕过运行时禁用写保护。
    StatusCode restore_encoded_value_snapshot(
        const std::string& mapping_id,
        const EncodedModbusRegisters& encoded,
        std::string* error_message = nullptr);
    // 更新寄存器映射的点位质量。
    StatusCode update_quality(
        const std::string& mapping_id,
        ModbusExportQuality quality,
        std::string* error_message = nullptr);
    // 读取指定范围的保持寄存器。
    StatusCode read_holding_registers(
        RegisterAddress start_address,
        RegisterCount register_count,
        std::vector<std::uint16_t>* values,
        std::string* error_message = nullptr) const;
    // 清空寄存器库配置与数据。
    void clear();

private:
    struct MappingSlot {
        RegisterAddress start_address{0};
        std::uint16_t register_count{0};
        RegisterAddress quality_address{0};
        bool enabled{false};
    };

    mutable std::shared_mutex mutex_;
    std::array<std::uint16_t, 65536> registers_{};
    std::bitset<65536> readable_{};
    std::unordered_map<std::string, MappingSlot> mappings_;
};

}  // namespace edge_controller
