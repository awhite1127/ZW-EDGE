// 编排后端组件初始化、启动、主循环与有序退出，是进程级生命周期的唯一入口。
// 边界：生命周期细节由应用装配层统一管理。

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "shared/common/status_code.h"
#include "application/interface/ipc_server.h"
#include "application/service/backend_service.h"

namespace edge_controller {

class Application {
public:
    // 初始化后端服务、运行目录和 IPC 服务。
    StatusCode initialize();
    // 启动主循环，保持后端进程运行并处理停止信号。
    StatusCode run();
    // 停止 IPC、轮询和后端服务，释放运行资源。
    void shutdown();

private:
    // 输出当前通道、主站和设备的拓扑摘要日志。
    void print_topology_summary() const;
    // 解析本次运行使用的数据目录。
    std::string resolve_data_directory() const;
    // 解析 IPC Unix Domain Socket 路径。
    std::string resolve_socket_path() const;
    // 接收 IPC 监听不可恢复失败并唤醒主运行循环。
    void handle_ipc_terminal_failure(StatusCode status, const std::string& error_message);

    bool initialized_{false};
    BackendService backend_service_{};
    std::atomic<bool> ipc_terminal_failure_{false};
    mutable std::mutex ipc_failure_mutex_;
    StatusCode ipc_failure_status_{StatusCode::kOk};
    std::string ipc_failure_message_;
    // 最后声明以确保析构时最先停止 IPC，failure 字段和 BackendService 仍保持有效。
    std::unique_ptr<BackendIpcServer> ipc_server_{};
};

}  // namespace edge_controller
