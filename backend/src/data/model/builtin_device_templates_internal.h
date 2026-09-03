// 内置设备定义文件共享的少量构造辅助。
#pragma once

#include <string>
#include <vector>

#include "data/model/device_template.h"

namespace edge_controller::builtin_detail {

DeviceTemplateFieldDefinition make_builtin_scalar_field(
    const std::string& field_key,
    const std::string& display_name,
    const std::string& parser_id,
    const std::string& unit,
    const std::string& data_type,
    RegisterAddress register_offset,
    RegisterCount register_count,
    double scale,
    double value_offset,
    std::uint32_t precision,
    bool summary,
    bool show_in_realtime,
    bool history_enabled,
    std::uint32_t display_order);

DeviceTemplateDefinition make_builtin_single_read_block_template(
    std::string template_id,
    std::string display_name,
    std::string description,
    RegisterAddress default_start_register,
    std::uint32_t register_count,
    std::vector<DeviceTemplateFieldDefinition> fields,
    std::vector<DeviceTemplateWriteCommandDefinition> write_commands = {});

}  // namespace edge_controller::builtin_detail
