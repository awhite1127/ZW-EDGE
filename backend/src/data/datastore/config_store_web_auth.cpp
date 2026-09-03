// Web 账户仓库：维护固定角色、随机 salt 和密码摘要，绝不存储或记录明文密码。
#include "data/datastore/config_store.h"
#include "data/datastore/config_store_internal.h"
#include <mutex>
#include <string>
#include <vector>
#include "shared/common/logger.h"
#include "shared/common/sqlite_compat.h"
#include "shared/common/time_utils.h"
#include "infrastructure/security/password_hash.h"

namespace edge_controller {

using namespace config_store_internal;

namespace {

// 判断 Web 用户角色是否合法。
bool valid_web_role(const std::string& role)
{
    return role == kWebRoleAdmin || role == kWebRoleEngineer ||
           role == kWebRoleOperator || role == kWebRoleViewer;
}

// 规范化 Web 用户角色。
std::string normalize_web_role(const std::string& role)
{
    return role == kWebRoleLegacyAdmin ? kWebRoleAdmin : role;
}

// 校验 Web 用户名是否合法。
bool valid_username(const std::string& username)
{
    if (username.empty() || username.size() > 64) return false;
    for (const auto ch : username) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
        if (!ok) return false;
    }
    return true;
}

// 生成 Web 用户默认显示名称。
std::string default_display_name_for_user(const std::string& username, const std::string& role)
{
    if (username == kDefaultWebAdminUsername) return "超级管理员";
    if (username == kDefaultWebViewerUsername) return "值守用户";
    if (role == kWebRoleEngineer) return "工程师";
    if (role == kWebRoleOperator) return "操作员";
    if (role == kWebRoleViewer) return "只读用户";
    return username;
}

// 统计已启用的超级管理员数量。
int enabled_super_admin_count(sqlite3* database)
{
    Statement statement(
        database,
        "SELECT COUNT(*) FROM web_users WHERE enabled = 1 AND role IN ('super_admin','admin');");
    if (!statement.ok() || sqlite3_step(statement.get()) != SQLITE_ROW) {
        return 0;
    }
    return sqlite3_column_int(statement.get(), 0);
}

// 将内部用户记录转换为安全展示模型。
WebUserView web_user_view(const WebUser& user)
{
    WebUserView view;
    view.username = user.username;
    view.display_name = user.display_name.empty() ? default_display_name_for_user(user.username, user.role) : user.display_name;
    view.role = user.role;
    view.enabled = user.enabled;
    view.created_at_ms = user.created_at_ms;
    view.updated_at_ms = user.updated_at_ms;
    view.last_login_at_ms = user.last_login_at_ms;
    view.password_change_recommended = user.password_change_recommended;
    return view;
}

}  // namespace

