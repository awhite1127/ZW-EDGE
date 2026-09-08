#include "application/service/backend_service.h"
namespace edge_controller {
StatusCode BackendService::get_update_current_version(std::string* result_json, std::string* error_message) const
{
    return update_service_.get_update_current_version(result_json, error_message);
}
StatusCode BackendService::get_update_status(std::string* result_json, std::string* error_message) const
{
    return update_service_.get_update_status(result_json, error_message);
}
StatusCode BackendService::validate_update_package(const std::string& package_identifier, std::string* result_json, std::string* error_message) const
{
    return update_service_.validate_update_package(package_identifier, result_json, error_message);
}
StatusCode BackendService::start_update_job(const std::string& job_id, std::string* result_json, std::string* error_message) const
{
    return update_service_.start_update_job(job_id, result_json, error_message);
}
StatusCode BackendService::import_application_upgrade_package(const std::string& upload_id, const std::string& package_identifier, std::string* result_json, std::string* error_message) const
{
    return update_service_.import_application_upgrade_package(upload_id, package_identifier, result_json, error_message);
}
}  // namespace edge_controller
