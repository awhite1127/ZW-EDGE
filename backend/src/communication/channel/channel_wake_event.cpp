#include "communication/channel/channel_wake_event.h"

#include <cerrno>
#include <cstdint>

#if defined(__linux__)
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace edge_controller::channel_internal {

ChannelWakeEvent::ChannelWakeEvent()
{
#if defined(__linux__)
    fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif
}

ChannelWakeEvent::~ChannelWakeEvent()
{
#if defined(__linux__)
    if (fd_ >= 0) {
        ::close(fd_);
    }
#endif
}

int ChannelWakeEvent::native_handle() const noexcept
{
    return fd_;
}

void ChannelWakeEvent::signal() noexcept
{
#if defined(__linux__)
    if (fd_ < 0) {
        return;
    }
    const std::uint64_t value = 1;
    while (::write(fd_, &value, sizeof(value)) < 0 && errno == EINTR) {
    }
#endif
}

void ChannelWakeEvent::reset() noexcept
{
#if defined(__linux__)
    if (fd_ < 0) {
        return;
    }
    std::uint64_t value = 0;
    while (true) {
        const auto read_size = ::read(fd_, &value, sizeof(value));
        if (read_size == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (read_size >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
#endif
}

}  // namespace edge_controller::channel_internal
