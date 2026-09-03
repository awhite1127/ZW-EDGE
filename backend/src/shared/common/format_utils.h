// 提供十六进制、布尔值等诊断文本的无状态格式化辅助。
// 边界：保持低依赖、无业务状态，供上层单向复用。

#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace edge_controller::format_utils {

// 把二进制负载格式化成十六进制文本，便于打印收发日志。
inline std::string bytes_to_hex(const std::vector<std::uint8_t>& data)
{
    if (data.empty()) {
        return "<empty>";
    }

    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result(data.size() * 3U - 1U, ' ');
    for (std::size_t index = 0; index < data.size(); ++index) {
        const auto output_index = index * 3U;
        result[output_index] = kHexDigits[(data[index] >> 4U) & 0x0FU];
        result[output_index + 1U] = kHexDigits[data[index] & 0x0FU];
    }
    return result;
}

// 把寄存器数组格式化成文本，便于采集诊断输出。
inline std::string registers_to_string(const std::vector<std::uint16_t>& registers)
{
    std::ostringstream stream;
    stream << "[";
    for (std::size_t index = 0; index < registers.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << registers[index];
    }
    stream << "]";
    return stream.str();
}

}  // namespace edge_controller::format_utils
