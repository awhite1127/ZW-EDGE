#pragma once
#include <string>
#include "shared/common/status_code.h"
namespace edge_controller {
// 固定升级引擎的受限接口，独立校验标识、限制输出并执行超时。
class UpdateService {
public:
    StatusCode get_update_current_version(std::string* result_json, std::string* error_message) const;
    StatusCode get_update_status(std::string* result_json, std::string* error_message) const;
    StatusCode validate_update_package(const std::string& package_identifier, std::string* result_json, std::string* error_message) const;
    StatusCode start_update_job(const std::string& job_id, std::string* result_json, std::string* error_message) const;
    StatusCode import_application_upgrade_package(const std::string& upload_id, const std::string& package_identifier, std::string* result_json, std::string* error_message) const;
};
}  // namespace edge_controller
