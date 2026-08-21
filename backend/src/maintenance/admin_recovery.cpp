// 本地维护命令：只恢复已有超级管理员密码，不暴露网络入口或修改其他业务数据。
#include "maintenance/admin_recovery.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "common/filesystem_compat.h"
#include "common/status_code.h"
#include "common/time_utils.h"
#include "datastore/config_store.h"
#include "datastore/database_paths.h"

#if defined(__linux__)
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace edge_controller::maintenance {

namespace {

std::string configured_data_directory()
{
    const auto* value = std::getenv("EDGE_CONTROLLER_DATA_DIR");
    return value != nullptr && value[0] != '\0'
               ? value
               : DatabasePaths::default_data_directory();
}

std::string configured_maintenance_log_path()
{
    const auto* value = std::getenv("EDGE_CONTROLLER_LOG_DIR");
    const auto directory = value != nullptr && value[0] != '\0'
                               ? edge::fs::path(value)
                               : edge::fs::path("/opt/edge-controller/log");
    return (directory / "maintenance.log").string();
}

std::string audit_text(std::string value)
{
    for (auto& ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = '?';
    }
    return value;
}

bool read_password_line(const char* prompt, std::string* value)
{
    if (value == nullptr) return false;
    std::cerr << prompt << std::flush;
#if defined(__linux__)
    const bool interactive = ::isatty(STDIN_FILENO) == 1;
    termios original{};
    bool echo_disabled = false;
    if (interactive && ::tcgetattr(STDIN_FILENO, &original) == 0) {
        auto hidden = original;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        echo_disabled = ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) == 0;
    }
#endif
    const bool read = static_cast<bool>(std::getline(std::cin, *value));
#if defined(__linux__)
    if (echo_disabled) {
        (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        std::cerr << '\n';
    }
#endif
    return read;
}

bool prepare_audit_log(std::ofstream* audit, std::string* error_message)
{
    if (audit == nullptr) return false;
    const edge::fs::path path(configured_maintenance_log_path());
    std::error_code ec;
    edge::fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error_message != nullptr) *error_message = "创建维护日志目录失败：" + ec.message();
        return false;
    }
#if defined(__linux__)
    const auto previous_mask = ::umask(0027);
#endif
    audit->open(path.string(), std::ios::out | std::ios::app);
#if defined(__linux__)
    (void)::umask(previous_mask);
    if (*audit) (void)::chmod(path.c_str(), 0640);
#endif
    if (!*audit) {
        if (error_message != nullptr) *error_message = "无法打开维护日志：" + path.string();
        return false;
    }
    return true;
}

void write_audit(std::ofstream* audit, const std::string& username, const std::string& result)
{
    if (audit == nullptr || !*audit) return;
    *audit << time_utils::local_time_string()
           << " action=recover-super-admin username=" << audit_text(username)
           << " result=" << result << '\n';
    audit->flush();
}

}  // namespace

int run_super_admin_recovery(const std::string& username)
{
#if !defined(__linux__)
    (void)username;
    std::cerr << "超级管理员恢复命令只支持部署目标 Linux 系统\n";
    return 1;
#else
    if (::geteuid() != 0) {
        std::cerr << "拒绝执行：超级管理员恢复命令仅允许 root 用户运行\n";
        return 1;
    }

    std::ofstream audit;
    std::string audit_error;
    if (!prepare_audit_log(&audit, &audit_error)) {
        std::cerr << "拒绝执行：无法建立维护审计记录：" << audit_error << '\n';
        return 1;
    }
    write_audit(&audit, username, "started");

    std::string password;
    std::string confirmation;
    if (!read_password_line("请输入新密码：", &password) ||
        !read_password_line("请再次输入新密码：", &confirmation)) {
        write_audit(&audit, username, "failed-input");
        std::cerr << "读取新密码失败\n";
        return 1;
    }
    if (password != confirmation) {
        write_audit(&audit, username, "failed-confirmation");
        std::cerr << "两次输入的新密码不一致\n";
        return 1;
    }

    const edge::fs::path database_path(
        DatabasePaths(configured_data_directory()).config_database());
    std::error_code database_ec;
    const auto database_status = edge::fs::symlink_status(database_path, database_ec);
    if (database_ec || !edge::fs::is_regular_file(database_status) ||
        edge::fs::is_symlink(database_status)) {
        write_audit(&audit, username, "failed-database-path");
        std::cerr << "配置数据库不存在或不是常规文件：" << database_path.string() << '\n';
        return 1;
    }

    ConfigStore store;
    std::string error_message;
    const auto init_status = store.initialize(database_path.string(), &error_message);
    if (!is_ok(init_status)) {
        write_audit(&audit, username, "failed-database-init");
        std::cerr << "打开配置数据库失败：" << error_message << '\n';
        return 1;
    }

    WebUserMutationResult result;
    const auto status = store.recover_super_admin_password(
        username, password, &result, &error_message);
    password.clear();
    confirmation.clear();
    if (!is_ok(status) || !result.success) {
        write_audit(&audit, username, "failed");
        const auto detail = !result.message.empty() ? result.message : error_message;
        std::cerr << "超级管理员密码恢复失败："
                  << (detail.empty() ? std::string(to_string(status)) : detail) << '\n';
        return 1;
    }

    write_audit(&audit, username, "success");
    std::cout << result.message << "（账号：" << username << "）\n";
    return 0;
#endif
}

}  // namespace edge_controller::maintenance
