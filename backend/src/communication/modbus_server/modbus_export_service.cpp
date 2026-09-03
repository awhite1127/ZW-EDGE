#include "communication/modbus_server/modbus_export_service.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shared/common/logger.h"
#include "communication/modbus_server/modbus_register_bank.h"
#include "communication/modbus_server/modbus_register_encoder.h"

namespace edge_controller {

namespace {

// 判断点位质量是否允许沿用上次有效值。
bool can_carry_last_value(
    const ModbusRegisterMapping& previous,
    const ModbusRegisterMapping& next)
{
    return previous.device_id == next.device_id &&
        previous.point_key == next.point_key &&
        previous.data_type == next.data_type &&
        previous.value_multiplier == next.value_multiplier &&
        previous.value_offset == next.value_offset &&
        previous.byte_order == next.byte_order &&
        previous.word_order == next.word_order;
}

}  // namespace

struct ModbusExportService::MappingState {
    std::shared_ptr<ModbusRegisterBank> bank;
    std::unordered_map<std::string, ModbusRegisterMapping> mapping_by_id;
    std::unordered_map<DeviceId, std::vector<ModbusRegisterMapping>> mappings_by_device;
};

// 应用 Modbus 服务端设置和寄存器映射。
StatusCode ModbusExportService::configure(
    const std::vector<ModbusRegisterMapping>& mappings,
    std::shared_ptr<ModbusRegisterBank> register_bank,
    std::string* error_message)
{
    if (register_bank == nullptr) {
        if (error_message != nullptr) *error_message = "Modbus Register Bank 不能为空";
        return StatusCode::kInvalidArgument;
    }

    auto normalized = mappings;
    for (auto& mapping : normalized) normalize_modbus_register_mapping(&mapping);
    auto next = std::make_shared<MappingState>();
    next->bank = std::move(register_bank);
    next->mapping_by_id.reserve(normalized.size());
    next->mappings_by_device.reserve(normalized.size());
    for (const auto& mapping : normalized) {
        next->mapping_by_id.emplace(mapping.mapping_id, mapping);
        next->mappings_by_device[mapping.device_id].push_back(mapping);
    }

    // Bank::configure 先在临时容器中完成校验和构建；失败不会替换当前导出快照。
    const auto status = next->bank->configure(normalized, error_message);
    if (!is_ok(status)) return status;
    {
        std::lock_guard<std::mutex> lock(update_mutex_);
        std::atomic_store_explicit(
            &state_,
            std::static_pointer_cast<const MappingState>(next),
            std::memory_order_release);
    }
    return StatusCode::kOk;
}

// 使用新配置重新构建快照。
StatusCode ModbusExportService::reconfigure_with_snapshot(
    const std::vector<ModbusRegisterMapping>& mappings,
    std::shared_ptr<ModbusRegisterBank> register_bank,
    DeviceStatusSnapshotProvider snapshot_provider,
    RegisterBankSwitchCallback bank_switch_callback,
    std::string* error_message)
{
    if (register_bank == nullptr || !snapshot_provider || !bank_switch_callback) {
        if (error_message != nullptr) *error_message = "Modbus 映射热重载缺少 Bank、状态快照或切换回调";
        return StatusCode::kInvalidArgument;
    }

    try {
        // 先标准化和建立不可变索引；此时旧 state/Bank 仍供回调和 FC03 使用。
        auto normalized = mappings;
        for (auto& mapping : normalized) normalize_modbus_register_mapping(&mapping);
        auto next = std::make_shared<MappingState>();
        next->bank = std::move(register_bank);
        next->mapping_by_id.reserve(normalized.size());
        next->mappings_by_device.reserve(normalized.size());
        for (const auto& mapping : normalized) {
            next->mapping_by_id.emplace(mapping.mapping_id, mapping);
            next->mappings_by_device[mapping.device_id].push_back(mapping);
        }

        std::lock_guard<std::mutex> lock(update_mutex_);
        // 锁内取快照：已完成的回调已写入 DataStore；后到的回调在切换完成前等待。
        const auto statuses = snapshot_provider();
        const auto configure_status = next->bank->configure(normalized, error_message);
        if (!is_ok(configure_status)) return configure_status;
        const auto previous_state = std::atomic_load_explicit(&state_, std::memory_order_acquire);
        if (previous_state != nullptr && previous_state->bank != nullptr) {
            for (const auto& item : next->mapping_by_id) {
                const auto previous_mapping = previous_state->mapping_by_id.find(item.first);
                if (previous_mapping == previous_state->mapping_by_id.end() ||
                    !can_carry_last_value(previous_mapping->second, item.second)) {
                    continue;
                }
                EncodedModbusRegisters previous_value;
                const auto read_status = previous_state->bank->read_encoded_value_snapshot(
                    item.first, &previous_value, error_message);
                if (!is_ok(read_status)) return read_status;
                const auto write_status = next->bank->restore_encoded_value_snapshot(
                    item.first, previous_value, error_message);
                if (!is_ok(write_status)) return write_status;
            }
        }
        const auto next_const = std::static_pointer_cast<const MappingState>(next);
        backfill_state(next_const, statuses);

        // 只有完整 Bank 已可读时才发布；Server 切换也在协调锁内完成，随后才恢复回调更新。
        std::atomic_store_explicit(&state_, next_const, std::memory_order_release);
        try {
            bank_switch_callback(next->bank);
        } catch (...) {
            std::atomic_store_explicit(&state_, previous_state, std::memory_order_release);
            throw;
        }
        return StatusCode::kOk;
    } catch (const std::exception& error) {
        if (error_message != nullptr) {
            *error_message = "Modbus 映射热重载失败：" + std::string(error.what());
        }
        return StatusCode::kInternalError;
    } catch (...) {
        if (error_message != nullptr) *error_message = "Modbus 映射热重载发生未知异常";
        return StatusCode::kInternalError;
    }
}

// 更新状态。
void ModbusExportService::update_one_status(
    const std::shared_ptr<const MappingState>& state,
    const DeviceStatus& status)
{
    if (state == nullptr || state->bank == nullptr) return;
    const auto mappings = state->mappings_by_device.find(status.device_id);
    if (mappings == state->mappings_by_device.end()) return;

    // 同一设备可能导出多个映射；先为本次稳定状态快照建一次轻量索引，
    // 避免每个映射都从头扫描全部点位（O(mapping_count * point_count)）。
    std::unordered_map<std::string_view, const PointValue*> points_by_key;
    points_by_key.reserve(status.points.size());
    for (const auto& point : status.points) {
        points_by_key.emplace(point.key, &point);
    }

    for (const auto& mapping : mappings->second) {
        if (!mapping.enabled) continue;

        // 初始拓扑快照并不代表一次采集失败；没有成功或失败时间时仍是“未产出”。
        if (status.last_success_time_ms == 0 && status.last_failure_time_ms == 0) {
            state->bank->update_quality(mapping.mapping_id, ModbusExportQuality::kNotProduced, nullptr);
            continue;
        }
        if (!status.online) {
            state->bank->update_quality(mapping.mapping_id, ModbusExportQuality::kDeviceOffline, nullptr);
            continue;
        }

        const auto point = points_by_key.find(mapping.point_key);
        if (point == points_by_key.end()) {
            state->bank->update_quality(mapping.mapping_id, ModbusExportQuality::kTargetMissing, nullptr);
            continue;
        }
        // 点位级 valid/quality 是北向可用性的权威依据。unknown/partial 不得被误编码为 good；
        // 非有限值在此归类为点位不可用，且只更新本映射质量，不影响同设备其他映射。
        const auto& point_value = *point->second;
        if (!point_value.valid ||
            (point_value.quality != DataQuality::kGood &&
             point_value.quality != DataQuality::kStale) ||
            !std::isfinite(point_value.value)) {
            state->bank->update_quality(mapping.mapping_id, ModbusExportQuality::kPointInvalid, nullptr);
            continue;
        }
        if (point_value.quality == DataQuality::kStale ||
            status.communication_quality == DataQuality::kStale) {
            state->bank->update_quality(mapping.mapping_id, ModbusExportQuality::kStale, nullptr);
            continue;
        }

        EncodedModbusRegisters encoded;
        std::string encode_error;
        const auto encode_status = encode_modbus_register_value(
            point_value.value, mapping, &encoded, &encode_error);
        if (!is_ok(encode_status)) {
            state->bank->update_quality(mapping.mapping_id, ModbusExportQuality::kEncodeFailed, nullptr);
            if (Logger::debug_enabled()) {
                Logger::debug("Modbus 映射编码失败：" + mapping.mapping_id + "，" + encode_error);
            }
            continue;
        }
        state->bank->write_encoded_value(
            mapping.mapping_id, encoded, ModbusExportQuality::kGood, nullptr);
    }
}

// 将已发布的点位值回填到重建后的寄存器数据区。
void ModbusExportService::backfill_state(
    const std::shared_ptr<const MappingState>& state,
    const std::vector<DeviceStatus>& statuses)
{
    if (state == nullptr || state->bank == nullptr) return;
    for (const auto& item : state->mapping_by_id) {
        if (item.second.enabled) {
            state->bank->update_quality(item.first, ModbusExportQuality::kTargetMissing, nullptr);
        }
    }
    for (const auto& status : statuses) update_one_status(state, status);
}

// 更新设备状态。
void ModbusExportService::update_device_statuses(const std::vector<DeviceStatus>& statuses)
{
    std::lock_guard<std::mutex> lock(update_mutex_);
    const auto state = std::atomic_load_explicit(&state_, std::memory_order_acquire);
    if (state == nullptr) return;
    try {
        for (const auto& status : statuses) update_one_status(state, status);
    } catch (const std::exception& error) {
        // 导出链路绝不能把异常传播回南向采集 worker。
        Logger::warn("更新 Modbus Register Bank 时发生异常：" + std::string(error.what()));
    } catch (...) {
        Logger::warn("更新 Modbus Register Bank 时发生未知异常");
    }
}

// 批量标记目标缺失状态。
void ModbusExportService::mark_all_targets_missing_or_not_produced()
{
    std::lock_guard<std::mutex> lock(update_mutex_);
    const auto state = std::atomic_load_explicit(&state_, std::memory_order_acquire);
    if (state == nullptr || state->bank == nullptr) return;
    for (const auto& item : state->mapping_by_id) {
        if (item.second.enabled) {
            state->bank->update_quality(item.first, ModbusExportQuality::kTargetMissing, nullptr);
        }
    }
}

// 回填设备状态。
void ModbusExportService::backfill_device_statuses(const std::vector<DeviceStatus>& statuses)
{
    std::lock_guard<std::mutex> lock(update_mutex_);
    const auto state = std::atomic_load_explicit(&state_, std::memory_order_acquire);
    backfill_state(state, statuses);
}

// 返回当前只读寄存器数据区快照。
std::shared_ptr<ModbusRegisterBank> ModbusExportService::register_bank() const
{
    const auto state = std::atomic_load_explicit(&state_, std::memory_order_acquire);
    return state == nullptr ? nullptr : state->bank;
}

}  // namespace edge_controller
