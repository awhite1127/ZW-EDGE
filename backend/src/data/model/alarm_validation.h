// 告警规则领域校验。
// 边界：只包含与持久化和运行时评估共同适用的业务不变量，不依赖 Web 或 IPC。
#pragma once

#include <string>

#include "shared/common/status_code.h"
#include "data/model/alarm.h"

namespace edge_controller {

// 校验告警规则的领域不变量。
// 设备/数据项是否存在由 BackendService 的拓扑上下文校验；本函数只负责规则本身。
StatusCode validate_alarm_rule(
    const AlarmRule& rule,
    std::string* error_message = nullptr);

}  // namespace edge_controller
