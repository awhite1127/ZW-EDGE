// 北向 Modbus Server 的配置管理、目标查询、事务式 CRUD 和映射热切换入口。
#include "service/backend_service.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "common/time_utils.h"

namespace edge_controller {

namespace {

std::atomic<std::uint64_t> next_mapping_sequence{0};

// 生成 Modbus 寄存器映射标识。
std::string make_mapping_id()
{
    return "modbus-" + std::to_string(time_utils::system_now_ms()) + "-" +
        std::to_string(next_mapping_sequence.fetch_add(1, std::memory_order_relaxed));
}

// 按设备和点位键查找可导出点位。
const ModbusExportablePoint* find_exportable_point(
    const std::vector<ModbusExportablePoint>& points,
    const DeviceId& device_id,
    const std::string& point_key)
{
    const auto found = std::find_if(points.begin(), points.end(), [&](const auto& point) {
        return point.device_id == device_id && point.point_key == point_key;
    });
    return found == points.end() ? nullptr : &*found;
}

}  // namespace

// 应用 Modbus 服务端设置并刷新运行实例。
StatusCode BackendService::apply_modbus_server_settings(
    const ModbusServerSettings& settings,
    std::string* error_message)
{
    std::lock_guard<std::mutex> management_lock(modbus_management_mutex_);
    auto normalized = settings;
    normalize_modbus_server_settings(&normalized);
    const auto validation = validate_modbus_server_settings(
        normalized, "modbus_server_settings", error_message);
    if (!is_ok(validation)) return validation;

    std::shared_ptr<ModbusRegisterBank> bank;
    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            if (error_message != nullptr) *error_message = "后端服务尚未初始化";
            return StatusCode::kInvalidState;
        }
        const auto save_status = config_store_.save_modbus_server_settings(normalized, error_message);
        if (!is_ok(save_status)) return save_status;
        modbus_server_settings_ = normalized;
        bank = modbus_register_bank_;
    }

    // restart 内部可能 join I/O 线程，必须位于 BackendService::service_mutex_ 之外。
    const auto runtime_status = modbus_tcp_server_.restart(normalized, std::move(bank), error_message);
    if (!is_ok(runtime_status)) {
        Logger::warn("应用 Modbus TCP Server 设置失败，南向采集保持运行");
    }
    return runtime_status;
}

// 重新加载 Modbus 寄存器映射。
StatusCode BackendService::reload_modbus_register_mappings(std::string* error_message)
{
    std::lock_guard<std::mutex> management_lock(modbus_management_mutex_);
    return reload_modbus_register_mappings_managed(error_message);
}

// 在管理锁内重新加载 Modbus 寄存器映射。
StatusCode BackendService::reload_modbus_register_mappings_managed(std::string* error_message)
{
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            if (error_message != nullptr) *error_message = "后端服务尚未初始化";
            return StatusCode::kInvalidState;
        }
    }

    std::vector<ModbusRegisterMapping> mappings;
    const auto load_status = config_store_.load_modbus_register_mappings(&mappings, error_message);
    if (!is_ok(load_status)) {
        Logger::warn("重新加载 Modbus 寄存器映射失败");
        return load_status;
    }

    auto next_bank = std::make_shared<ModbusRegisterBank>();
    const auto configure_status = modbus_export_service_->reconfigure_with_snapshot(
        mappings,
        next_bank,
        [this]() { return data_store_.get_all_device_statuses(); },
        [this](const std::shared_ptr<ModbusRegisterBank>& bank) {
            modbus_tcp_server_.replace_register_bank(bank);
        },
        error_message);
    if (!is_ok(configure_status)) {
        Logger::warn("重建 Modbus Register Bank 失败，继续使用旧 Bank");
        return configure_status;
    }

    // 快照获取、回填、导出 state 和 Server Bank 切换已在 ExportService 协调锁内完成。
    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        modbus_register_bank_ = std::move(next_bank);
        modbus_register_mappings_ = std::move(mappings);
    }
    return StatusCode::kOk;
}

// 获取Modbus服务运行态状态。
ModbusServerRuntimeStatus BackendService::get_modbus_server_runtime_status() const
{
    return modbus_tcp_server_.get_runtime_status();
}

// 获取Modbus服务快照。
StatusCode BackendService::get_modbus_server_page_snapshot(
    ModbusServerPageSnapshot* snapshot,
    std::string* error_message) const
{
    if (snapshot == nullptr) {
        if (error_message != nullptr) *error_message = "缺少 Modbus 页面快照输出参数";
        return StatusCode::kInvalidArgument;
    }
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            if (error_message != nullptr) *error_message = "后端服务尚未初始化或正在关闭";
            return StatusCode::kInvalidState;
        }
        snapshot->settings = modbus_server_settings_;
        snapshot->mappings = modbus_register_mappings_;
    }
    snapshot->runtime_status = modbus_tcp_server_.get_runtime_status();
    return StatusCode::kOk;
}

