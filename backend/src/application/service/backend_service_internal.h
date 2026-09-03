// 提供 BackendService 多实现文件共享的校验、查找和诊断辅助。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "shared/common/status_code.h"
#include "data/model/channel_config.h"
#include "data/model/channel_update.h"
#include "data/model/device_config.h"
#include "data/model/master_node_config.h"
#include "data/model/master_node_update.h"
#include "data/model/network_settings.h"
#include "data/model/serial_port_info.h"
#include "data/model/service_summary.h"
#include "data/model/system_config.h"
#include "data/model/system_settings.h"

namespace edge_controller::backend_internal {

// 追加消息。
inline void append_message(std::string* target, const std::string& message)
{
    if (target == nullptr || message.empty()) {
        return;
    }
    if (!target->empty()) {
        *target += "; ";
    }
    *target += message;
}

// 将诊断来源枚举转换为稳定文本。
inline std::string diagnosis_source(const DiagnosisStatus& diagnosis)
{
    if (diagnosis.level == "channel") {
        return "channel";
    }
    if (diagnosis.level == "master" || diagnosis.level == "device") {
        return "polling";
    }
    return "system";
}

// 构造错误信息摘要来源诊断。
inline ServiceErrorSummary build_error_summary_from_diagnosis(const DiagnosisStatus& diagnosis)
{
    ServiceErrorSummary summary;
    if (!diagnosis_has_issue(diagnosis)) {
        return summary;
    }
    summary.has_error = true;
    summary.source = diagnosis_source(diagnosis);
    summary.target_id = diagnosis.target_id;
    summary.diagnosis = diagnosis;
    summary.message = diagnosis.message;
    summary.timestamp_ms = diagnosis.last_error_time_ms;
    return summary;
}

// 构造当前错误信息摘要。
inline ServiceErrorSummary build_current_error_summary(const SystemStatus& status)
{
    DiagnosisStatus selected;
    // 诊断摘要按通道、主站、设备、系统的顺序选取最能解释当前异常的对象。
    for (const auto& channel_status : status.channel_status_list) {
        consider_diagnosis(channel_status.diagnosis, &selected);
    }
    if (diagnosis_has_issue(selected)) {
        return build_error_summary_from_diagnosis(selected);
    }

    for (const auto& master_status : status.master_status_list) {
        consider_diagnosis(master_status.diagnosis, &selected);
    }
    if (diagnosis_has_issue(selected)) {
        return build_error_summary_from_diagnosis(selected);
    }

    for (const auto& device_status : status.device_status_list) {
        consider_diagnosis(device_status.diagnosis, &selected);
    }
    if (diagnosis_has_issue(selected)) {
        return build_error_summary_from_diagnosis(selected);
    }

    consider_diagnosis(status.diagnosis, &selected);
    return build_error_summary_from_diagnosis(selected);
}

class ScopedConfigApplyFlag {
public:
    // 禁止复制配置应用状态守卫。
    explicit ScopedConfigApplyFlag(std::atomic_bool& flag)
        : flag_(&flag)
    {
        flag_->store(true);
    }

    // 构造 ScopedConfigApplyFlag 实例。
    ScopedConfigApplyFlag(const ScopedConfigApplyFlag&) = delete;
    // 禁止复制赋值配置应用状态守卫。
    ScopedConfigApplyFlag& operator=(const ScopedConfigApplyFlag&) = delete;

    // 销毁 ScopedConfigApplyFlag 实例并释放相关资源。
    ~ScopedConfigApplyFlag()
    {
        if (flag_ != nullptr) {
            flag_->store(false);
        }
    }

private:
    std::atomic_bool* flag_{nullptr};
};

// 校验网络设置请求。
StatusCode validate_network_settings_request(
    const NetworkSettingsUpdateRequest& request,
    NetworkSettings* settings,
    std::string* error_message);
// 规范化并校验网络设置请求。
StatusCode canonicalize_network_settings_request(
    const NetworkSettingsUpdateRequest& request,
    const NetworkSettings* current_settings,
    NetworkSettings* settings,
    std::string* error_message);
// 校验系统设置请求。
StatusCode validate_system_settings_request(
    const SystemSettingsUpdateRequest& request,
    SystemSettings* settings,
    std::string* error_message);

// 解析 IPv4 地址并输出整数形式。
bool parse_ipv4_address(const std::string& value, std::uint32_t* output);
// 返回去除首尾空白的字符串副本。
std::string trim_copy(const std::string& value);
// 生成配置写入失败的可读错误。
std::string config_write_error_message(const std::string& fallback, const std::string& detail);

// 查找通道配置内列表。
const ChannelConfig* find_channel_config_in_list(
    const std::vector<ChannelConfig>& channels,
    const ChannelId& channel_id,
    std::size_t* index = nullptr);
// 查找主站配置内列表。
const MasterNodeConfig* find_master_config_in_list(
    const std::vector<MasterNodeConfig>& masters,
    const MasterNodeId& master_id,
    std::size_t* index = nullptr);
// 查找设备配置内列表。
const DeviceConfig* find_device_config_in_list(
    const std::vector<DeviceConfig>& devices,
    const DeviceId& device_id,
    std::size_t* index = nullptr);

// 校验通道配置更新请求。
StatusCode validate_channel_update_request(
    const std::vector<ChannelConfig>& channels,
    const ChannelConfigUpdateRequest& request,
    const std::vector<SerialPortInfo>& serial_ports,
    std::string* warning_message,
    std::string* error_message);
// 校验通道类型变更不会破坏已存在主站与通道之间的协议约束。
StatusCode validate_channel_type_for_referencing_masters(
    const std::vector<MasterNodeConfig>& masters,
    const ChannelId& channel_id,
    ChannelType channel_type,
    std::string* error_message);
// 在解析和应用导入载荷前校验统一通道容量边界。
StatusCode validate_import_channel_limits(
    const std::vector<ChannelConfig>& channels,
    std::string* error_message);
// 在导入写库前按规范化 ID 拒绝重复通道和主站，避免依赖 SQLite 唯一约束返回底层错误。
StatusCode validate_import_resource_ids(
    const std::vector<ChannelConfig>& channels,
    const std::vector<MasterNodeConfig>& masters,
    std::string* error_message);
// 构造新建通道配置。
ChannelConfig build_new_channel_config(const ChannelConfigUpdateRequest& request);
// 合并现有配置与请求，构造更新后的通道配置。
ChannelConfig build_updated_channel_config(
    const ChannelConfig& current,
    const ChannelConfigUpdateRequest& request);

// 校验主站配置更新请求。
StatusCode validate_master_update_request(
    const SystemConfig& system_config,
    const MasterNodeConfigUpdateRequest& request,
    bool creating,
    std::string* error_message);
// 结合设备类型定义校验主站更新请求。
StatusCode validate_master_update_request_with_templates(
    const SystemConfig& system_config,
    const std::vector<DeviceTemplateDefinition>& templates,
    const MasterNodeConfigUpdateRequest& request,
    bool creating,
    std::string* error_message);
// 合并现有配置与请求，构造更新后的主站配置。
MasterNodeConfig build_updated_master_config(
    const MasterNodeConfig* current,
    const MasterNodeConfigUpdateRequest& request);

}  // namespace edge_controller::backend_internal
