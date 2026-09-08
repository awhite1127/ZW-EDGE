// 系统概览快照：聚合进程资源、存储使用、轮询与 MQTT 状态；单个指标失败不影响其余字段。
#include "application/service/backend_service.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utility>

#include "shared/common/filesystem_compat.h"
#include "shared/common/time_utils.h"
#include "data/datastore/database_paths.h"
#include "application/service/backend_service_internal.h"

namespace edge_controller {

namespace {

constexpr double kLowStorageAvailableRatio = 0.10;

// 从系统状态文本解析以 KB 为单位的数值。
std::uint64_t parse_status_kb_value(const std::string& line)
{
    std::istringstream stream(line);
    std::string key;
    std::uint64_t value = 0;
    stream >> key >> value;
    return value;
}

// 从系统状态文本解析无符号整数。
std::uint32_t parse_status_uint_value(const std::string& line)
{
    std::istringstream stream(line);
    std::string key;
    std::uint32_t value = 0;
    stream >> key >> value;
    return value;
}

// 追加错误信息。
void append_error(std::string* target, const std::string& message)
{
    if (target == nullptr || message.empty()) {
        return;
    }
    if (!target->empty()) {
        *target += "；";
    }
    *target += message;
}

// 将系统错误码转换为可读文本。
std::string errno_message(const std::string& prefix)
{
    return prefix + "：" + std::strerror(errno);
}

// 读取文件大小，文件不存在或失败时返回零。
std::uint64_t file_size_or_zero(const edge::fs::path& path)
{
    std::error_code ec;
    if (path.empty() || !edge::fs::exists(path, ec) || ec) {
        return 0;
    }
    if (!edge::fs::is_regular_file(path, ec) || ec) {
        return 0;
    }
    const auto size = edge::fs::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

// 采集进程指标。
SystemProcessMetrics collect_process_metrics(
    const SystemStatus& system_status,
    TimestampMs sampled_at_ms)
{
    SystemProcessMetrics metrics;
    metrics.sampled_at_ms = sampled_at_ms;
    if (system_status.started_at_ms != 0 && sampled_at_ms >= system_status.started_at_ms) {
        metrics.backend_uptime_seconds = (sampled_at_ms - system_status.started_at_ms) / 1000;
    }

    std::string error_message;
    std::ifstream status_file("/proc/self/status");
    if (!status_file) {
        append_error(&error_message, "读取 /proc/self/status 失败");
    } else {
        std::string line;
        while (std::getline(status_file, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                metrics.vm_rss_kb = parse_status_kb_value(line);
            } else if (line.rfind("VmSize:", 0) == 0) {
                metrics.vm_size_kb = parse_status_kb_value(line);
            } else if (line.rfind("Threads:", 0) == 0) {
                metrics.thread_count = parse_status_uint_value(line);
            }
        }
    }

    DIR* fd_dir = opendir("/proc/self/fd");
    if (fd_dir == nullptr) {
        append_error(&error_message, errno_message("读取 /proc/self/fd 失败"));
    } else {
        std::uint32_t count = 0;
        const dirent* entry = nullptr;
        while ((entry = readdir(fd_dir)) != nullptr) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            ++count;
        }
        closedir(fd_dir);
        metrics.open_fd_count = count;
    }

    std::ifstream loadavg_file("/proc/loadavg");
    if (loadavg_file) {
        loadavg_file >> metrics.load_average_1m
                     >> metrics.load_average_5m
                     >> metrics.load_average_15m;
        metrics.load_average_available = !loadavg_file.fail();
    }

    metrics.available = error_message.empty();
    metrics.error_message = error_message;
    return metrics;
}

// 递归统计目录大小并返回可诊断错误。
bool directory_size_recursive(
    const edge::fs::path& directory,
    std::uint64_t* total_bytes,
    std::string* error_message)
{
    DIR* handle = opendir(directory.string().c_str());
    if (handle == nullptr) {
        if (error_message != nullptr) *error_message = errno_message("读取数据目录占用失败");
        return false;
    }
    bool ok = true;
    const dirent* entry = nullptr;
    while ((entry = readdir(handle)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const auto child = directory / name;
        struct stat stat_buffer {};
        if (lstat(child.string().c_str(), &stat_buffer) != 0) {
            if (error_message != nullptr) *error_message = errno_message("统计数据目录文件失败");
            ok = false;
            break;
        }
        if (S_ISREG(stat_buffer.st_mode)) {
            *total_bytes += static_cast<std::uint64_t>(stat_buffer.st_size);
        } else if (S_ISDIR(stat_buffer.st_mode) &&
                   !directory_size_recursive(child, total_bytes, error_message)) {
            ok = false;
            break;
        }
    }
    closedir(handle);
    return ok;
}

// 采集数据库指标。
DatabaseStorageMetrics collect_database_metrics(
    const std::string& role,
    const std::string& path)
{
    DatabaseStorageMetrics metrics;
    metrics.role = role;
    metrics.path = path;
    metrics.size_bytes = file_size_or_zero(path);
    metrics.wal_size_bytes = file_size_or_zero(edge::fs::path(path + "-wal"));
    metrics.shm_size_bytes = file_size_or_zero(edge::fs::path(path + "-shm"));
    return metrics;
}

// 采集存储指标。
SystemStorageMetrics collect_storage_metrics(const DatabasePaths& paths)
{
    SystemStorageMetrics metrics;
    metrics.storage_path = paths.data_directory();
    metrics.databases = {
        collect_database_metrics("config", paths.config_database()),
        collect_database_metrics("history", paths.history_database()),
        collect_database_metrics("events", paths.events_database()),
    };
    for (const auto& database : metrics.databases) {
        metrics.sqlite_database_size_bytes += database.size_bytes;
        metrics.sqlite_auxiliary_size_bytes += database.wal_size_bytes + database.shm_size_bytes;
    }

    if (metrics.storage_path.empty()) {
        metrics.error_message = "数据目录路径为空，无法读取存储状态";
        metrics.available = false;
        return metrics;
    }

    std::string directory_size_error;
    if (!directory_size_recursive(metrics.storage_path, &metrics.data_directory_size_bytes, &directory_size_error)) {
        append_error(&metrics.error_message, directory_size_error);
    }

    struct statvfs stat {};
    if (statvfs(metrics.storage_path.c_str(), &stat) != 0) {
        append_error(&metrics.error_message, errno_message("读取数据目录容量失败"));
        metrics.available = false;
        return metrics;
    }

    const auto block_size = stat.f_frsize != 0 ? stat.f_frsize : stat.f_bsize;
    metrics.storage_total_bytes = static_cast<std::uint64_t>(stat.f_blocks) * block_size;
    metrics.storage_free_bytes = static_cast<std::uint64_t>(stat.f_bfree) * block_size;
    metrics.storage_available_bytes = static_cast<std::uint64_t>(stat.f_bavail) * block_size;
    if (metrics.storage_total_bytes > 0) {
        const auto used_bytes = metrics.storage_total_bytes > metrics.storage_free_bytes
                                    ? metrics.storage_total_bytes - metrics.storage_free_bytes
                                    : 0;
        metrics.storage_used_percent =
            static_cast<double>(used_bytes) * 100.0 / static_cast<double>(metrics.storage_total_bytes);
    }
    metrics.available = metrics.error_message.empty();
    return metrics;
}

// 汇总健康状态。
SystemHealthSummary summarize_health(
    const ServiceErrorSummary& current_error,
    const SystemStorageMetrics& storage)
{
    SystemHealthSummary health;
    if (current_error.has_error) {
        health.level = "error";
        health.message = current_error.message.empty()
                             ? "当前存在运行诊断异常"
                             : current_error.message;
        return health;
    }
    if (!storage.available) {
        health.level = "warning";
        health.message = "存储状态暂时无法读取";
        return health;
    }
    if (storage.storage_total_bytes > 0 &&
        static_cast<double>(storage.storage_available_bytes) /
            static_cast<double>(storage.storage_total_bytes) < kLowStorageAvailableRatio) {
        health.level = "warning";
        health.message = "数据目录剩余空间偏低";
        return health;
    }
    health.level = "normal";
    health.message = "系统运行状态正常";
    return health;
}

}  // namespace

// 读取系统概览快照。
SystemOverviewSnapshot BackendService::get_system_overview_snapshot() const
{
    SystemOverviewSnapshot snapshot;
    std::string data_directory;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        snapshot.generated_at_ms = time_utils::system_now_ms();
        snapshot.system_status = data_store_.get_system_status();
        snapshot.polling = polling_runtime_.get() != nullptr
                               ? polling_runtime_.get()->get_last_cycle_summary()
                               : data_store_.get_polling_summary();
        snapshot.current_error = backend_internal::build_current_error_summary(snapshot.system_status);
        snapshot.mqtt_runtime = mqtt_publisher_service_.runtime_status();
        snapshot.modbus_server_runtime = modbus_tcp_server_.get_runtime_status();
        const auto config_database_path = config_store_.database_path();
        if (!config_database_path.empty()) {
            data_directory = edge::fs::path(config_database_path).parent_path().string();
        }
    }

