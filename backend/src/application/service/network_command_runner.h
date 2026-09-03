// Linux 网络配置命令执行器：提供有界输出、总超时和进程组回收语义。
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "shared/common/status_code.h"

namespace edge_controller::network_runtime_internal {

struct CommandResult {
    int exit_code{-1};
    bool timed_out{false};
    bool output_truncated{false};
    std::string output;
};

// Detached 命令只等待 fork/setsid/exec 启动握手，不等待命令自身退出。
// 后两个字段仅用于让 Linux 回归测试稳定覆盖 exec 前超时与部分读取。
struct DetachedProcessOptions {
    std::chrono::milliseconds startup_timeout{std::chrono::seconds(3)};
    std::chrono::milliseconds pre_exec_delay_for_test{0};
    std::size_t max_wire_read_bytes_for_test{0};
};

StatusCode start_detached_process(
    const std::vector<std::string>& arguments,
    std::string* error_message,
    const DetachedProcessOptions& options = {});

// 执行受控网络命令。timeout 覆盖命令运行阶段；超时后仍会在固定短宽限期内回收进程组。
CommandResult run_command(
    const std::vector<std::string>& arguments,
    std::chrono::milliseconds timeout = std::chrono::seconds(10),
    std::size_t max_output_bytes = 16U * 1024U);

}  // namespace edge_controller::network_runtime_internal
