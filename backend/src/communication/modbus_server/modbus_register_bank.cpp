#include "communication/modbus_server/modbus_register_bank.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace edge_controller {
namespace {

// 写入错误信息并返回指定失败状态。
StatusCode fail(StatusCode code, const std::string& message, std::string* error_message)
{
    if (error_message != nullptr) *error_message = message;
    return code;
}

// 判断点位质量值是否合法。
bool valid_quality(ModbusExportQuality quality)
{
    switch (quality) {
    case ModbusExportQuality::kGood:
    case ModbusExportQuality::kNotProduced:
    case ModbusExportQuality::kStale:
    case ModbusExportQuality::kDeviceOffline:
    case ModbusExportQuality::kPointInvalid:
    case ModbusExportQuality::kTargetMissing:
    case ModbusExportQuality::kEncodeFailed:
    case ModbusExportQuality::kDisabled:
        return true;
    }
    return false;
}

}  // namespace

// 应用寄存器映射并保留语义未变化的已有值。
StatusCode ModbusRegisterBank::configure(
    const std::vector<ModbusRegisterMapping>& mappings,
    std::string* error_message)
{
    auto normalized = mappings;
    for (auto& mapping : normalized) normalize_modbus_register_mapping(&mapping);
    const auto validation_status = validate_modbus_register_mappings(normalized, error_message);
    if (!is_ok(validation_status)) return validation_status;

    std::array<std::uint16_t, 65536> next_registers{};
    std::bitset<65536> next_readable;
    std::unordered_map<std::string, MappingSlot> next_mappings;
    next_mappings.reserve(normalized.size());
    for (const auto& mapping : normalized) {
        MappingSlot slot;
        slot.start_address = mapping.start_address;
        slot.register_count = modbus_data_register_count(mapping.data_type);
        slot.quality_address = mapping.quality_address;
        slot.enabled = mapping.enabled;
        for (std::uint32_t offset = 0; offset < slot.register_count; ++offset) {
            next_readable.set(static_cast<std::size_t>(slot.start_address) + offset);
        }
        next_readable.set(slot.quality_address);
        next_registers[slot.quality_address] = static_cast<std::uint16_t>(
            mapping.enabled ? ModbusExportQuality::kNotProduced : ModbusExportQuality::kDisabled);
        next_mappings.emplace(mapping.mapping_id, slot);
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    registers_.swap(next_registers);
    readable_ = std::move(next_readable);
    mappings_.swap(next_mappings);
    return StatusCode::kOk;
}

// 写入编码值值。
StatusCode ModbusRegisterBank::write_encoded_value(
    const std::string& mapping_id,
    const EncodedModbusRegisters& encoded,
    ModbusExportQuality quality,
    std::string* error_message)
{
    if (!valid_quality(quality)) {
        return fail(StatusCode::kInvalidArgument, "Modbus 质量码不受支持", error_message);
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto found = mappings_.find(mapping_id);
    if (found == mappings_.end()) return fail(StatusCode::kNotFound, "Modbus 映射不存在：" + mapping_id, error_message);
    const auto& slot = found->second;
    if (encoded.register_count != slot.register_count) {
        return fail(StatusCode::kInvalidArgument, "Modbus 编码寄存器数量与映射不匹配：" + mapping_id, error_message);
    }
    if (!slot.enabled) {
        registers_[slot.quality_address] = static_cast<std::uint16_t>(ModbusExportQuality::kDisabled);
        return fail(StatusCode::kInvalidState, "禁用的 Modbus 映射不可写入：" + mapping_id, error_message);
    }
    if (quality == ModbusExportQuality::kDisabled) {
        return fail(StatusCode::kInvalidArgument, "启用的 Modbus 映射不能写入 disabled 质量：" + mapping_id, error_message);
    }
    for (std::uint16_t index = 0; index < slot.register_count; ++index) {
        registers_[static_cast<std::size_t>(slot.start_address) + index] = encoded.registers[index];
    }
    registers_[slot.quality_address] = static_cast<std::uint16_t>(quality);
    return StatusCode::kOk;
}

// 读取编码值值快照。
StatusCode ModbusRegisterBank::read_encoded_value_snapshot(
    const std::string& mapping_id,
    EncodedModbusRegisters* encoded,
    std::string* error_message) const
{
    if (encoded == nullptr) {
        return fail(StatusCode::kInvalidArgument, "Modbus 编码寄存器快照输出参数为空", error_message);
    }
    *encoded = EncodedModbusRegisters{};
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = mappings_.find(mapping_id);
    if (found == mappings_.end()) {
        return fail(StatusCode::kNotFound, "Modbus 映射不存在：" + mapping_id, error_message);
    }
    const auto& slot = found->second;
    encoded->register_count = slot.register_count;
    for (std::uint16_t index = 0; index < slot.register_count; ++index) {
        encoded->registers[index] = registers_[static_cast<std::size_t>(slot.start_address) + index];
    }
    return StatusCode::kOk;
}

// 恢复编码值值快照。
StatusCode ModbusRegisterBank::restore_encoded_value_snapshot(
    const std::string& mapping_id,
    const EncodedModbusRegisters& encoded,
    std::string* error_message)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto found = mappings_.find(mapping_id);
    if (found == mappings_.end()) {
        return fail(StatusCode::kNotFound, "Modbus 映射不存在：" + mapping_id, error_message);
    }
    const auto& slot = found->second;
    if (encoded.register_count != slot.register_count) {
        return fail(StatusCode::kInvalidArgument, "Modbus 编码寄存器快照数量与映射不匹配：" + mapping_id, error_message);
    }
    for (std::uint16_t index = 0; index < slot.register_count; ++index) {
        registers_[static_cast<std::size_t>(slot.start_address) + index] = encoded.registers[index];
    }
    return StatusCode::kOk;
}

// 更新质量。
StatusCode ModbusRegisterBank::update_quality(
    const std::string& mapping_id,
    ModbusExportQuality quality,
    std::string* error_message)
{
    if (!valid_quality(quality)) {
        return fail(StatusCode::kInvalidArgument, "Modbus 质量码不受支持", error_message);
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto found = mappings_.find(mapping_id);
    if (found == mappings_.end()) return fail(StatusCode::kNotFound, "Modbus 映射不存在：" + mapping_id, error_message);
    if (found->second.enabled && quality == ModbusExportQuality::kDisabled) {
        return fail(StatusCode::kInvalidArgument, "启用的 Modbus 映射不能写入 disabled 质量：" + mapping_id, error_message);
    }
    registers_[found->second.quality_address] = static_cast<std::uint16_t>(
        found->second.enabled ? quality : ModbusExportQuality::kDisabled);
    return StatusCode::kOk;
}

// 读取寄存器。
StatusCode ModbusRegisterBank::read_holding_registers(
    RegisterAddress start_address,
    RegisterCount register_count,
    std::vector<std::uint16_t>* values,
    std::string* error_message) const
{
    if (values == nullptr) return fail(StatusCode::kInvalidArgument, "Modbus 读取输出参数为空", error_message);
    if (register_count == 0) return fail(StatusCode::kInvalidArgument, "Modbus 读取寄存器数量不能为 0", error_message);
    const auto end = static_cast<std::uint32_t>(start_address) + register_count;
    if (end > 65536U) return fail(StatusCode::kInvalidArgument, "Modbus 读取范围超过地址 65535", error_message);

    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (std::uint32_t address = start_address; address < end; ++address) {
        if (!readable_.test(address)) {
            return fail(StatusCode::kNotFound, "Modbus 寄存器地址未配置：" + std::to_string(address), error_message);
        }
    }
    values->assign(registers_.begin() + start_address, registers_.begin() + end);
    return StatusCode::kOk;
}

// 清空寄存器映射和值缓存。
void ModbusRegisterBank::clear()
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    registers_.fill(0);
    readable_.reset();
    mappings_.clear();
}

}  // namespace edge_controller