    snapshot.process = collect_process_metrics(snapshot.system_status, snapshot.generated_at_ms);
    snapshot.storage = collect_storage_metrics(DatabasePaths(data_directory));
    snapshot.health = summarize_health(snapshot.current_error, snapshot.storage);
    return snapshot;
}

// 读取概览页面快照。
StatusCode BackendService::get_overview_page_snapshot(
    OverviewPageSnapshot* snapshot,
    std::string* error_message) const
{
    if (snapshot == nullptr) {
        if (error_message != nullptr) {
            *error_message = "系统概览首屏快照输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }

    OverviewPageSnapshot result;
    {
        // 配置相关字段在同一个锁区间复制，避免首屏看到不同配置代次的通道、主站和设备。
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        result.system_settings = system_config_.settings;
        result.config_summary.project_name = system_config_.project_name;
        result.config_summary.project_version = system_config_.project_version;
        result.config_summary.site_id = system_config_.site_id;
        result.config_summary.default_poll_interval_ms = system_config_.default_poll_interval_ms;
        result.config_summary.channel_count = system_config_.channels.size();
        result.config_summary.master_count = system_config_.master_nodes.size();
        result.config_summary.device_count = system_config_.devices.size();
        result.config_summary.device_templates = device_templates();
        result.channels = system_config_.channels;
        result.masters = system_config_.master_nodes;
        result.devices = system_config_.devices;
    }

    // 进程/存储采样和 SQLite 查询都在 service_mutex_ 外执行。
    result.system_overview_snapshot = get_system_overview_snapshot();
    const auto event_status = get_recent_events(5, &result.recent_events, error_message);
    if (!is_ok(event_status)) {
        return event_status;
    }
    const auto alarm_status = list_active_alarms(&result.active_alarms, error_message);
    if (!is_ok(alarm_status)) {
        return alarm_status;
    }

    *snapshot = std::move(result);
    return StatusCode::kOk;
}

}  // namespace edge_controller
