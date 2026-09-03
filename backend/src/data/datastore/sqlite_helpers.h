// SQLite 存储实现共享的最小语句生命周期与类型转换辅助。
// 本层不拥有连接或事务，调用方仍负责锁、BEGIN/COMMIT/ROLLBACK 和业务错误上下文。
#pragma once

#include <string>
#include <type_traits>

#include "shared/common/sqlite_compat.h"

namespace edge_controller {
namespace sqlite_helpers {

inline std::string sqlite_error(sqlite3* database)
{
    if (database == nullptr) {
        return "SQLite 数据库未打开";
    }
    const auto* message = sqlite3_errmsg(database);
    return message == nullptr ? "未知 SQLite 错误" : message;
}

// schema 不兼容时必须把现场数据保全约束告知运维，避免各 Store 给出删库建议。
inline std::string schema_migration_required_message(const std::string& detail)
{
    return detail +
           "；请停止服务、完整备份现场数据库，并使用提供对应 schema 迁移路径的发布版本；"
           "不得删除现场数据库";
}

class Statement {
public:
    Statement(sqlite3* database, const char* sql)
    {
        status_ = sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    ~Statement()
    {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    bool ok() const { return status_ == SQLITE_OK && statement_ != nullptr; }
    int status() const { return status_; }
    sqlite3_stmt* get() const { return statement_; }

private:
    sqlite3_stmt* statement_{nullptr};
    int status_{SQLITE_MISUSE};
};

inline bool bind_text(sqlite3_stmt* statement, int index, const std::string& value)
{
    return sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

inline bool bind_int(sqlite3_stmt* statement, int index, int value)
{
    return sqlite3_bind_int(statement, index, value) == SQLITE_OK;
}

template <typename Integer, std::enable_if_t<std::is_integral_v<Integer>, int> = 0>
inline bool bind_int64(sqlite3_stmt* statement, int index, Integer value)
{
    return sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

inline bool bind_double(sqlite3_stmt* statement, int index, double value)
{
    return sqlite3_bind_double(statement, index, value) == SQLITE_OK;
}

inline std::string column_text(sqlite3_stmt* statement, int column)
{
    const auto* text = sqlite3_column_text(statement, column);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

inline sqlite3_int64 column_int64(sqlite3_stmt* statement, int column)
{
    return sqlite3_column_int64(statement, column);
}

}  // namespace sqlite_helpers
}  // namespace edge_controller
