// 内置设备定义统一入口及稳定单设备访问接口。
#pragma once

#include <string>
#include <vector>

#include "data/model/device_template.h"

namespace edge_controller {

const std::string& default_device_template_id();
const DeviceTemplateDefinition& default_device_template();
const DeviceTemplateDefinition& communication_manager_rd100_device_template();
const DeviceTemplateDefinition& r2_temperature_point_device_template();
const DeviceTemplateDefinition& r2a_0000h_device_template();
const DeviceTemplateDefinition& r2a_1000h_device_template();
const DeviceTemplateDefinition& r2a_2000h_device_template();
const DeviceTemplateDefinition& r4_rd_1000h_device_template();
const DeviceTemplateDefinition& r4_wt_1000h_device_template();
const DeviceTemplateDefinition& r4_rd_2000h_device_template();
const DeviceTemplateDefinition& r4_wt_2000h_device_template();
const DeviceTemplateDefinition& communication_manager_sf6_device_template();
const DeviceTemplateDefinition& communication_manager_wireless_temperature_device_template();
const DeviceTemplateDefinition& em100_insulation_monitor_device_template();
const DeviceTemplateDefinition& em100h_insulation_monitor_device_template();

// 按稳定顺序汇总全部内置设备定义。
std::vector<DeviceTemplateDefinition> builtin_device_templates();

}  // namespace edge_controller
