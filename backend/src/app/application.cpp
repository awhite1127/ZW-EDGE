// 应用装配层：负责创建后端服务、IPC 服务和生命周期顺序，不承载采集协议业务。
#include "app/application.h"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <system_error>
#include <thread>
#include "common/logger.h"

#if defined(__linux__)
#include <pthread.h>
#include <signal.h>
#include <time.h>
#endif

namespace edge_controller {

namespace {

std::atomic<bool> g_stop_requested{false};
std::mutex g_stop_mutex;
std::condition_variable g_stop_cv;

// 请求应用停止运行；既用于系统信号，也用于 IPC 监听终态失败。
void request_stop()
{
    g_stop_requested.store(true);
    g_stop_cv.notify_all();
}

#if !defined(__linux__)
// 处理进程停止信号。
void handle_stop_signal(int)
{
    request_stop();
}
#endif

#if defined(__linux__)
// 阻塞停止信号并交由专用线程处理。
void block_stop_signals()
{
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
}
#endif

}  // namespace

// 初始化后端服务、持久化网络模式和 IPC 服务。
StatusCode Application::initialize()
{
    g_stop_requested.store(false);
    ipc_terminal_failure_.store(false);
    {
        std::lock_guard<std::mutex> lock(ipc_failure_mutex_);
        ipc_failure_status_ = StatusCode::kOk;
        ipc_failure_message_.clear();
    }
#if defined(__linux__)
    // 产品时区以 /etc/localtime 为唯一真源。在线程启动前清除外部 TZ 覆盖，后续切换只需
    // 原子替换 /etc/localtime 并调用 tzset，不再在运行期修改全局 environ。
    if (::unsetenv("TZ") != 0) {
        Logger::warn("清除 TZ 环境覆盖失败，进程时区可能与系统设置不一致");
    }
    ::tzset();
    // Linux 下先屏蔽主线程的退出信号，后续统一交给 sigwait 线程处理，避免信号打断初始化流程。
    block_stop_signals();
#endif

    Logger::info("Edge Controller 软件版本：" + std::string(EDGE_CONTROLLER_PACKAGE_VERSION));

    // 后端启动主路径：从 SQLite 数据目录加载配置和模板，再推导运行态拓扑。
    const auto load_status = backend_service_.initialize(resolve_data_directory());
    if (!is_ok(load_status)) {
        Logger::error("后端初始化失败");
        return load_status;
    }

    std::string time_restore_error;
    const auto time_restore_status = backend_service_.restore_time_settings_on_startup(&time_restore_error);
    if (!is_ok(time_restore_status)) {
        Logger::warn(
            "启动时恢复时间设置失败，后端将继续启动：" +
            (time_restore_error.empty() ? std::string("未知错误") : time_restore_error));
    }

    std::string network_apply_error;
    const auto network_apply_status = backend_service_.apply_network_settings_on_startup(&network_apply_error);
    if (!is_ok(network_apply_status)) {
        Logger::warn(
            "启动时恢复网络配置失败，后端将继续启动：" +
            (network_apply_error.empty() ? std::string("未知错误") : network_apply_error));
    }

    // Web 管理台只通过 UDS + JSON-RPC 访问后端，IPC 服务启动失败时应用不能继续对外提供能力。
    ipc_server_ = std::make_unique<BackendIpcServer>(&backend_service_, resolve_socket_path());
    ipc_server_->set_terminal_failure_callback(
        [this](StatusCode status, const std::string& error_message) {
            handle_ipc_terminal_failure(status, error_message);
        });
    const auto ipc_status = ipc_server_->start();
    if (!is_ok(ipc_status)) {
        Logger::error("启动 IPC 服务失败");
        ipc_server_.reset();
        return ipc_status;
    }

    initialized_ = true;
    Logger::info("应用初始化完成");
    Logger::info("IPC Unix Domain Socket 路径: " + ipc_server_->socket_path());
    print_topology_summary();
    return StatusCode::kOk;
}

// 启动采集轮询并等待进程停止信号。
StatusCode Application::run()
{
    if (!initialized_) {
        return StatusCode::kInvalidState;
    }

    // start() 返回后监听线程仍可能立刻进入终态；此时不要再启动采集或信号线程。
    if (ipc_terminal_failure_.load()) {
        StatusCode failure_status = StatusCode::kIoError;
        {
            std::lock_guard<std::mutex> lock(ipc_failure_mutex_);
            if (!is_ok(ipc_failure_status_)) failure_status = ipc_failure_status_;
        }
        shutdown();
        return failure_status;
    }

    const auto polling_status = backend_service_.start_polling();
    if (!is_ok(polling_status)) {
        Logger::error("初始轮询启动失败，服务保持运行并等待配置修正");
    }

#if defined(__linux__)
    // 使用独立线程等待 SIGINT/SIGTERM，主线程只负责轮询生命周期和优雅退出。
    std::thread signal_thread;
    try {
        signal_thread = std::thread([]() {
            sigset_t signals;
            sigemptyset(&signals);
            sigaddset(&signals, SIGINT);
            sigaddset(&signals, SIGTERM);
            int signal_number = 0;
            if (sigwait(&signals, &signal_number) == 0) {
                Logger::info("收到停止信号: " + std::to_string(signal_number));
                request_stop();
            }
        });
    } catch (const std::system_error& error) {
        Logger::error("创建信号等待线程失败：" + std::string(error.what()));
        shutdown();
        return StatusCode::kInternalError;
    }
#else
    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);
#endif

