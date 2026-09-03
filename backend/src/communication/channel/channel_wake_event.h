// 通道阻塞等待唤醒器：close/stop 可在不获取通道主锁时唤醒正在执行的 I/O。
#pragma once

namespace edge_controller::channel_internal {

class ChannelWakeEvent {
public:
    ChannelWakeEvent();
    ~ChannelWakeEvent();

    ChannelWakeEvent(const ChannelWakeEvent&) = delete;
    ChannelWakeEvent& operator=(const ChannelWakeEvent&) = delete;

    int native_handle() const noexcept;
    void signal() noexcept;
    void reset() noexcept;

private:
    int fd_{-1};
};

}  // namespace edge_controller::channel_internal
