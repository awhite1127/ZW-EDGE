// 应用级离线升级入口：只调用固定升级引擎和受限标识，不接受任意路径或 shell。
#include "application/service/backend_service.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/service/network_command_runner.h"

namespace edge_controller {
namespace {

[[maybe_unused]] constexpr const char* kUpdateManagerPath = "/opt/edge-controller/scripts/update-manager.py";

bool safe_identifier_char(char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '.' || value == '_' || value == '+' || value == '-';
}

bool safe_package_identifier(const std::string& value)
{
    constexpr const char* prefix = "edge-controller-rk3562-";
    constexpr const char* suffix = ".tar.gz";
    if (value.size() <= std::char_traits<char>::length(prefix) + std::char_traits<char>::length(suffix) ||
        value.rfind(prefix, 0) != 0 ||
        value.compare(value.size() - std::char_traits<char>::length(suffix),
                      std::char_traits<char>::length(suffix), suffix) != 0) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), safe_identifier_char);
}

bool safe_job_id(const std::string& value)
{
    if (value.size() != 25 || value[8] != 'T' || value[15] != 'Z' || value[16] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 15 || index == 16) continue;
        const auto character = value[index];
        if (index < 15) {
            if (character < '0' || character > '9') return false;
        } else if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool safe_upload_id(const std::string& value)
{
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

StatusCode run_update_command(
    const std::vector<std::string>& arguments,
    std::chrono::milliseconds timeout,
    std::string* result_json,
    std::string* error_message)
{
    if (result_json == nullptr) {
        if (error_message != nullptr) *error_message = "升级命令结果输出参数为空";
        return StatusCode::kInvalidArgument;
    }
    result_json->clear();
#if !defined(__linux__)
    (void)arguments;
    (void)timeout;
    if (error_message != nullptr) *error_message = "应用级升级仅支持 RK3562 Linux 正式环境";
    return StatusCode::kInvalidState;
#else
    std::vector<std::string> command{"/usr/bin/python3", kUpdateManagerPath};
    command.insert(command.end(), arguments.begin(), arguments.end());
    const auto result = network_runtime_internal::run_command(command, timeout, 512U * 1024U);
    if (result.timed_out) {
        if (error_message != nullptr) *error_message = "升级引擎调用超时";
        return StatusCode::kTimeout;
    }
    if (result.exit_code != 0) {
        std::string engine_code;
        std::string engine_message;
        try {
            const auto parsed_error = nlohmann::json::parse(result.output);
            if (parsed_error.is_object()) {
                engine_code = parsed_error.value("error_code", std::string{});
                engine_message = parsed_error.value("error_message", std::string{});
            }
        } catch (const std::exception&) {
            // 非 JSON 输出仍作为有界诊断文本返回。
        }
        if (error_message != nullptr) {
            *error_message = !engine_message.empty() ?
                (engine_code.empty() ? engine_message : engine_code + "：" + engine_message) : result.output.empty() ?
                "升级引擎执行失败，退出码=" + std::to_string(result.exit_code) : result.output;
            if (result.output_truncated) *error_message += "（输出已截断）";
        }
        if (engine_code == "update_busy" || engine_code == "job_not_ready") {
            return StatusCode::kInvalidState;
        }
        if (engine_code == "package_not_found" || engine_code == "job_not_found") {
            return StatusCode::kNotFound;
        }
        if (engine_code.rfind("invalid_", 0) == 0 || engine_code == "package_mismatch") {
            return StatusCode::kInvalidArgument;
        }
        return StatusCode::kIoError;
    }
    try {
        const auto parsed = nlohmann::json::parse(result.output);
        if (!parsed.is_object()) {
            if (error_message != nullptr) *error_message = "升级引擎返回的 JSON 不是对象";
            return StatusCode::kProtocolError;
        }
        *result_json = parsed.dump();
        return StatusCode::kOk;
    } catch (const std::exception& error) {
        if (error_message != nullptr) *error_message = std::string("升级引擎返回无效 JSON：") + error.what();
        return StatusCode::kProtocolError;
    }
#endif
}

}  // namespace

StatusCode BackendService::get_update_current_version(std::string* result_json, std::string* error_message) const
{
    return run_update_command({"current-version"}, std::chrono::seconds(10), result_json, error_message);
}

StatusCode BackendService::get_update_status(std::string* result_json, std::string* error_message) const
{
    return run_update_command({"status"}, std::chrono::seconds(10), result_json, error_message);
}

StatusCode BackendService::validate_update_package(
    const std::string& package_identifier,
    std::string* result_json,
    std::string* error_message) const
{
    if (!safe_package_identifier(package_identifier)) {
        if (error_message != nullptr) *error_message = "升级包标识格式非法，只允许 incoming 中的正式 RK3562 包名";
        return StatusCode::kInvalidArgument;
    }
    return run_update_command(
        {"validate", "--package", package_identifier},
        std::chrono::minutes(5), result_json, error_message);
}

StatusCode BackendService::start_update_job(
    const std::string& job_id,
    std::string* result_json,
    std::string* error_message) const
{
    if (!safe_job_id(job_id)) {
        if (error_message != nullptr) *error_message = "升级任务 job_id 格式非法";
        return StatusCode::kInvalidArgument;
    }
    return run_update_command(
        {"start", "--job-id", job_id},
        std::chrono::seconds(30), result_json, error_message);
}

StatusCode BackendService::import_application_upgrade_package(
    const std::string& upload_id,
    const std::string& package_identifier,
    std::string* result_json,
    std::string* error_message) const
{
    if (!safe_upload_id(upload_id) || !safe_package_identifier(package_identifier)) {
        if (error_message != nullptr) *error_message = "上传标识或升级包名称格式非法";
        return StatusCode::kInvalidArgument;
    }
    return run_update_command(
        {"import-upload", "--upload-id", upload_id, "--package", package_identifier},
        std::chrono::minutes(5), result_json, error_message);
}

}  // namespace edge_controller
