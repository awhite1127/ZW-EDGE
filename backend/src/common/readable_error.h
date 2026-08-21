// 异常与错误文本的安全提取辅助接口。
#pragma once

#include <string>

namespace edge_controller {

struct ReadableErrorMessage {
    std::string summary;
    std::string detail;
};

// 将底层错误整理为可展示文本和诊断详情。
ReadableErrorMessage build_readable_runtime_error(const std::string& raw_message);

}  // namespace edge_controller