    {
        std::unique_lock<std::mutex> lock(g_stop_mutex);
        g_stop_cv.wait(lock, []() { return g_stop_requested.load(); });
    }

    Logger::info("后端正在退出");
    const bool ipc_failed = ipc_terminal_failure_.load();
#if defined(__linux__)
    if (signal_thread.joinable()) {
        // IPC 内部失败不会产生外部信号；显式唤醒 sigwait 线程，避免主线程 join 永久阻塞。
        if (ipc_failed) {
            const int wake_result = pthread_kill(signal_thread.native_handle(), SIGTERM);
            if (wake_result != 0 && wake_result != ESRCH) {
                Logger::warn("唤醒信号等待线程失败，错误码=" + std::to_string(wake_result));
            }
        }
        signal_thread.join();
    }
#endif
    StatusCode run_status = StatusCode::kOk;
    std::string ipc_failure_message;
    if (ipc_failed) {
        std::lock_guard<std::mutex> lock(ipc_failure_mutex_);
        run_status = is_ok(ipc_failure_status_) ? StatusCode::kIoError : ipc_failure_status_;
        ipc_failure_message = ipc_failure_message_;
    }
    // 信号线程已回收后再执行可能触发异常的组件收尾，避免 joinable thread 析构导致进程 terminate。
    shutdown();
    if (!is_ok(run_status)) {
        Logger::error(
            "应用因 IPC 监听终态失败退出：" +
            (ipc_failure_message.empty() ? std::string("未知错误") : ipc_failure_message));
    }
    return run_status;
}

// 停止 IPC 服务和后端采集服务。
void Application::shutdown()
{
    if (ipc_server_ != nullptr) {
        ipc_server_->stop();
        ipc_server_.reset();
    }
    backend_service_.shutdown();
    initialized_ = false;
    Logger::info("应用已退出");
}

// 记录 IPC 不可恢复错误，并唤醒 Application::run 的主等待条件。
void Application::handle_ipc_terminal_failure(StatusCode status, const std::string& error_message)
{
    {
        std::lock_guard<std::mutex> lock(ipc_failure_mutex_);
        ipc_failure_status_ = is_ok(status) ? StatusCode::kIoError : status;
        ipc_failure_message_ = error_message;
    }
    ipc_terminal_failure_.store(true);
    request_stop();
}

// 输出当前通道、主站和设备拓扑摘要。
void Application::print_topology_summary() const
{
    Logger::info("拓扑摘要:");
    for (const auto& channel : backend_service_.get_channels()) {
        Logger::info("通道 " + channel.channel_id + " 目标=" + channel_target_description(channel));

        for (const auto& master : backend_service_.get_masters_by_channel(channel.channel_id)) {
            Logger::info(
                "  主控 " + master.master_id +
                " 地址=" + std::to_string(master.target_address) +
                " 设备数=" + std::to_string(master.device_count));

            for (const auto& device : backend_service_.get_devices_by_master(master.master_id)) {
                Logger::info(
                    "    设备 " + device.device_id +
                    " 偏移=" + std::to_string(device.register_offset) +
                    " 模板=" + master.device_template);
            }
        }
    }
}

// 解析后端数据目录。
std::string Application::resolve_data_directory() const
{
    if (const auto* env = std::getenv("EDGE_CONTROLLER_DATA_DIR"); env != nullptr && env[0] != '\0') {
        return env;
    }
    return {};
}

// 解析 IPC socket 路径。
std::string Application::resolve_socket_path() const
{
    if (const auto* env = std::getenv("EDGE_CONTROLLER_IPC_SOCK"); env != nullptr && env[0] != '\0') {
        return env;
    }
    return "/tmp/edge-controller.sock";
}

}  // namespace edge_controller
