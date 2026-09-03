// 实现长度前缀 Unix Domain Socket 服务、连接循环和请求分派。
// 边界：只做参数校验、服务调用与稳定 JSON 契约转换。

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "shared/common/status_code.h"

namespace edge_controller {

class BackendService;

// IPC 监听生命周期；Recovering 表示进程仍在有限退避重建监听，Failed 为不可恢复终态。
enum class BackendIpcServerState {
    kStopped,
    kStarting,
    kRunning,
    kRecovering,
    kFailed,
    kStopping,
};

class BackendIpcServer {
public:
    using TerminalFailureCallback = std::function<void(StatusCode, const std::string&)>;
    // 返回非零 errno 可在指定监听操作注入失败；仅供状态机故障测试在 start 前配置。
    using ListenerFaultInjector = std::function<int(const std::string&)>;

    // 禁止复制后端 IPC 服务实例。
    BackendIpcServer(BackendService* backend_service, std::string socket_path);
    // 销毁 BackendIpcServer 实例并释放相关资源。
    ~BackendIpcServer();

    // 构造 BackendIpcServer 实例。
    BackendIpcServer(const BackendIpcServer&) = delete;
    // 禁止复制赋值后端 IPC 服务实例。
    BackendIpcServer& operator=(const BackendIpcServer&) = delete;

    // 启动 IPC 监听线程和客户端工作线程。
    StatusCode start();
    // 停止 IPC 监听并关闭所有客户端连接。
    void stop();
    // 判断 IPC 服务是否处于运行状态。
    bool is_running() const;
    // 返回当前监听生命周期状态。
    BackendIpcServerState state() const;
    // 返回监听终态失败；非 Failed 状态返回 kOk。
    StatusCode terminal_status(std::string* error_message = nullptr) const;
    // 返回当前监听使用的 Unix Domain Socket 路径。
    const std::string& socket_path() const;
    // 通过完整 UDS 帧和 health_check RPC 探测后端是否真实可服务。
    static StatusCode probe(
        const std::string& socket_path,
        std::chrono::milliseconds timeout,
        std::string* error_message = nullptr);
    // 配置监听终态失败通知；必须在 start 前调用。
    void set_terminal_failure_callback(TerminalFailureCallback callback);
    // 配置监听故障注入；必须在 start 前调用，仅供测试。
    void set_listener_fault_injector_for_test(ListenerFaultInjector injector);
    // 缩短恢复上限和退避；必须在 start 前调用，仅供测试。
    void set_listener_recovery_policy_for_test(
        std::uint32_t max_consecutive_failures,
        std::chrono::milliseconds base_backoff);

private:
    struct SlowIpcLogDecision {
        bool emit{false};
        std::uint64_t suppressed_count{0};
    };
    struct SlowIpcLogState {
        std::chrono::steady_clock::time_point last_emit{};
        std::chrono::steady_clock::time_point last_seen{};
        std::uint64_t suppressed_count{0};
    };

