#include "channel/bounded_address_resolver.h"
#include "common/time_utils.h"

#include <iostream>

int main()
{
#if defined(__linux__)
    const auto result = edge_controller::channel_internal::resolve_addresses_until(
        "localhost",
        "80",
        edge_controller::time_utils::steady_now_ms() + 5000,
        nullptr,
        nullptr,
        -1);
    if (!edge_controller::is_ok(result.status) || result.addresses.empty()) {
        std::cerr << "resolver helper failed: " << result.error_message << '\n';
        return 1;
    }
    for (const auto& address : result.addresses) {
        if (address.family == 0 || address.socket_type == 0 || address.address.empty()) {
            std::cerr << "resolver helper returned an invalid address\n";
            return 1;
        }
    }
#endif
    return 0;
}
