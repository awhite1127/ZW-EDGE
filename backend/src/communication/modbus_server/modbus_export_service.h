// 南向 DeviceStatus 到北向预编码 Register Bank 的轻量运行时桥接。
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "shared/common/status_code.h"
#include "data/model/device_status.h"
#include "data/model/modbus_server.h"

namespace edge_controller {

class ModbusRegisterBank;

class ModbusExportService {
public:
    using DeviceStatusSnapshotProvider = std::function<std::vector<DeviceStatus>()>;
    using RegisterBankSwitchCallback =
        std::function<void(const std::shared_ptr<ModbusRegisterBank>&)>;

    // 校验配置并重建寄存器映射状态。
    StatusCode configure(
        const std::vector<ModbusRegisterMapping>& mappings,
        std::shared_ptr<ModbusRegisterBank> register_bank,
        std::string* error_message = nullptr);
    // 运行中重载的完整事务：协调锁内取最新快照、构建回填、切换导出状态和 Server Bank。
    StatusCode reconfigure_with_snapshot(
        const std::vector<ModbusRegisterMapping>& mappings,
        std::shared_ptr<ModbusRegisterBank> register_bank,
        DeviceStatusSnapshotProvider snapshot_provider,
        RegisterBankSwitchCallback bank_switch_callback,
        std::string* error_message = nullptr);
    // 批量更新设备状态到寄存器映射。
    void update_device_statuses(const std::vector<DeviceStatus>& statuses);
    // 用设备状态快照回填寄存器映射。
    void backfill_device_statuses(const std::vector<DeviceStatus>& statuses);
    // 将全部映射目标标记为缺失或未产出。
    void mark_all_targets_missing_or_not_produced();
    // 返回当前只读寄存器库。
    std::shared_ptr<ModbusRegisterBank> register_bank() const;

private:
    struct MappingState;
    // 将单个设备状态更新到寄存器库。
    static void update_one_status(const std::shared_ptr<const MappingState>& state, const DeviceStatus& status);
    // 用已有状态回填单个映射目标。
    static void backfill_state(
        const std::shared_ptr<const MappingState>& state,
        const std::vector<DeviceStatus>& statuses);

    // 只协调导出状态更新/替换；不与 BackendService::service_mutex_ 或 Socket I/O 锁嵌套。
    mutable std::mutex update_mutex_;
    std::shared_ptr<const MappingState> state_;
};

}  // namespace edge_controller
