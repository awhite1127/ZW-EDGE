// Modbus TCP Server 的纯内存运行状态和诊断计数。
#pragma once

#include <cstdint>
#include <string>

#include "shared/common/types.h"

namespace edge_controller {

struct ModbusServerRuntimeStatus {
    bool configured_enabled{false};
    bool running{false};
    bool listening{false};

    std::string state{"stopped"};
    std::string listen_address;
    std::uint16_t listen_port{0};
    std::uint8_t unit_id{0};

    std::uint32_t current_connections{0};
    std::uint64_t total_connections{0};
    std::uint64_t total_requests{0};
    std::uint64_t successful_requests{0};
    std::uint64_t exception_responses{0};
    std::uint64_t unsupported_function_count{0};
    std::uint64_t invalid_address_count{0};
    std::uint64_t invalid_value_count{0};
    std::uint64_t malformed_request_count{0};
    std::uint64_t rejected_connection_count{0};

    TimestampMs started_at_ms{0};
    TimestampMs last_request_time_ms{0};
    std::string last_client_ip;
    std::string last_error_message;
};

}  // namespace edge_controller
