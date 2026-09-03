// 组装模板定义、引用主站和只读能力视图。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#include "application/service/backend_service.h"
#include <algorithm>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "shared/common/time_utils.h"
#include "application/service/backend_service_internal.h"

namespace edge_controller {

namespace {

// 生成设备类型当前不可删除的引用原因。
std::string template_reference_reason(const DeviceTemplateManagementItem& item)
{
    if (!item.referenced) {
        return {};
    }
    return "该模板已被主控引用，不能编辑或删除。请先解除主控绑定后再操作。";
}

// 查找模板内列表。
const DeviceTemplateDefinition* find_template_in_list(
    const std::vector<DeviceTemplateDefinition>& templates,
    const std::string& template_id)
{
    for (const auto& device_template : templates) {
        if (device_template.template_id == template_id) {
            return &device_template;
        }
    }
    return nullptr;
}

// 判断设备类型是否仍被任一主站引用。
bool is_template_referenced_by_master(
    const std::vector<MasterNodeConfig>& masters,
    const std::string& template_id)
{
    return std::any_of(
        masters.begin(),
        masters.end(),
        [&](const MasterNodeConfig& master) {
            return master.device_template == template_id;
        });
}

}  // namespace

// 获取设备模板管理视图。
DeviceTemplateManagementView BackendService::get_device_template_management() const
{
    std::vector<MasterNodeConfig> masters;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        masters = system_config_.master_nodes;
    }

    DeviceTemplateManagementView view;
    auto templates = device_templates();
    view.total_templates = templates.size();
    view.templates.reserve(templates.size());

    for (const auto& device_template : templates) {
        // 管理视图把模板定义和引用关系合并，前端据此决定编辑/删除按钮是否可用。
        DeviceTemplateManagementItem item;
        item.template_id = device_template.template_id;
        item.template_name = device_template.display_name;
        item.description = device_template.description;
        item.default_start_register = device_template.default_start_register;
        item.device_address_stride = device_template.device_address_stride;
        item.read_blocks = device_template.read_blocks;
        item.field_count = device_template.fields.size();
        item.builtin = device_template.builtin;
        item.fields = device_template.fields;
        item.write_commands = device_template.write_commands;
        item.realtime_grouping_enabled = device_template.realtime_grouping_enabled;
        item.realtime_groups = device_template.realtime_groups;

        for (const auto& master : masters) {
            if (master.device_template != device_template.template_id) {
                continue;
            }
            item.referenced_masters.push_back({
                master.master_id,
                master.master_name,
            });
        }

        item.reference_count = item.referenced_masters.size();
        item.referenced = item.reference_count > 0;
        if (item.builtin) {
            item.readonly_reason = "内置模板不能编辑或删除";
        } else if (item.referenced) {
            item.readonly_reason = template_reference_reason(item);
        }
        item.editable = !item.builtin && !item.referenced;
        item.deletable = !item.builtin && !item.referenced;
        if (item.referenced) {
            ++view.referenced_templates;
        }
        view.templates.push_back(std::move(item));
    }

    view.unreferenced_templates = view.total_templates - view.referenced_templates;
    return view;
}

// 新增设备模板并返回最新管理视图。
StatusCode BackendService::create_device_template(
    const DeviceTemplateDefinition& device_template,
    DeviceTemplateManagementView* result,
    std::string* error_message)
{
    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        if (result == nullptr) {
            if (error_message != nullptr) {
                *error_message = "设备模板创建结果参数为空";
            }
            return StatusCode::kInvalidArgument;
        }
        if (!initialized_) {
            if (error_message != nullptr) {
                *error_message = "后端服务尚未初始化，无法新增设备模板";
            }
            return StatusCode::kInvalidState;
        }
        if (config_apply_in_progress_.load()) {
            if (error_message != nullptr) {
                *error_message = "配置正在应用中，请稍后新增设备模板";
            }
            return StatusCode::kInvalidState;
        }

        backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);
        std::string create_error;
        lock.unlock();
        const auto create_status = device_template_store_.create_template(device_template, &create_error);
        lock.lock();

        if (!is_ok(create_status)) {
            const auto message =
                "设备模板保存失败：" + (create_error.empty() ? std::string("未知错误") : create_error);
            if (error_message != nullptr) {
                *error_message = message;
            }
            return create_status;
        }
        // 新模板尚未被任何主站引用，不会改变当前采集计划；无需停止或重建轮询。
        append_event("info", "config_apply", "", "设备模板已保存，当前轮询不受影响", "", time_utils::system_now_ms());
    }

    *result = get_device_template_management();
    return StatusCode::kOk;
}

