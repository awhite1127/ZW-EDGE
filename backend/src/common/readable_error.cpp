// 将异常和底层错误统一提取为非空文本，供日志、IPC 和诊断事件使用。
#include "common/readable_error.h"

#include "model/diagnosis_status.h"

namespace edge_controller {

// 将底层错误整理为可展示文本和诊断详情。
ReadableErrorMessage build_readable_runtime_error(const std::string& raw_message)
{
    ReadableErrorMessage readable;
    readable.detail = raw_message;

    const auto error_code = classify_runtime_error(raw_message);
    const auto& definition = diagnosis_definition(error_code);
    readable.summary = definition.message;
    if (definition.suggestion[0] != '\0') {
        readable.detail = raw_message.empty()
                              ? definition.suggestion
                              : raw_message + "；" + definition.suggestion;
    }
    return readable;
}

}  // namespace edge_controller
