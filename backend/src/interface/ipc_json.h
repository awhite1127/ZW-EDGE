// 领域模型与 IPC JSON 之间的显式编解码声明。
#pragma once

#include <vector>

#include <nlohmann/json.hpp>

#include "model/alarm.h"
#include "datastore/history_store.h"
#include "model/channel_config.h"
#include "model/channel_update.h"
#include "model/channel_status.h"
#include "model/communication_trace.h"
#include "model/diagnosis_status.h"
#include "model/device_config.h"
#include "model/data_maintenance.h"
#include "model/device_history_view.h"
#include "model/device_realtime.h"
#include "model/device_status.h"
#include "model/master_node_config.h"
#include "model/master_node_update.h"
#include "model/modbus_write_request.h"
#include "model/master_node_status.h"
#include "model/mqtt_settings.h"
#include "model/network_settings.h"
#include "model/point_value.h"
#include "model/realtime_view_snapshot.h"
#include "model/serial_port_info.h"
#include "model/service_summary.h"
#include "model/system_settings.h"
#include "model/system_status.h"
#include "model/time_settings.h"
#include "model/web_auth.h"

namespace edge_controller::ipc_json {

// 将模型对象转换为JSON。
nlohmann::json to_json(const ConfigSummary& summary);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateFieldDefinition& field);
// 将结构化枚举项转换为 JSON。
nlohmann::json to_json(const DeviceTemplateEnumItemDefinition& item);
// 将读取区块转换为稳定 JSON；block_key 是跨数据库与 IPC 的引用键。
nlohmann::json to_json(const DeviceTemplateReadBlockDefinition& read_block);
// 将实时展示分组转换为稳定 JSON；分组名称变化不影响字段引用。
nlohmann::json to_json(const DeviceTemplateRealtimeGroupDefinition& group);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateWriteCommandOption& option);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateWriteCommandField& field);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateWriteCommandDefinition& command);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateDefinition& device_template);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ReferencedMasterSummary& master);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateManagementItem& item);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateManagementView& view);
// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemSettings& settings);
// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemSettingsUpdateResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const TimeSettings& settings);
// 将模型对象转换为JSON。
nlohmann::json to_json(const TimeApplyResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const TimeSyncResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ManualTimeSetResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const TimeRuntimeStatus& status);
// 将模型对象转换为JSON。
nlohmann::json to_json(const NetworkSettings& settings);
// 将模型对象转换为JSON。
nlohmann::json to_json(const NetworkSettingsUpdateResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const NetworkApplyResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const NetworkRuntimeStatus& status);
// 将模型对象转换为JSON。
nlohmann::json to_json(const MqttSettings& settings);
// 将模型对象转换为JSON。
nlohmann::json to_json(const MqttSettingsUpdateResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const MqttRuntimeStatus& status);
// 将 Modbus 北向配置与运行态转换为 JSON。
nlohmann::json to_json(const ModbusServerSettings& settings);
// 将对应模型序列化为 JSON。
nlohmann::json to_json(const ModbusServerRuntimeStatus& status);
// 将对应模型序列化为 JSON。
nlohmann::json to_json(const ModbusRegisterMapping& mapping);
// 将对应模型序列化为 JSON。
nlohmann::json to_json(const ModbusExportablePoint& point);
// 将对应模型序列化为 JSON。
nlohmann::json to_json(const ModbusServerPageSnapshot& snapshot);
// 将模型对象转换为JSON。
nlohmann::json to_json(const PollingCycleSummary& summary);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DiagnosisStatus& status);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ServiceErrorSummary& summary);
// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemHealthSummary& summary);
// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemProcessMetrics& metrics);
// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemStorageMetrics& metrics);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DatabaseStorageMetrics& metrics);
// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemOverviewSnapshot& snapshot);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ConfigExportBundle& bundle);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ConfigImportResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ServiceEvent& event);
// 将模型对象转换为JSON。
nlohmann::json to_json(const OverviewPageSnapshot& snapshot);
// 将模型对象转换为JSON。
nlohmann::json to_json(const AlarmRule& rule);
// 将模型对象转换为JSON。
nlohmann::json to_json(const AlarmRuntimeState& state);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ActiveAlarmView& alarm);
// 将模型对象转换为JSON。
nlohmann::json to_json(const FactoryResetResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const WebAuthStatus& status);
// 将模型对象转换为JSON。
nlohmann::json to_json(const WebLoginResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const WebPasswordChangeResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const WebUserView& user);
// 将模型对象转换为JSON。
nlohmann::json to_json(const WebUserMutationResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const PointValue& value);
// 将模型对象转换为JSON。
nlohmann::json to_json(const SerialPortInfo& info);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ChannelConfig& config);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ChannelConfigUpdateResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ChannelConfigDeleteResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const MasterNodeConfig& config);
// 将模型对象转换为JSON。
nlohmann::json to_json(const MasterNodeConfigUpdateResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const MasterNodeConfigDeleteResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceConfig& config);
// 将批量设备名称更新结果转换为 JSON。
nlohmann::json to_json(const DeviceDisplayNameBatchResult& result);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ChannelStatus& status);
// 将模型对象转换为JSON。
nlohmann::json to_json(const MasterNodeStatus& status);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceStatus& status);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceRealtimeSnapshot& snapshot);
// 将模型对象转换为JSON。
nlohmann::json to_json(const HistoryRecord& record);
// 将模型对象转换为JSON。
nlohmann::json to_json(const HistoryPointSummary& point);
// 将模型对象转换为JSON。
nlohmann::json to_json(const HistoryOverviewSummary& summary);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceHistoryStats& stats);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceHistoryView& view);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DataMaintenanceSummary& summary);
// 将模型对象转换为JSON。
nlohmann::json to_json(const CommunicationTraceRecord& record);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ChannelCommunicationTraces& traces);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ModbusWriteMultipleRegistersResponse& response);
// 将模型对象转换为JSON。
nlohmann::json to_json(const ModbusReadHoldingRegistersResponse& response);
// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceCommandExecuteResponse& response);
// 将模型对象转换为JSON。
nlohmann::json to_json(const EM100RecordReadResponse& response);
// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemStatus& status);
// 将模型对象转换为JSON。
nlohmann::json to_json(const RealtimeViewSnapshot& snapshot);

// 将模型对象列表转换为JSON数组。
template <typename T>
// 将模型集合序列化为 JSON 数组。
nlohmann::json to_json_array(const std::vector<T>& items)
{
    nlohmann::json values = nlohmann::json::array();
    values.template get_ref<nlohmann::json::array_t&>().reserve(items.size());
    for (const auto& item : items) {
        values.push_back(to_json(item));
    }
    return values;
}

}  // namespace edge_controller::ipc_json