// 列出Modbus寄存器映射。
StatusCode BackendService::list_modbus_register_mappings(
    std::vector<ModbusRegisterMapping>* mappings,
    std::string* error_message) const
{
    if (mappings == nullptr) {
        if (error_message != nullptr) *error_message = "缺少 Modbus 映射列表输出参数";
        return StatusCode::kInvalidArgument;
    }
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    if (!initialized_) {
        if (error_message != nullptr) *error_message = "后端服务尚未初始化或正在关闭";
        return StatusCode::kInvalidState;
    }
    *mappings = modbus_register_mappings_;
    return StatusCode::kOk;
}

// 列出Modbus点位。
StatusCode BackendService::list_modbus_exportable_points(
    std::vector<ModbusExportablePoint>* points,
    std::string* error_message) const
{
    if (points == nullptr) {
        if (error_message != nullptr) *error_message = "缺少可发布点位输出参数";
        return StatusCode::kInvalidArgument;
    }
    std::vector<DeviceConfig> devices;
    std::vector<MasterNodeConfig> masters;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            if (error_message != nullptr) *error_message = "后端服务尚未初始化或正在关闭";
            return StatusCode::kInvalidState;
        }
        devices = system_config_.devices;
        masters = system_config_.master_nodes;
    }

    std::unordered_map<std::string, MasterNodeConfig> master_by_id;
    for (const auto& master : masters) master_by_id.emplace(master.master_id, master);
    std::vector<ModbusExportablePoint> result;
    for (const auto& device : devices) {
        const auto master = master_by_id.find(device.master_id);
        if (master == master_by_id.end()) continue;
        const auto definition = find_device_template(master->second.device_template);
        if (definition == nullptr) continue;
        for (const auto& field : definition->fields) {
            ModbusExportablePoint point;
            point.device_id = device.device_id;
            point.device_name = device.device_name.empty() ? device.generated_name : device.device_name;
            point.device_type_id = definition->template_id;
            point.device_type_name = definition->display_name;
            point.point_key = field.field_key;
            point.point_name = field.display_name;
            point.unit = field.unit;
            point.summary = field.summary;
            result.push_back(std::move(point));
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.device_name != right.device_name) return left.device_name < right.device_name;
        if (left.device_id != right.device_id) return left.device_id < right.device_id;
        // 同一设备内保留模板字段的稳定 display_order/定义顺序。
        return false;
    });
    *points = std::move(result);
    return StatusCode::kOk;
}

// 创建Modbus寄存器映射。
StatusCode BackendService::create_modbus_register_mapping(
    const ModbusRegisterMapping& request,
    ModbusRegisterMapping* created,
    std::string* error_message)
{
    if (created == nullptr) {
        if (error_message != nullptr) *error_message = "缺少创建映射结果输出参数";
        return StatusCode::kInvalidArgument;
    }
    std::lock_guard<std::mutex> management_lock(modbus_management_mutex_);
    std::vector<ModbusExportablePoint> points;
    auto status = list_modbus_exportable_points(&points, error_message);
    if (!is_ok(status)) return status;
    const auto* target = find_exportable_point(points, request.device_id, request.point_key);
    if (target == nullptr) {
        if (error_message != nullptr) *error_message = "当前设备或数据项已不存在，请重新选择";
        return StatusCode::kNotFound;
    }
    std::vector<ModbusRegisterMapping> previous;
    status = config_store_.load_modbus_register_mappings(&previous, error_message);
    if (!is_ok(status)) return status;

    auto mapping = request;
    mapping.mapping_id = make_mapping_id();
    mapping.device_name_snapshot = target->device_name;
    mapping.point_name_snapshot = target->point_name;
    mapping.created_at_ms = time_utils::system_now_ms();
    mapping.updated_at_ms = mapping.created_at_ms;
    normalize_modbus_register_mapping(&mapping);
    status = config_store_.create_modbus_register_mapping(mapping, error_message);
    if (!is_ok(status)) return status;
    status = reload_modbus_register_mappings_managed(error_message);
    if (!is_ok(status)) {
        const auto apply_error = error_message == nullptr ? std::string{} : *error_message;
        std::string rollback_error;
        const auto rollback = config_store_.replace_modbus_register_mappings(previous, &rollback_error);
        const auto restore = is_ok(rollback) ? reload_modbus_register_mappings_managed(&rollback_error) : rollback;
        if (error_message != nullptr) {
            *error_message = "新映射的 Register Bank 热重载失败：" + apply_error;
            if (!is_ok(restore)) *error_message += "；恢复旧配置失败，持久化配置与运行配置可能不一致：" + rollback_error;
        }
        return is_ok(restore) ? status : restore;
    }
    *created = std::move(mapping);
    return StatusCode::kOk;
}

