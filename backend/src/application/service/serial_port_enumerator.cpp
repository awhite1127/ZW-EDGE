// 枚举 Linux 串口并生成稳定、可读的候选信息。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#include "application/service/serial_port_enumerator.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>

#include "shared/common/filesystem_compat.h"
#include "shared/common/logger.h"

#if defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace edge_controller {

namespace {

#if defined(__linux__)
struct SerialSymlinkMaps {
    std::unordered_map<std::string, std::string> by_id;
    std::unordered_map<std::string, std::string> by_path;
};

// 判断文本是否以指定前缀开头。
bool starts_with(const std::string& text, const char* prefix)
{
    return text.rfind(prefix, 0) == 0;
}

// 判断设备节点名称是否属于支持的串口类型。
bool is_supported_serial_name(const std::string& name)
{
    return starts_with(name, "ttyS") ||
           starts_with(name, "ttyUSB") ||
           starts_with(name, "ttyACM") ||
           starts_with(name, "ttyAMA") ||
           starts_with(name, "ttyTHS");
}

// 判断设备节点是否为板载 ttyS 串口。
bool is_onboard_ttyS_name(const std::string& name)
{
    return starts_with(name, "ttyS");
}

// 根据设备节点名称识别串口类型。
std::string detect_kind(const std::string& name)
{
    if (starts_with(name, "ttyUSB") || starts_with(name, "ttyACM")) {
        return "usb-serial";
    }
    if (starts_with(name, "ttyS") || starts_with(name, "ttyAMA") || starts_with(name, "ttyTHS")) {
        return "uart";
    }
    return "unknown";
}

// 返回不同串口类型的基础说明。
std::string base_description_for_kind(const std::string& kind)
{
    if (kind == "usb-serial") {
        return "USB 串口适配器";
    }
    if (kind == "uart") {
        return "板载或 SoC UART";
    }
    return "串口设备";
}

// 解析符号链接并返回规范化的真实路径。
std::string normalized_real_path(const edge::fs::path& path)
{
    std::error_code ec;
    const auto canonical_path = edge::fs::canonical(path, ec);
    if (!ec) {
        return canonical_path.string();
    }

    ec.clear();
    const auto absolute_path = edge::fs_absolute(path, ec);
    if (!ec) {
        return edge::fs_lexically_normal(absolute_path).string();
    }

    return path.string();
}

// 读取当前被系统控制台占用的 TTY 名称。
std::set<std::string> load_active_console_names()
{
    std::set<std::string> result;

    std::ifstream input("/sys/class/tty/console/active");
    if (!input.is_open()) {
        return result;
    }

    std::string token;
    while (input >> token) {
        result.insert(token);
    }

    return result;
}

// 扫描目录中的串口符号链接并更新映射。
void scan_symlink_directory(
    const edge::fs::path& directory,
    std::unordered_map<std::string, std::string>* output)
{
    if (output == nullptr) {
        return;
    }

    std::error_code ec;
    if (!edge::fs::exists(directory, ec) || ec || !edge::fs::is_directory(directory, ec)) {
        return;
    }

    const auto options = edge::fs::directory_options::skip_permission_denied;
    for (edge::fs::directory_iterator it(directory, options, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const auto real_path = normalized_real_path(it->path());
        if (real_path.empty()) {
            continue;
        }

        output->emplace(real_path, it->path().string());
    }
}

// 加载串口设备路径与稳定符号链接之间的映射。
SerialSymlinkMaps load_symlink_maps()
{
    SerialSymlinkMaps maps;
    scan_symlink_directory("/dev/serial/by-id", &maps.by_id);
    scan_symlink_directory("/dev/serial/by-path", &maps.by_path);
    return maps;
}

// 判断路径是否指向字符设备。
bool is_character_device(const edge::fs::path& path)
{
#if !defined(__linux__)
    (void)path;
    return false;
#else
    struct stat stat_buffer {};
    if (::stat(path.string().c_str(), &stat_buffer) != 0) {
        return false;
    }
    return S_ISCHR(stat_buffer.st_mode);
#endif
}

// 从 sysfs 识别串口使用的内核驱动。
std::string detect_driver_name(const std::string& tty_name)
{
    std::error_code ec;
    const auto driver_link = edge::fs::read_symlink(
        edge::fs::path("/sys/class/tty") / tty_name / "device" / "driver",
        ec);
    if (ec) {
        return {};
    }
    return driver_link.filename().string();
}

// 判断串口在 sysfs 中是否存在对应设备节点。
bool has_sysfs_device_node(const std::string& tty_name)
{
    std::error_code ec;
    const auto device_path = edge::fs::path("/sys/class/tty") / tty_name / "device";
    const auto status = edge::fs::symlink_status(device_path, ec);
    return !ec && edge::fs::exists(status);
}

// 检查串口路径可用性并生成说明。
std::string availability_note_for_path(const std::string& path, bool* available)
{
    if (available == nullptr) {
        return {};
    }

#if !defined(__linux__)
    (void)path;
    *available = false;
    return "串口发现仅支持 Linux";
#else
    errno = 0;
    if (::access(path.c_str(), R_OK | W_OK) == 0) {
        *available = true;
        return {};
    }

    *available = false;
    if (errno == EACCES) {
        return "当前进程没有读写权限";
    }
    if (errno == ENOENT) {
        return "扫描过程中设备节点已消失";
    }
    return "访问探测失败: " + std::string(std::strerror(errno));
#endif
}

// 提取路径末级名称，空路径返回空字符串。
std::string basename_or_empty(const std::string& path)
{
    if (path.empty()) {
        return {};
    }
    return edge::fs::path(path).filename().string();
}

// 拼接非空文本片段。
std::string join_non_empty(const std::vector<std::string>& parts)
{
    std::ostringstream stream;
    bool first = true;
    for (const auto& part : parts) {
        if (part.empty()) {
            continue;
        }
        if (!first) {
            stream << "; ";
        }
        stream << part;
        first = false;
    }
    return stream.str();
}

// 构造串口候选项的显示名称。
std::string build_display_name(const SerialPortInfo& info)
{
    std::ostringstream stream;
    stream << info.name << " (" << info.path << ")";
    if (!info.symlink_by_id.empty()) {
        stream << " [" << basename_or_empty(info.symlink_by_id) << "]";
    }
    return stream.str();
}

// 构造描述。
std::string build_description(const SerialPortInfo& info, const std::string& availability_note)
{
    std::vector<std::string> parts;
    parts.push_back(base_description_for_kind(info.kind));

    if (!info.driver.empty()) {
        parts.push_back("驱动=" + info.driver);
    }
    if (!info.symlink_by_path.empty()) {
        parts.push_back("路径别名=" + basename_or_empty(info.symlink_by_path));
    }
    if (!availability_note.empty()) {
        parts.push_back(availability_note);
    }

    return join_non_empty(parts);
}
#endif

}  // namespace

// 枚举系统串口候选项并补充可读描述。
StatusCode SerialPortEnumerator::enumerate(
    std::vector<SerialPortInfo>* ports,
    std::string* error_message) const
{
    if (ports == nullptr) {
        if (error_message != nullptr) {
            *error_message = "串口列表输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }

    ports->clear();

#if !defined(__linux__)
    if (error_message != nullptr) {
        *error_message = "串口发现仅支持 Linux /dev 扫描";
    }
    return StatusCode::kInvalidState;
#else
    // 基于 /dev 和 /dev/serial 符号链接扫描，避免依赖外部命令。
    const edge::fs::path dev_directory("/dev");
    std::error_code ec;
    if (!edge::fs::exists(dev_directory, ec) || ec || !edge::fs::is_directory(dev_directory, ec)) {
        if (error_message != nullptr) {
            *error_message = "/dev 不可访问，无法继续发现串口";
        }
        return StatusCode::kIoError;
    }

    const auto active_console_names = load_active_console_names();
    const auto symlink_maps = load_symlink_maps();
    std::set<std::string> seen_real_paths;

    const auto options = edge::fs::directory_options::skip_permission_denied;
    for (edge::fs::directory_iterator it(dev_directory, options, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const auto device_path = it->path();
        const auto tty_name = device_path.filename().string();
        if (!is_supported_serial_name(tty_name)) {
            continue;
        }

        // 排除已被系统控制台占用的 TTY，避免将保留端口提供给业务绑定。
        if (active_console_names.find(tty_name) != active_console_names.end()) {
            continue;
        }

        if (!is_character_device(device_path)) {
            continue;
        }

        const auto driver_name = detect_driver_name(tty_name);
        if (is_onboard_ttyS_name(tty_name) && (!has_sysfs_device_node(tty_name) || driver_name.empty())) {
            if (Logger::debug_enabled()) {
                Logger::debug("跳过无有效硬件驱动的 ttyS 串口候选: " + tty_name);
            }
            continue;
        }

        const auto real_path = normalized_real_path(device_path);
        if (!seen_real_paths.insert(real_path).second) {
            continue;
        }

        SerialPortInfo info;
        info.path = device_path.string();
        info.name = tty_name;
        info.kind = detect_kind(tty_name);

        const auto by_id_it = symlink_maps.by_id.find(real_path);
        if (by_id_it != symlink_maps.by_id.end()) {
            info.symlink_by_id = by_id_it->second;
        }

        const auto by_path_it = symlink_maps.by_path.find(real_path);
        if (by_path_it != symlink_maps.by_path.end()) {
            info.symlink_by_path = by_path_it->second;
            info.physical_hint = basename_or_empty(by_path_it->second);
        }

        info.driver = driver_name;
        const auto availability_note = availability_note_for_path(info.path, &info.available);
        info.busy = false;
        info.display_name = build_display_name(info);
        info.description = build_description(info, availability_note);

        ports->push_back(std::move(info));
    }

    std::sort(ports->begin(), ports->end(), [](const SerialPortInfo& left, const SerialPortInfo& right) {
        return left.path < right.path;
    });

    return StatusCode::kOk;
#endif
}

}  // namespace edge_controller
