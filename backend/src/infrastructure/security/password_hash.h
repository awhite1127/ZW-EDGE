// 密码摘要生成与常量时间验证接口；调用方不得缓存明文密码。
#pragma once

#include <cstdint>
#include <string>

namespace edge_controller::security {

// 创建密码哈希。
bool create_password_hash(
    const std::string& password,
    std::uint32_t iterations,
    std::string* hash_hex,
    std::string* salt_hex,
    std::string* error_message = nullptr);

// 验证密码。
bool verify_password(
    const std::string& password,
    const std::string& hash_hex,
    const std::string& salt_hex,
    std::uint32_t iterations);

}  // namespace edge_controller::security