// 读取并初始化Web管理员。
StatusCode ConfigStore::load_or_initialize_web_admin(
    WebUser* user,
    std::string* error_message)
{
    if (user == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 Web 管理员账号输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto load_status = load_web_user_locked(kDefaultWebAdminUsername, user, error_message);
    if (is_ok(load_status)) {
        return StatusCode::kOk;
    }
    if (load_status != StatusCode::kNotFound) {
        return load_status;
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return initialize_default_web_user_locked(
        kDefaultWebAdminUsername,
        kWebRoleAdmin,
        kDefaultWebAdminPassword,
        user,
        error_message);
}

// 读取并初始化Web用户。
StatusCode ConfigStore::load_or_initialize_web_users(std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const struct DefaultAccount {
        const char* username;
        const char* role;
        const char* password;
    } accounts[] = {
        {kDefaultWebAdminUsername, kWebRoleAdmin, kDefaultWebAdminPassword},
        {kDefaultWebViewerUsername, kWebRoleOperator, kDefaultWebViewerPassword},
    };
    for (const auto& account : accounts) {
        WebUser existing;
        const auto load_status = load_web_user_locked(account.username, &existing, error_message);
        if (is_ok(load_status)) {
            if (existing.username == kDefaultWebViewerUsername &&
                (existing.role == kWebRoleViewer || existing.display_name.empty() || existing.display_name == "只读用户")) {
                existing.role = kWebRoleOperator;
                existing.display_name = default_display_name_for_user(existing.username, existing.role);
                existing.updated_at_ms = time_utils::system_now_ms();
                const auto save_status = save_web_user_locked(existing, error_message);
                if (!is_ok(save_status)) {
                    return save_status;
                }
            }
            continue;
        }
        if (load_status != StatusCode::kNotFound) {
            return load_status;
        }
        if (error_message != nullptr) {
            error_message->clear();
        }
        const auto initialize_status = initialize_default_web_user_locked(
            account.username,
            account.role,
            account.password,
            nullptr,
            error_message);
        if (!is_ok(initialize_status)) {
            return initialize_status;
        }
    }
    return StatusCode::kOk;
}

// 获取Web认证状态。
StatusCode ConfigStore::get_web_auth_status(
    WebAuthStatus* status,
    std::string* error_message)
{
    if (status == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 Web 登录状态输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    WebUser user;
    const auto load_status = load_or_initialize_web_admin(&user, error_message);
    if (!is_ok(load_status)) {
        return load_status;
    }
    status->username = user.username;
    status->password_change_recommended = user.password_change_recommended;
    status->auth_initialized = true;
    return StatusCode::kOk;
}

// 验证Web登录。
StatusCode ConfigStore::verify_web_login(
    const WebLoginRequest& request,
    WebLoginResult* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 Web 登录结果输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    *result = {};
    if (request.password.size() > kMaxWebPasswordBytes) {
        result->message = "账号或密码错误";
        return StatusCode::kOk;
    }
    if (!valid_username(request.username)) {
        result->message = "账号或密码错误";
        return StatusCode::kOk;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    WebUser user;
    const auto load_status = load_web_user_locked(request.username, &user, error_message);
    if (!is_ok(load_status)) {
        if (load_status == StatusCode::kNotFound) {
            result->message = "账号或密码错误";
            return StatusCode::kOk;
        }
        return load_status;
    }
    if (!user.enabled) {
        result->message = "账号已禁用";
        return StatusCode::kOk;
    }

    const bool password_ok = security::verify_password(
        request.password,
        user.password_hash,
        user.password_salt,
        user.password_iterations);
    if (!password_ok) {
        result->success = false;
        result->message = "账号或密码错误";
        return StatusCode::kOk;
    }

    result->success = true;
    result->username = user.username;
    result->role = user.role;
    result->password_change_recommended = user.password_change_recommended;
    result->message = "登录成功";
    user.last_login_at_ms = time_utils::system_now_ms();
    user.updated_at_ms = user.last_login_at_ms;
    std::string save_error;
    const auto save_status = save_web_user_locked(user, &save_error);
    if (!is_ok(save_status)) {
        Logger::warn(
            "登录成功但最近登录时间保存失败：username=" + user.username +
            "，error=" + (save_error.empty() ? std::string(to_string(save_status)) : save_error));
    }
    return StatusCode::kOk;
}

// 修改Web密码。
StatusCode ConfigStore::change_web_password(
    const WebPasswordChangeRequest& request,
    WebPasswordChangeResult* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 Web 密码修改结果输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    *result = {};

    if (!valid_username(request.username)) {
        result->message = "账户无效";
        return StatusCode::kInvalidArgument;
    }
    const auto& current_password = request.current_password;
    const auto& new_password = request.new_password;
    if (trim_ascii_copy(current_password).empty()) {
        result->message = "当前密码不能为空";
        return StatusCode::kInvalidArgument;
    }
    if (current_password.size() > kMaxWebPasswordBytes || new_password.size() > kMaxWebPasswordBytes) {
        result->message = "密码长度不能超过 256 字节";
        return StatusCode::kInvalidArgument;
    }
    if (new_password.size() < 8U) {
        result->message = "新密码长度不能少于 8 位";
        return StatusCode::kInvalidArgument;
    }
    if (trim_ascii_copy(new_password).empty()) {
        result->message = "新密码不能全为空白字符";
        return StatusCode::kInvalidArgument;
    }
    if (new_password == current_password) {
        result->message = "新密码不能与当前密码相同";
        return StatusCode::kInvalidArgument;
    }
    if ((request.username == kDefaultWebAdminUsername && new_password == kDefaultWebAdminPassword) ||
        (request.username == kDefaultWebViewerUsername && new_password == kDefaultWebViewerPassword)) {
        result->message = "新密码不能使用出厂默认密码";
        return StatusCode::kInvalidArgument;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    WebUser user;
    const auto load_status = load_web_user_locked(request.username, &user, error_message);
    if (!is_ok(load_status)) {
        return load_status;
    }
    if (!user.enabled) {
        result->message = "账户已禁用";
        return StatusCode::kOk;
    }

    if (!security::verify_password(
            current_password,
            user.password_hash,
            user.password_salt,
            user.password_iterations)) {
        result->success = false;
        result->message = "当前密码不正确";
        result->password_change_recommended = user.password_change_recommended;
        return StatusCode::kOk;
    }

    std::string hash_hex;
    std::string salt_hex;
    if (!security::create_password_hash(
            new_password,
            kDefaultWebPasswordIterations,
            &hash_hex,
            &salt_hex,
            error_message)) {
        return StatusCode::kInternalError;
    }
    const auto now_ms = time_utils::system_now_ms();
    user.password_hash = std::move(hash_hex);
    user.password_salt = std::move(salt_hex);
    user.password_iterations = kDefaultWebPasswordIterations;
    user.password_change_recommended = false;
    user.updated_at_ms = now_ms;
    user.last_password_change_ms = now_ms;

    const auto save_status = save_web_user_locked(user, error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }
    result->success = true;
    result->message = "当前账户密码已更新";
    result->password_change_recommended = false;
    return StatusCode::kOk;
}

// 设置Web查看员密码。
StatusCode ConfigStore::set_web_viewer_password(
    const WebViewerPasswordSetRequest& request,
    WebPasswordChangeResult* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少只读用户密码设置结果输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    *result = {};

    const auto& admin_password = request.admin_password;
    const auto& new_password = request.new_user_password;
    if (trim_ascii_copy(admin_password).empty()) {
        result->message = "管理员当前密码不能为空";
        return StatusCode::kInvalidArgument;
    }
    if (admin_password.size() > kMaxWebPasswordBytes || new_password.size() > kMaxWebPasswordBytes) {
        result->message = "密码长度不能超过 256 字节";
        return StatusCode::kInvalidArgument;
    }
    if (new_password.size() < 8U) {
        result->message = "user 新密码长度不能少于 8 位";
        return StatusCode::kInvalidArgument;
    }
    if (trim_ascii_copy(new_password).empty()) {
        result->message = "user 新密码不能全为空白字符";
        return StatusCode::kInvalidArgument;
    }
    if (new_password == kDefaultWebViewerPassword) {
        result->message = "user 新密码不能使用出厂默认密码";
        return StatusCode::kInvalidArgument;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    WebUser admin;
    auto load_status = load_web_user_locked(kDefaultWebAdminUsername, &admin, error_message);
    if (!is_ok(load_status)) {
        return load_status;
    }
    if (!security::verify_password(
            admin_password,
            admin.password_hash,
            admin.password_salt,
            admin.password_iterations)) {
        result->message = "管理员当前密码不正确";
        return StatusCode::kOk;
    }

    WebUser viewer;
    load_status = load_web_user_locked(kDefaultWebViewerUsername, &viewer, error_message);
    if (!is_ok(load_status)) {
        return load_status;
    }
    if (security::verify_password(
            new_password,
            viewer.password_hash,
            viewer.password_salt,
            viewer.password_iterations)) {
        result->message = "user 新密码不能与当前密码相同";
        return StatusCode::kOk;
    }

    std::string hash_hex;
    std::string salt_hex;
    if (!security::create_password_hash(
            new_password,
            kDefaultWebPasswordIterations,
            &hash_hex,
            &salt_hex,
            error_message)) {
        return StatusCode::kInternalError;
    }
    const auto now_ms = time_utils::system_now_ms();
    viewer.password_hash = std::move(hash_hex);
    viewer.password_salt = std::move(salt_hex);
    viewer.password_iterations = kDefaultWebPasswordIterations;
    viewer.password_change_recommended = false;
    viewer.updated_at_ms = now_ms;
    viewer.last_password_change_ms = now_ms;

    const auto save_status = save_web_user_locked(viewer, error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }
    result->success = true;
    result->message = "只读用户密码已更新";
    result->password_change_recommended = false;
    return StatusCode::kOk;
}

// 重置全部 Web 用户密码。
StatusCode ConfigStore::reset_web_user_passwords(std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string transaction_error;
    auto status = execute_sql_locked("BEGIN IMMEDIATE;", &transaction_error);
    if (!is_ok(status)) {
        if (error_message != nullptr) {
            *error_message = transaction_error;
        }
        return status;
    }
    status = initialize_default_web_user_locked(
        kDefaultWebAdminUsername,
        kWebRoleAdmin,
        kDefaultWebAdminPassword,
        nullptr,
        error_message);
    if (is_ok(status)) {
        status = initialize_default_web_user_locked(
            kDefaultWebViewerUsername,
            kWebRoleOperator,
            kDefaultWebViewerPassword,
            nullptr,
            error_message);
    }
    if (is_ok(status)) {
        status = execute_sql_locked("DELETE FROM web_users WHERE username NOT IN ('admin','user');", error_message);
    }
    if (is_ok(status)) {
        status = execute_sql_locked("COMMIT;", error_message);
    }
    if (!is_ok(status)) {
        std::string ignored_error;
        execute_sql_locked("ROLLBACK;", &ignored_error);
    }
    return status;
}

// 列出Web用户。
StatusCode ConfigStore::list_web_users(std::vector<WebUserView>* users, std::string* error_message) const
{
    if (users == nullptr) {
        if (error_message != nullptr) *error_message = "缺少 Web 用户列表输出参数";
        return StatusCode::kInvalidArgument;
    }
    users->clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;

    const char* sql =
        "SELECT username, display_name, role, password_hash, password_salt, password_iterations, "
        "enabled, password_change_recommended, created_at_ms, updated_at_ms, last_password_change_ms, last_login_at_ms "
        "FROM web_users ORDER BY CASE role WHEN 'super_admin' THEN 0 WHEN 'engineer' THEN 1 WHEN 'operator' THEN 2 ELSE 3 END, username;";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) *error_message = "准备读取 web_users 列表失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
        WebUser user;
        user.username = column_text(statement.get(), 0);
        user.display_name = column_text(statement.get(), 1);
        user.role = normalize_web_role(column_text(statement.get(), 2));
        user.password_hash = column_text(statement.get(), 3);
        user.password_salt = column_text(statement.get(), 4);
        user.password_iterations = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 5));
        user.enabled = sqlite3_column_int(statement.get(), 6) != 0;
        user.password_change_recommended = sqlite3_column_int(statement.get(), 7) != 0;
        user.created_at_ms = static_cast<TimestampMs>(column_int64(statement.get(), 8));
        user.updated_at_ms = static_cast<TimestampMs>(column_int64(statement.get(), 9));
        user.last_password_change_ms = static_cast<TimestampMs>(column_int64(statement.get(), 10));
        user.last_login_at_ms = static_cast<TimestampMs>(column_int64(statement.get(), 11));
        if (valid_username(user.username) && valid_web_role(user.role)) {
            users->push_back(web_user_view(user));
        }
    }
    if (rc != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "读取 web_users 列表失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 创建Web用户。
StatusCode ConfigStore::create_web_user(
    const WebUserCreateRequest& request,
    WebUserMutationResult* result,
    std::string* error_message)
{
    if (result == nullptr) return StatusCode::kInvalidArgument;
    *result = {};
    if (!valid_username(request.username)) {
        result->message = "用户名只能包含字母、数字、点、下划线和中划线，长度 1-64";
        return StatusCode::kInvalidArgument;
    }
    if (!valid_web_role(request.role)) {
        result->message = "角色无效";
        return StatusCode::kInvalidArgument;
    }
    if (normalize_web_role(request.role) == kWebRoleAdmin) {
        result->message = "不允许创建新的超级管理员";
        if (error_message != nullptr) {
            *error_message = result->message;
        }
        return StatusCode::kInvalidArgument;
    }
    if (request.password.size() < 8U || request.password.size() > kMaxWebPasswordBytes || trim_ascii_copy(request.password).empty()) {
        result->message = "密码长度需为 8 到 256 字节，且不能全为空白字符";
        return StatusCode::kInvalidArgument;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    WebUser existing;
    const auto load_status = load_web_user_locked(request.username, &existing, error_message);
    if (is_ok(load_status)) {
        result->message = "用户名已存在";
        return StatusCode::kInvalidArgument;
    }
    if (load_status != StatusCode::kNotFound) return load_status;
    if (error_message != nullptr) error_message->clear();

    std::string hash_hex;
    std::string salt_hex;
    if (!security::create_password_hash(request.password, kDefaultWebPasswordIterations, &hash_hex, &salt_hex, error_message)) {
        return StatusCode::kInternalError;
    }
    const auto now_ms = time_utils::system_now_ms();
    WebUser user;
    user.username = request.username;
    user.display_name = request.display_name.empty() ? default_display_name_for_user(request.username, request.role) : request.display_name;
    user.role = request.role;
    user.password_hash = std::move(hash_hex);
    user.password_salt = std::move(salt_hex);
    user.password_iterations = kDefaultWebPasswordIterations;
    user.enabled = request.enabled;
    user.password_change_recommended = false;
    user.created_at_ms = now_ms;
    user.updated_at_ms = now_ms;
    const auto save_status = save_web_user_locked(user, error_message);
    if (!is_ok(save_status)) return save_status;
    result->success = true;
    result->message = "用户已创建";
    result->user = web_user_view(user);
    return StatusCode::kOk;
}

// 更新Web用户。
StatusCode ConfigStore::update_web_user(
    const WebUserUpdateRequest& request,
    WebUserMutationResult* result,
    std::string* error_message)
{
    if (result == nullptr) return StatusCode::kInvalidArgument;
    *result = {};
    if (!valid_username(request.username) || !valid_web_role(request.role)) {
        result->message = "用户或角色无效";
        return StatusCode::kInvalidArgument;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    WebUser user;
    const auto load_status = load_web_user_locked(request.username, &user, error_message);
    if (!is_ok(load_status)) return load_status;
    if (user.role != kWebRoleAdmin && normalize_web_role(request.role) == kWebRoleAdmin) {
        result->message = "不允许将普通用户提升为超级管理员";
        if (error_message != nullptr) {
            *error_message = result->message;
        }
        return StatusCode::kInvalidArgument;
    }
    if (user.enabled && user.role == kWebRoleAdmin &&
        (!request.enabled || normalize_web_role(request.role) != kWebRoleAdmin) &&
        enabled_super_admin_count(database_) <= 1) {
        result->message = "不能禁用或降级最后一个超级管理员";
        if (error_message != nullptr) {
            *error_message = result->message;
        }
        return StatusCode::kInvalidArgument;
    }
    user.display_name = request.display_name.empty() ? default_display_name_for_user(user.username, request.role) : request.display_name;
    user.role = normalize_web_role(request.role);
    user.enabled = request.enabled;
    user.updated_at_ms = time_utils::system_now_ms();
    const auto save_status = save_web_user_locked(user, error_message);
    if (!is_ok(save_status)) return save_status;
    result->success = true;
    result->message = "用户已更新";
    result->user = web_user_view(user);
    return StatusCode::kOk;
}

// 重置指定 Web 用户的密码。
StatusCode ConfigStore::reset_web_user_password(
    const WebUserPasswordResetRequest& request,
    WebUserMutationResult* result,
    std::string* error_message)
{
    if (result == nullptr) return StatusCode::kInvalidArgument;
    *result = {};
    if (!valid_username(request.username)) {
        result->message = "用户无效";
        return StatusCode::kInvalidArgument;
    }
    if (request.new_password.size() < 8U || request.new_password.size() > kMaxWebPasswordBytes || trim_ascii_copy(request.new_password).empty()) {
        result->message = "密码长度需为 8 到 256 字节，且不能全为空白字符";
        return StatusCode::kInvalidArgument;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    WebUser user;
    const auto load_status = load_web_user_locked(request.username, &user, error_message);
    if (!is_ok(load_status)) return load_status;
    if (security::verify_password(request.new_password, user.password_hash, user.password_salt, user.password_iterations)) {
        result->message = "新密码不能与当前密码相同";
        return StatusCode::kOk;
    }
    std::string hash_hex;
    std::string salt_hex;
    if (!security::create_password_hash(request.new_password, kDefaultWebPasswordIterations, &hash_hex, &salt_hex, error_message)) {
        return StatusCode::kInternalError;
    }
    const auto now_ms = time_utils::system_now_ms();
    user.password_hash = std::move(hash_hex);
    user.password_salt = std::move(salt_hex);
    user.password_iterations = kDefaultWebPasswordIterations;
    user.password_change_recommended = true;
    user.updated_at_ms = now_ms;
    user.last_password_change_ms = now_ms;
    const auto save_status = save_web_user_locked(user, error_message);
    if (!is_ok(save_status)) return save_status;
    result->success = true;
    result->message = "用户密码已重置";
    result->user = web_user_view(user);
    return StatusCode::kOk;
}

// 恢复已有超级管理员密码；该专用入口不会创建、提权或启用账号，也不修改其他业务数据。
StatusCode ConfigStore::recover_super_admin_password(
    const std::string& username,
    const std::string& new_password,
    WebUserMutationResult* result,
    std::string* error_message)
{
    if (result == nullptr) return StatusCode::kInvalidArgument;
    *result = {};
    if (!valid_username(username)) {
        result->message = "用户名只能包含字母、数字、点、下划线和中划线，长度 1-64";
        return StatusCode::kInvalidArgument;
    }
    if (new_password.size() < 8U || new_password.size() > kMaxWebPasswordBytes ||
        trim_ascii_copy(new_password).empty()) {
        result->message = "密码长度需为 8 到 256 字节，且不能全为空白字符";
        return StatusCode::kInvalidArgument;
    }
    if (new_password == kDefaultWebAdminPassword || new_password == kDefaultWebViewerPassword) {
        result->message = "恢复密码不能使用出厂默认密码";
        return StatusCode::kInvalidArgument;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    WebUser user;
    const auto load_status = load_web_user_locked(username, &user, error_message);
    if (!is_ok(load_status)) {
        if (load_status == StatusCode::kNotFound) result->message = "指定账号不存在";
        return load_status;
    }
    if (normalize_web_role(user.role) != kWebRoleAdmin) {
        result->message = "指定账号不是超级管理员";
        return StatusCode::kInvalidArgument;
    }
    if (!user.enabled) {
        result->message = "指定超级管理员账号已禁用，请先通过正式维护流程恢复账号状态";
        return StatusCode::kInvalidState;
    }
    if (security::verify_password(
            new_password, user.password_hash, user.password_salt, user.password_iterations)) {
        result->message = "恢复密码不能与当前密码相同";
        return StatusCode::kInvalidArgument;
    }

    std::string hash_hex;
    std::string salt_hex;
    if (!security::create_password_hash(
            new_password,
            kDefaultWebPasswordIterations,
            &hash_hex,
            &salt_hex,
            error_message)) {
        return StatusCode::kInternalError;
    }
    const auto now_ms = time_utils::system_now_ms();
    user.password_hash = std::move(hash_hex);
    user.password_salt = std::move(salt_hex);
    user.password_iterations = kDefaultWebPasswordIterations;
    user.password_change_recommended = true;
    user.updated_at_ms = now_ms;
    user.last_password_change_ms = now_ms;
    const auto save_status = save_web_user_locked(user, error_message);
    if (!is_ok(save_status)) return save_status;

    result->success = true;
    result->message = "超级管理员密码已恢复；首次登录后请再次确认并更新密码";
    result->user = web_user_view(user);
    return StatusCode::kOk;
}

// 在持锁状态下加载 Web 用户。
StatusCode ConfigStore::load_web_user_locked(
    const std::string& username,
    WebUser* user,
    std::string* error_message) const
{
    if (user == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 Web 账号输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    const char* sql =
        "SELECT username, display_name, role, password_hash, password_salt, password_iterations, "
        "enabled, password_change_recommended, created_at_ms, updated_at_ms, last_password_change_ms, last_login_at_ms "
        "FROM web_users WHERE username = ? LIMIT 1;";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备读取 web_users 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    if (!bind_text(statement.get(), 1, username)) {
        if (error_message != nullptr) {
            *error_message = "绑定 web_users.username 参数失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto step_status = sqlite3_step(statement.get());
    if (step_status == SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "web_users 尚未初始化";
        }
        return StatusCode::kNotFound;
    }
    if (step_status != SQLITE_ROW) {
        if (error_message != nullptr) {
            *error_message = "读取 web_users 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    WebUser loaded;
    loaded.username = column_text(statement.get(), 0);
    loaded.display_name = column_text(statement.get(), 1);
    loaded.role = normalize_web_role(column_text(statement.get(), 2));
    loaded.password_hash = column_text(statement.get(), 3);
    loaded.password_salt = column_text(statement.get(), 4);
    loaded.password_iterations = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 5));
    loaded.enabled = sqlite3_column_int(statement.get(), 6) != 0;
    loaded.password_change_recommended = sqlite3_column_int(statement.get(), 7) != 0;
    loaded.created_at_ms = static_cast<TimestampMs>(column_int64(statement.get(), 8));
    loaded.updated_at_ms = static_cast<TimestampMs>(column_int64(statement.get(), 9));
    loaded.last_password_change_ms = static_cast<TimestampMs>(column_int64(statement.get(), 10));
    loaded.last_login_at_ms = static_cast<TimestampMs>(column_int64(statement.get(), 11));
    if (loaded.display_name.empty()) {
        loaded.display_name = default_display_name_for_user(loaded.username, loaded.role);
    }
    if (loaded.username.empty() || loaded.role.empty() || loaded.password_hash.empty() || loaded.password_salt.empty() ||
        loaded.password_iterations == 0 || loaded.password_iterations > kMaxWebPasswordIterations) {
        if (error_message != nullptr) {
            *error_message = "web_users 账号数据不完整";
        }
        return StatusCode::kInvalidState;
    }
    if (!valid_username(loaded.username) || !valid_web_role(loaded.role)) {
        if (error_message != nullptr) {
            *error_message = "web_users 账号角色无效";
        }
        return StatusCode::kInvalidState;
    }
    *user = std::move(loaded);
    return StatusCode::kOk;
}

// 在持锁状态下保存 Web 用户。
StatusCode ConfigStore::save_web_user_locked(
    const WebUser& user,
    std::string* error_message)
{
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }
    if (!valid_username(user.username) || !valid_web_role(user.role) || user.password_hash.empty() || user.password_salt.empty() ||
        user.password_iterations == 0) {
        if (error_message != nullptr) {
            *error_message = "Web 账号数据不完整";
        }
        return StatusCode::kInvalidArgument;
    }

    const char* sql =
        "INSERT OR REPLACE INTO web_users "
        "(id, username, display_name, role, password_hash, password_salt, password_iterations, enabled, password_change_recommended, "
        "created_at_ms, updated_at_ms, last_password_change_ms, last_login_at_ms) "
        "VALUES ((SELECT id FROM web_users WHERE username=?), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备写入 web_users 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    const auto display_name = user.display_name.empty() ? default_display_name_for_user(user.username, user.role) : user.display_name;
    if (!bind_text(statement.get(), 1, user.username) ||
        !bind_text(statement.get(), 2, user.username) ||
        !bind_text(statement.get(), 3, display_name) ||
        !bind_text(statement.get(), 4, user.role) ||
        !bind_text(statement.get(), 5, user.password_hash) ||
        !bind_text(statement.get(), 6, user.password_salt) ||
        !bind_int(statement.get(), 7, static_cast<int>(user.password_iterations)) ||
        !bind_int(statement.get(), 8, user.enabled ? 1 : 0) ||
        !bind_int(statement.get(), 9, user.password_change_recommended ? 1 : 0) ||
        !bind_int64(statement.get(), 10, static_cast<sqlite3_int64>(user.created_at_ms)) ||
        !bind_int64(statement.get(), 11, static_cast<sqlite3_int64>(user.updated_at_ms)) ||
        !bind_int64(statement.get(), 12, static_cast<sqlite3_int64>(user.last_password_change_ms)) ||
        !bind_int64(statement.get(), 13, static_cast<sqlite3_int64>(user.last_login_at_ms))) {
        if (error_message != nullptr) {
            *error_message = "绑定 web_users 参数失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "写入 web_users 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 在持锁状态下初始化默认 Web 用户。
StatusCode ConfigStore::initialize_default_web_user_locked(
    const std::string& username,
    const std::string& role,
    const std::string& default_password,
    WebUser* user,
    std::string* error_message)
{
    std::string hash_hex;
    std::string salt_hex;
    if (!security::create_password_hash(
            default_password,
            kDefaultWebPasswordIterations,
            &hash_hex,
            &salt_hex,
            error_message)) {
        return StatusCode::kInternalError;
    }
    const auto now_ms = time_utils::system_now_ms();
    WebUser initialized;
    initialized.username = username;
    initialized.display_name = default_display_name_for_user(username, role);
    initialized.role = role;
    initialized.password_hash = std::move(hash_hex);
    initialized.password_salt = std::move(salt_hex);
    initialized.password_iterations = kDefaultWebPasswordIterations;
    initialized.enabled = true;
    initialized.password_change_recommended = true;
    initialized.created_at_ms = now_ms;
    initialized.updated_at_ms = now_ms;
    initialized.last_password_change_ms = 0;
    const auto save_status = save_web_user_locked(initialized, error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }
    if (user != nullptr) {
        *user = std::move(initialized);
    }
    return StatusCode::kOk;
}

}  // namespace edge_controller
