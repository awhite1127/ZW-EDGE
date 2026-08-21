// Linux 串口候选项枚举接口，仅提供发现和诊断信息，不打开设备。
#pragma once

#include <string>
#include <vector>

#include "common/status_code.h"
#include "model/serial_port_info.h"

namespace edge_controller {

class SerialPortEnumerator {
public:
    // 枚举可用于业务通讯的串口并附带诊断信息。
    StatusCode enumerate(std::vector<SerialPortInfo>* ports, std::string* error_message = nullptr) const;
};

}  // namespace edge_controller
