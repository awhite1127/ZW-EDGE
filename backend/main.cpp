// 后端进程入口只负责启动 Application 并返回退出码，生命周期细节由应用装配层管理。
#include "application/app/application.h"
#include "communication/channel/bounded_address_resolver.h"
#include "shared/common/logger.h"
#include "application/interface/ipc_server.h"
#include "infrastructure/maintenance/admin_recovery.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

std::string configured_socket_path()
{
    const auto* configured = std::getenv("EDGE_CONTROLLER_IPC_SOCK");
    return configured != nullptr && configured[0] != '\0' ? configured : "/tmp/edge-controller.sock";
}

}  // namespace

// 启动后端进程并执行主运行流程。
int main(int argc, char** argv)
{
    try {
        if (argc > 1) {
#if defined(__linux__)
            if (argc == 4 &&
                std::string(argv[1]) ==
                    edge_controller::channel_internal::kAddressResolverHelperArgument) {
                edge_controller::channel_internal::run_address_resolver_helper(
                    argv[2], argv[3]);
            }
#endif
            if (argc == 2 && std::string(argv[1]) == "--ipc-health-check") {
                std::string error_message;
                const auto status = edge_controller::BackendIpcServer::probe(
                    configured_socket_path(),
                    std::chrono::milliseconds(1500),
                    &error_message);
                if (!edge_controller::is_ok(status)) {
                    std::cerr << (error_message.empty() ? "IPC health check failed" : error_message) << '\n';
                    return 1;
                }
                return 0;
            }
            if (argc == 3 && std::string(argv[1]) == "--recover-super-admin") {
                return edge_controller::maintenance::run_super_admin_recovery(argv[2]);
            }
            std::cerr << "用法：edge-controller [--ipc-health-check | --recover-super-admin <username>]\n";
            return 2;
        }

        edge_controller::Application application;

        const auto init_status = application.initialize();
        if (!edge_controller::is_ok(init_status)) {
            edge_controller::Logger::error("应用初始化失败");
            return 1;
        }

        const auto run_status = application.run();
        if (!edge_controller::is_ok(run_status)) {
            edge_controller::Logger::error("应用运行失败");
            return 1;
        }
    } catch (const std::exception& error) {
        edge_controller::Logger::error(
            "应用发生未处理异常：" + std::string(error.what()));
        return 1;
    } catch (...) {
        edge_controller::Logger::error("应用发生未知未处理异常");
        return 1;
    }

    return 0;
}
