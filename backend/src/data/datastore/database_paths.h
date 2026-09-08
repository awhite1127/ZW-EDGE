// 集中计算配置、历史和事件数据库的规范路径。
// 边界：一致性由类内锁或 SQLite 事务保证，错误通过 StatusCode 返回。

#pragma once

#include <string>
#include <system_error>

#include "shared/common/filesystem_compat.h"
#include "shared/common/status_code.h"

namespace edge_controller {

// DatabasePaths 是集中数据目录下数据库文件名的唯一来源。
// 三个数据库互不 ATTACH，只通过业务逻辑 ID 关联。
class DatabasePaths {
public:

    explicit DatabasePaths(const std::string& data_directory)
        : data_directory_(data_directory.empty() ? default_data_directory() : data_directory)
    {
    }

    // 获取默认数据目录。
    static std::string default_data_directory()
    {
        return "/opt/edge-controller/data";
    }

    // 获取数据目录。
    const std::string& data_directory() const { return data_directory_; }

    // 返回配置数据库路径。
    std::string config_database() const
    {
        return (edge::fs::path(data_directory_) / "edge-config.db").string();
    }

    // 返回历史数据库路径。
    std::string history_database() const
    {
        return (edge::fs::path(data_directory_) / "edge-history.db").string();
    }

    // 返回事件数据库路径。
    std::string events_database() const
    {
        return (edge::fs::path(data_directory_) / "edge-events.db").string();
    }

    // 返回旧版单数据库文件路径。
    std::string old_single_database() const
    {
        return (edge::fs::path(data_directory_) / "edge-controller.db").string();
    }

    // 检测并拒绝不受支持的旧版单数据库布局。
    StatusCode reject_old_single_database(std::string* error_message = nullptr) const
    {
        std::error_code ec;
        const auto old_database_path = edge::fs::path(old_single_database());
        const bool exists = edge::fs::exists(old_database_path, ec);
        if (ec) {
            if (error_message != nullptr) {
                *error_message = "检查旧版单库失败：" + old_database_path.string() + "，原因=" + ec.message();
            }
            return StatusCode::kIoError;
        }
        if (!exists) {
            return StatusCode::kOk;
        }
        if (error_message != nullptr) {
            *error_message = "检测到不受支持的旧版单库 " + old_database_path.string() +
                             "；当前版本不执行该布局迁移；请停止服务、完整备份现场数据库，"
                             "并使用提供对应 schema 迁移路径的发布版本；不得删除现场数据库";
        }
        return StatusCode::kInvalidState;
    }

private:
    std::string data_directory_;
};

}  // namespace edge_controller
