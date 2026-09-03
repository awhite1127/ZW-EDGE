// 单 I/O 线程、poll 驱动的北向 Modbus TCP Server。
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "shared/common/status_code.h"
#include "data/model/modbus_server.h"
#include "communication/modbus_server/modbus_server_runtime_status.h"

namespace edge_controller {

class ModbusRegisterBank;

class ModbusTcpServer {
public:
    // 构造 Modbus TCP 服务并绑定寄存器库。
    ModbusTcpServer() = default;
    // 停止服务并释放监听与客户端资源。
    ~ModbusTcpServer();

    // 构造 Modbus TCP 服务并绑定寄存器库。
    ModbusTcpServer(const ModbusTcpServer&) = delete;
    ModbusTcpServer& operator=(const ModbusTcpServer&) = delete;

    // 启动 Modbus TCP 监听服务。
    StatusCode start(
        const ModbusServerSettings& settings,
        std::shared_ptr<ModbusRegisterBank> register_bank,
        std::string* error_message = nullptr);
    // 停止 Modbus TCP 监听服务。
    void stop();
    // 按新配置重启 Modbus TCP 服务。
    StatusCode restart(
        const ModbusServerSettings& settings,
        std::shared_ptr<ModbusRegisterBank> register_bank,
        std::string* error_message = nullptr);

    // 映射重载后只切换预填充 Bank，不中断监听或已有连接。
    void replace_register_bank(std::shared_ptr<ModbusRegisterBank> register_bank);
    // 判断 Modbus TCP 服务是否正在运行。
    bool is_running() const;
    // 返回 Modbus TCP 服务运行状态快照。
    ModbusServerRuntimeStatus get_runtime_status() const;

private:
    // 运行 Modbus TCP 服务的 I/O 线程。
    void io_thread_entry();
    // 记录服务启动结果。
    void set_startup_result(StatusCode status, const std::string& error_message);
    // 记录服务运行错误。
    void set_runtime_error(const std::string& error_message);
    // 关闭 I/O 线程唤醒管道。
    void close_wakeup_pipe();

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex status_mutex_;
    std::mutex startup_mutex_;
    std::condition_variable startup_cv_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    int wakeup_read_fd_{-1};
    int wakeup_write_fd_{-1};
    bool startup_complete_{false};
    StatusCode startup_status_{StatusCode::kInvalidState};
    std::string startup_error_;
    ModbusServerSettings settings_{};
    std::shared_ptr<ModbusRegisterBank> register_bank_;
    ModbusServerRuntimeStatus runtime_status_{};
};

}  // namespace edge_controller
