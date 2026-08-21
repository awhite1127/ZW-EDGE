// 受控 Modbus 多寄存器写请求、回读和诊断结果模型。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common/types.h"

namespace edge_controller {

struct ModbusWriteMultipleRegistersRequest {
    std::string master_id;
    std::uint16_t start_register{0};
    std::vector<std::uint16_t> values;
};

struct ModbusWriteMultipleRegistersResponse {
    bool success{false};
    std::string master_id;
    std::string master_name;
    std::string channel_id;
    std::string channel_name;
    std::string protocol;
    std::uint32_t slave_address{0};
    std::uint32_t function_code{16};
    std::uint32_t start_register{0};
    std::uint32_t register_count{0};
    std::string request_hex;
    std::string response_hex;
    std::string status;
    std::string diagnosis_error_code;
    std::string error_message;
    TimestampMs timestamp_ms{0};
};

struct ModbusReadHoldingRegistersResponse {
    bool success{false};
    std::string master_id;
    std::string master_name;
    std::string channel_id;
    std::string channel_name;
    std::string protocol;
    std::uint32_t slave_address{0};
    std::uint32_t function_code{3};
    std::uint32_t start_register{0};
    std::uint32_t register_count{0};
    std::vector<std::uint16_t> values;
    std::string request_hex;
    std::string response_hex;
    std::string status;
    std::string diagnosis_error_code;
    std::string error_message;
    TimestampMs timestamp_ms{0};
};

struct DeviceCommandExecuteRequest {
    std::string device_id;
    std::string command_key;
    std::map<std::string, std::uint16_t> values;
};

struct DeviceCommandExecuteResponse {
    bool success{false};
    std::string device_id;
    std::string device_name;
    std::string template_id;
    std::string command_key;
    std::string command_name;
    std::vector<std::string> warnings;
    std::string success_hint;
    ModbusWriteMultipleRegistersResponse write_result;
};

struct EM100RecordReadResponse {
    bool success{false};
    std::string device_id;
    std::string device_name;
    std::string template_id;
    std::string record_type;
    bool valid_record{false};
    std::uint32_t unread_count{0};
    std::string title;
    std::string content;
    std::string data_text;
    std::string ratio_type;
    std::string ratio_value_text;
    std::string record_time;
    std::vector<std::uint16_t> raw_registers;
    ModbusReadHoldingRegistersResponse read_result;
};

}  // namespace edge_controller
