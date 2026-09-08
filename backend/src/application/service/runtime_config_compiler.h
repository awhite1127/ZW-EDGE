#pragma once
#include "data/datastore/config_store.h"
#include "application/manager/channel_manager.h"
#include "application/manager/topology_manager.h"

namespace edge_controller {
struct PreparedRuntimeConfig {
    SystemConfig system_config;
    TopologyManager topology_manager;
    ChannelManager channel_manager;
    ChannelOpenSummary channel_open_summary;
};

// 只构建候选运行态，不持有 BackendService 或修改正在使用的资源。
// 调用者串行化配置变更；成功后在配置锁内接管结果并重新绑定拓扑。
class RuntimeConfigCompiler {
public:
    explicit RuntimeConfigCompiler(ConfigStore& store) : store_(store) {}
    StatusCode prepare(PreparedRuntimeConfig* prepared, std::vector<std::string>* errors);
private:
    ConfigStore& store_;
};
}  // namespace edge_controller
