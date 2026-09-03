// 无状态字符串规范化辅助函数。
#pragma once

#include <cstddef>
#include <string>

namespace edge_controller {

// 统计UTF-8字符串中的字符数量。
inline std::size_t utf8_character_count(const std::string& value)
{
    std::size_t count = 0;
    for (const unsigned char ch : value) {
        if ((ch & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

}  // namespace edge_controller
