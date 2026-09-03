// 串口发现结果及物理路径、驱动、占用状态诊断模型。
#pragma once

#include <string>

namespace edge_controller {

struct SerialPortInfo {
    std::string path;
    std::string name;
    std::string kind{"unknown"};
    std::string display_name;
    bool available{true};
    bool busy{false};
    std::string description;
    std::string symlink_by_id;
    std::string symlink_by_path;
    std::string driver;
    std::string physical_hint;
};

}  // namespace edge_controller
