// Web 固定账户、角色和密码变更请求/响应模型。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/types.h"

namespace edge_controller {

constexpr const char* kDefaultWebAdminUsername = "admin";
constexpr const char* kDefaultWebAdminPassword = "admin123";
constexpr const char* kDefaultWebViewerUsername = "user";
constexpr const char* kDefaultWebViewerPassword = "user12345";
constexpr const char* kWebRoleAdmin = "super_admin";
constexpr const char* kWebRoleLegacyAdmin = "admin";
constexpr const char* kWebRoleEngineer = "engineer";
constexpr const char* kWebRoleOperator = "operator";
constexpr const char* kWebRoleViewer = "viewer";
constexpr std::uint32_t kDefaultWebPasswordIterations = 120000;
constexpr std::uint32_t kMaxWebPasswordIterations = 1000000;
constexpr std::size_t kMaxWebPasswordBytes = 256;

struct WebUser {
    std::string username;
    std::string display_name;
    std::string role;
    std::string password_hash;
    std::string password_salt;
    std::uint32_t password_iterations{kDefaultWebPasswordIterations};
    bool enabled{true};
    bool password_change_recommended{true};
    TimestampMs created_at_ms{0};
    TimestampMs updated_at_ms{0};
    TimestampMs last_password_change_ms{0};
    TimestampMs last_login_at_ms{0};
};

struct WebUserView {
    std::string username;
    std::string display_name;
    std::string role;
    bool enabled{true};
    TimestampMs created_at_ms{0};
    TimestampMs updated_at_ms{0};
    TimestampMs last_login_at_ms{0};
    bool password_change_recommended{false};
};

struct WebAuthStatus {
    std::string username{kDefaultWebAdminUsername};
    bool password_change_recommended{true};
    bool auth_initialized{false};
};

struct WebLoginRequest {
    std::string username;
    std::string password;
};

struct WebLoginResult {
    bool success{false};
    std::string username;
    std::string role;
    bool password_change_recommended{true};
    std::string message;
};

struct WebPasswordChangeRequest {
    std::string username;
    std::string current_password;
    std::string new_password;
};

struct WebViewerPasswordSetRequest {
    std::string admin_password;
    std::string new_user_password;
};

struct WebPasswordChangeResult {
    bool success{false};
    std::string message;
    bool password_change_recommended{true};
};

struct WebUserCreateRequest {
    std::string username;
    std::string display_name;
    std::string role;
    std::string password;
    bool enabled{true};
};

struct WebUserUpdateRequest {
    std::string username;
    std::string display_name;
    std::string role;
    bool enabled{true};
};

struct WebUserPasswordResetRequest {
    std::string username;
    std::string new_password;
};

struct WebUserMutationResult {
    bool success{false};
    std::string message;
    WebUserView user;
};

}  // namespace edge_controller