    // IPC 服务主循环，负责接受客户端连接。
    void server_loop();
    // 创建、绑定并发布新的监听 socket。
    StatusCode open_listen_socket(std::string* error_message);
    // 持有与 socket 路径配套的跨进程 flock，防止两个后端并发清理/绑定同一路径。
    StatusCode acquire_instance_lock(std::string* error_message);
    // 释放跨进程实例锁；锁文件保留，内核锁随 fd 释放。
    void release_instance_lock();
    // 关闭并删除当前实例拥有的监听 socket。
    void reset_listen_socket();
    // 等待有限退避；停止请求会提前唤醒。
    bool wait_for_recovery_backoff(std::uint32_t consecutive_failures);
    // 将不可恢复监听错误发布给 Application。
    void report_terminal_failure(StatusCode status, const std::string& error_message);
    // 返回指定监听操作的注入 errno，生产默认返回 0。
    int injected_listener_error(const std::string& operation);
    // 客户端连接由固定 worker 队列处理，避免请求风暴下无限创建线程。
    void handle_client_connection(int client_fd);
    // 客户端工作线程循环，从队列取连接并处理请求。
    void client_worker_loop();
    // 创建固定数量的客户端工作线程。
    void start_client_workers();
    // 停止客户端工作线程并等待退出。
    void stop_client_workers();
    // 将新客户端 socket 放入待处理队列。
    bool enqueue_client_socket(int client_fd);
    // 解析 IPC 请求 JSON、分发到后端服务并返回响应 JSON；可同时返回已解析的方法名供慢请求日志复用。
    std::string process_request(
        const std::string& request_json,
        std::string* parsed_method = nullptr);
    // 从 IPC socket 读取带长度帧的请求体。
    StatusCode read_frame(int fd, std::string* payload, std::string* error_message);
    // 向 IPC socket 写入带长度帧的响应体。
    StatusCode write_frame(int fd, const std::string& payload, std::string* error_message);
    // 记录 IPC 服务启动结果并通知等待线程。
    void mark_start_result(StatusCode status, const std::string& error_message);
    // 关闭监听 socket。
    void close_listen_socket();
    // 仅唤醒监听线程，不释放 fd 编号；最终 close 必须在 worker 退出后执行。
    void shutdown_listen_socket();
    // 记录活跃客户端 socket；停止开始后拒绝接管新的连接。
    bool track_client_socket(int client_fd);
    // 唤醒当前所有客户端 socket 上的阻塞读写，实际关闭仍由其 worker 完成。
    void shutdown_all_client_sockets();
    // 关闭单个客户端 socket 并从活跃集合移除。
    void close_client_socket(int client_fd);
    // 删除本实例成功绑定的 socket 文件，重复调用不会再次清理。
    void remove_socket_file();
    // 尝试获取客户端并发处理名额。
    bool try_acquire_client_slot();
    // 释放客户端并发处理名额。
    void release_client_slot();
    // 按 method 与成功/失败类别决定本次慢 IPC 是否输出，并返回窗口内累计抑制次数。
    SlowIpcLogDecision take_slow_ipc_log_decision(
        const std::string& method,
        bool succeeded);

    BackendService* backend_service_{nullptr};
    std::string socket_path_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<BackendIpcServerState> state_{BackendIpcServerState::kStopped};
    std::thread worker_;
    // start/stop 共用生命周期锁，避免并发 start 覆盖 joinable 线程。
    std::mutex lifecycle_mutex_;

    // 启动状态锁只保护监听 socket 的建立结果；等待启动完成时通过 start_cv_ 释放锁休眠。
    mutable std::mutex state_mutex_;
    std::condition_variable start_cv_;
    bool start_completed_{false};
    StatusCode start_status_{StatusCode::kInvalidState};
    std::string start_error_message_;
    StatusCode terminal_status_{StatusCode::kOk};
    std::string terminal_error_message_;
    TerminalFailureCallback terminal_failure_callback_;
    ListenerFaultInjector listener_fault_injector_;
    std::uint32_t max_consecutive_listener_failures_{5};
    std::chrono::milliseconds listener_recovery_base_backoff_{100};
    int listen_fd_{-1};
    int instance_lock_fd_{-1};
    bool socket_file_owned_{false};

    // 活跃 socket 集合独立加锁，停机可关闭连接而不依赖请求队列是否仍有任务。
    std::mutex client_fds_mutex_;
    std::unordered_set<int> client_fds_;

    // accept 线程只负责入队，固定 worker 池负责出队，避免为每个短连接创建新线程。
    std::mutex client_queue_mutex_;
    std::condition_variable client_queue_cv_;
    std::deque<int> client_queue_;
    std::vector<std::thread> client_workers_;
    std::atomic<int> active_client_tasks_{0};

    // 慢 IPC 独立限频表；键不包含耗时、错误正文或请求参数，容量受固定上限约束。
    std::mutex slow_ipc_log_mutex_;
    std::unordered_map<std::string, SlowIpcLogState> slow_ipc_logs_;
};

}  // namespace edge_controller