// 更新设备模板并返回最新管理视图。
StatusCode BackendService::update_device_template(
    const DeviceTemplateDefinition& device_template,
    DeviceTemplateManagementView* result,
    std::string* error_message)
{
    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        if (result == nullptr) {
            if (error_message != nullptr) {
                *error_message = "设备模板更新结果参数为空";
            }
            return StatusCode::kInvalidArgument;
        }
        if (!initialized_) {
            if (error_message != nullptr) {
                *error_message = "后端服务尚未初始化，无法编辑设备模板";
            }
            return StatusCode::kInvalidState;
        }
        if (config_apply_in_progress_.load()) {
            if (error_message != nullptr) {
                *error_message = "配置正在应用中，请稍后编辑设备模板";
            }
            return StatusCode::kInvalidState;
        }

        const auto templates = device_templates();
        const auto* current_template = find_template_in_list(templates, device_template.template_id);
        if (current_template == nullptr) {
            if (error_message != nullptr) {
                *error_message = "设备模板不存在：" + device_template.template_id;
            }
            return StatusCode::kNotFound;
        }
        if (current_template->builtin) {
            if (error_message != nullptr) {
                *error_message = "内置模板不能编辑";
            }
            return StatusCode::kInvalidArgument;
        }
        if (is_template_referenced_by_master(system_config_.master_nodes, device_template.template_id)) {
            if (error_message != nullptr) {
                *error_message = "该模板已被主控引用，不能编辑。请先解除主控绑定后再操作。";
            }
            return StatusCode::kInvalidArgument;
        }

        backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);
        std::string update_error;
        lock.unlock();
        const auto update_status = device_template_store_.update_template(device_template, &update_error);
        lock.lock();

        if (!is_ok(update_status)) {
            const auto message =
                "设备模板保存失败：" + (update_error.empty() ? std::string("未知错误") : update_error);
            if (error_message != nullptr) {
                *error_message = message;
            }
            return update_status;
        }
        // 已被引用的模板在上方已拒绝；更新未引用模板不需要扰动采集轮询。
        append_event("info", "config_apply", "", "设备模板已保存，当前轮询不受影响", "", time_utils::system_now_ms());
    }

    *result = get_device_template_management();
    return StatusCode::kOk;
}

// 更新设备类型字段的实时显示设置。
StatusCode BackendService::update_device_template_realtime_display(
    const std::string& template_id,
    const std::string& field_key,
    bool show_in_realtime,
    DeviceTemplateManagementView* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) *error_message = "设备模板展示配置结果参数为空";
        return StatusCode::kInvalidArgument;
    }
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            if (error_message != nullptr) *error_message = "后端服务尚未初始化，无法保存展示配置";
            return StatusCode::kInvalidState;
        }
    }
    const auto status = device_template_store_.update_realtime_display_preference(
        template_id, field_key, show_in_realtime, error_message);
    if (!is_ok(status)) return status;
    *result = get_device_template_management();
    return StatusCode::kOk;
}

// 更新设备类型历史数据启用状态。
StatusCode BackendService::update_device_template_history_enabled(
    const std::string& template_id,
    const std::string& field_key,
    bool history_enabled,
    DeviceTemplateManagementView* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) *error_message = "设备模板历史配置结果参数为空";
        return StatusCode::kInvalidArgument;
    }
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            if (error_message != nullptr) *error_message = "后端服务尚未初始化，无法保存历史记录配置";
            return StatusCode::kInvalidState;
        }
    }
    const auto status = device_template_store_.update_history_enabled_preference(
        template_id, field_key, history_enabled, error_message);
    if (!is_ok(status)) return status;
    *result = get_device_template_management();
    return StatusCode::kOk;
}

// 删除设备模板并返回最新管理视图。
StatusCode BackendService::delete_device_template(
    const std::string& template_id,
    DeviceTemplateManagementView* result,
    std::string* error_message)
{
    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        if (result == nullptr) {
            if (error_message != nullptr) {
                *error_message = "设备模板删除结果参数为空";
            }
            return StatusCode::kInvalidArgument;
        }
        if (!initialized_) {
            if (error_message != nullptr) {
                *error_message = "后端服务尚未初始化，无法删除设备模板";
            }
            return StatusCode::kInvalidState;
        }
        if (config_apply_in_progress_.load()) {
            if (error_message != nullptr) {
                *error_message = "配置正在应用中，请稍后删除设备模板";
            }
            return StatusCode::kInvalidState;
        }

        const auto templates = device_templates();
        const auto* current_template = find_template_in_list(templates, template_id);
        if (current_template == nullptr) {
            if (error_message != nullptr) {
                *error_message = "设备模板不存在：" + template_id;
            }
            return StatusCode::kNotFound;
        }
        if (current_template->builtin) {
            if (error_message != nullptr) {
                *error_message = "内置模板不能删除";
            }
            return StatusCode::kInvalidArgument;
        }
        if (is_template_referenced_by_master(system_config_.master_nodes, template_id)) {
            if (error_message != nullptr) {
                *error_message = "该模板已被主控引用，不能删除。请先解除主控绑定后再操作。";
            }
            return StatusCode::kInvalidArgument;
        }

        backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);
        std::string delete_error;
        lock.unlock();
        const auto delete_status = device_template_store_.delete_template(template_id, &delete_error);
        lock.lock();

        if (!is_ok(delete_status)) {
            const auto message =
                "设备模板删除失败：" + (delete_error.empty() ? std::string("未知错误") : delete_error);
            if (error_message != nullptr) {
                *error_message = message;
            }
            return delete_status;
        }
        // 已被引用的模板在上方已拒绝；删除未引用模板不需要扰动采集轮询。
        append_event("info", "config_apply", "", "设备模板已删除，当前轮询不受影响", "", time_utils::system_now_ms());
    }

    *result = get_device_template_management();
    return StatusCode::kOk;
}

}  // namespace edge_controller