// 更新Modbus寄存器映射。
StatusCode BackendService::update_modbus_register_mapping(
    const std::string& mapping_id,
    const ModbusRegisterMapping& request,
    ModbusRegisterMapping* updated,
    std::string* error_message)
{
    if (updated == nullptr) {
        if (error_message != nullptr) *error_message = "缺少更新映射结果输出参数";
        return StatusCode::kInvalidArgument;
    }
    std::lock_guard<std::mutex> management_lock(modbus_management_mutex_);
    std::vector<ModbusRegisterMapping> previous;
    auto status = config_store_.load_modbus_register_mappings(&previous, error_message);
    if (!is_ok(status)) return status;
    const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& item) {
        return item.mapping_id == mapping_id;
    });
    if (old == previous.end()) {
        if (error_message != nullptr) *error_message = "Modbus 映射不存在：" + mapping_id;
        return StatusCode::kNotFound;
    }

    auto mapping = request;
    mapping.mapping_id = mapping_id;
    mapping.created_at_ms = old->created_at_ms;
    mapping.updated_at_ms = std::max(time_utils::system_now_ms(), mapping.created_at_ms);
    if (mapping.device_id != old->device_id || mapping.point_key != old->point_key) {
        std::vector<ModbusExportablePoint> points;
        status = list_modbus_exportable_points(&points, error_message);
        if (!is_ok(status)) return status;
        const auto* target = find_exportable_point(points, mapping.device_id, mapping.point_key);
        if (target == nullptr) {
            if (error_message != nullptr) *error_message = "当前设备或数据项已不存在，请重新选择";
            return StatusCode::kNotFound;
        }
        mapping.device_name_snapshot = target->device_name;
        mapping.point_name_snapshot = target->point_name;
    } else {
        // 编辑悬空映射时保留原快照，不能被空的前端字段清除。
        mapping.device_name_snapshot = old->device_name_snapshot;
        mapping.point_name_snapshot = old->point_name_snapshot;
    }
    normalize_modbus_register_mapping(&mapping);
    status = config_store_.update_modbus_register_mapping(mapping, error_message);
    if (!is_ok(status)) return status;
    status = reload_modbus_register_mappings_managed(error_message);
    if (!is_ok(status)) {
        const auto apply_error = error_message == nullptr ? std::string{} : *error_message;
        std::string rollback_error;
        const auto rollback = config_store_.replace_modbus_register_mappings(previous, &rollback_error);
        const auto restore = is_ok(rollback) ? reload_modbus_register_mappings_managed(&rollback_error) : rollback;
        if (error_message != nullptr) {
            *error_message = "更新后的 Register Bank 热重载失败：" + apply_error;
            if (!is_ok(restore)) *error_message += "；恢复旧配置失败，持久化配置与运行配置可能不一致：" + rollback_error;
        }
        return is_ok(restore) ? status : restore;
    }
    *updated = std::move(mapping);
    return StatusCode::kOk;
}

// 删除Modbus寄存器映射。
StatusCode BackendService::delete_modbus_register_mapping(
    const std::string& mapping_id,
    std::string* error_message)
{
    std::lock_guard<std::mutex> management_lock(modbus_management_mutex_);
    std::vector<ModbusRegisterMapping> previous;
    auto status = config_store_.load_modbus_register_mappings(&previous, error_message);
    if (!is_ok(status)) return status;
    if (std::none_of(previous.begin(), previous.end(), [&](const auto& item) { return item.mapping_id == mapping_id; })) {
        if (error_message != nullptr) *error_message = "Modbus 映射不存在：" + mapping_id;
        return StatusCode::kNotFound;
    }
    status = config_store_.delete_modbus_register_mapping(mapping_id, error_message);
    if (!is_ok(status)) return status;
    status = reload_modbus_register_mappings_managed(error_message);
    if (!is_ok(status)) {
        const auto apply_error = error_message == nullptr ? std::string{} : *error_message;
        std::string rollback_error;
        const auto rollback = config_store_.replace_modbus_register_mappings(previous, &rollback_error);
        const auto restore = is_ok(rollback) ? reload_modbus_register_mappings_managed(&rollback_error) : rollback;
        if (error_message != nullptr) {
            *error_message = "删除后的 Register Bank 热重载失败：" + apply_error;
            if (!is_ok(restore)) *error_message += "；恢复旧配置失败，持久化配置与运行配置可能不一致：" + rollback_error;
        }
        return is_ok(restore) ? status : restore;
    }
    return StatusCode::kOk;
}

}  // namespace edge_controller
