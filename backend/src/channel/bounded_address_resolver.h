// 可取消、有总截止时间的地址解析器；Linux 上隔离同步 getaddrinfo，避免阻塞通道线程。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "common/status_code.h"
#include "common/types.h"

#if defined(__linux__)
#include <netdb.h>
#else
struct addrinfo;
#endif

namespace edge_controller::channel_internal {

struct ResolvedSocketAddress {
    int family{0};
    int socket_type{0};
    int protocol{0};
    std::vector<std::uint8_t> address;
};

struct AddressResolutionResult {
    StatusCode status{StatusCode::kInternalError};
    std::vector<ResolvedSocketAddress> addresses;
    std::string error_message;
};

using AddressResolverFunction =
    int (*)(const char*, const char*, const addrinfo*, addrinfo**);

AddressResolutionResult resolve_addresses_until(
    const std::string& host,
    const std::string& service,
    TimestampMs deadline_ms,
    const std::atomic<bool>* cancel_requested,
    const std::atomic<bool>* stop_requested,
    int wake_fd,
    AddressResolverFunction resolver = nullptr);

#if defined(__linux__)
// edge-controller 内部 exec helper 的固定输出描述符与入口。helper 在全新进程映像中
// 执行 NSS/getaddrinfo，避免多线程父进程 fork 后进入非 async-signal-safe 代码。
inline constexpr int kAddressResolverHelperOutputFd = 3;
inline constexpr const char* kAddressResolverHelperArgument = "--internal-address-resolver";

[[noreturn]] void run_address_resolver_helper(
    const std::string& host,
    const std::string& service,
    int output_fd = kAddressResolverHelperOutputFd);
#endif

}  // namespace edge_controller::channel_internal
