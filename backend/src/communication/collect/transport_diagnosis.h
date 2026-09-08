#pragma once
#include "data/model/channel_status.h"
#include "shared/common/status_code.h"

namespace edge_controller {
inline DiagnosisErrorCode transport_diagnosis(StatusCode status, const ChannelStatus& channel, bool tcp = false)
{
    const auto code = diagnosis_code_from_string(channel.diagnosis.error_code);
    if (code != DiagnosisErrorCode::kNone && code != DiagnosisErrorCode::kUnknownError) return code;
    if (status == StatusCode::kTimeout)
        return tcp ? DiagnosisErrorCode::kTcpResponseTimeout : DiagnosisErrorCode::kModbusTimeout;
    return DiagnosisErrorCode::kChannelIoError;
}
}  // namespace edge_controller
