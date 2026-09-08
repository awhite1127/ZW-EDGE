// 统一运行态快照仓库的稳定名称。
//
// DataStore 是历史兼容名称；新的服务代码应通过 RuntimeStateStore 表达其职责：
// 它只拥有内存中的运行态和不可变快照，不负责配置、历史或事件持久化。
#pragma once

#include "data/datastore/data_store.h"

namespace edge_controller {

using RuntimeStateStore = DataStore;

}  // namespace edge_controller
