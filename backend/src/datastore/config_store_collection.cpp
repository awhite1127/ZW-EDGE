// 采集配置持久化：原子读写通道、主站和相关引用，不负责打开通信链路。
#include "datastore/config_store.h"
#include "datastore/config_store_internal.h"
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#include "common/sqlite_compat.h"
#include "common/time_utils.h"

namespace edge_controller {

using namespace config_store_internal;

// 读取通道配置列表。
StatusCode ConfigStore::load_channels(
    std::vector<ChannelConfig>* channels,
    std::string* error_message) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return load_channels_locked(channels, error_message);
}

// 保存通道配置列表。
StatusCode ConfigStore::save_channels(
    const std::vector<ChannelConfig>& channels,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return save_channels_locked(channels, error_message);
}

// 读取主站配置列表。
StatusCode ConfigStore::load_masters(
    std::vector<MasterNodeConfig>* masters,
    std::string* error_message) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return load_masters_locked(masters, error_message);
}

// 保存主站配置列表。
StatusCode ConfigStore::save_masters(
    const std::vector<MasterNodeConfig>& masters,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return save_masters_locked(masters, error_message);
}

// 在持锁状态下读取全部通道配置。
StatusCode ConfigStore::load_channels_locked(
    std::vector<ChannelConfig>* channels,
    std::string* error_message) const
{
    if (channels == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少通道配置输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    const char* sql =
        "SELECT id, name, type, enabled, device_path, port_name, tcp_host, tcp_port, "
        "connect_timeout_ms, baud_rate, data_bits, stop_bits, parity, timeout_ms, retry_count "
        "FROM channels ORDER BY display_order ASC, id ASC;";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备读取 channels 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    std::vector<ChannelConfig> loaded;
    while (true) {
        const auto step_status = sqlite3_step(statement.get());
        if (step_status == SQLITE_DONE) {
            break;
        }
        if (step_status != SQLITE_ROW) {
            if (error_message != nullptr) {
                *error_message = "读取 channels 失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }

        ChannelConfig channel;
        channel.channel_id = column_text(statement.get(), 0);
        channel.channel_name = column_text(statement.get(), 1);
        if (!parse_channel_type(column_text(statement.get(), 2), &channel.channel_type)) {
            if (error_message != nullptr) {
                *error_message = "通道 " + channel.channel_id + " 的 type 非法";
            }
            return StatusCode::kInvalidArgument;
        }
        channel.enabled = sqlite3_column_int(statement.get(), 3) != 0;
        channel.device_path = column_text(statement.get(), 4);
        channel.port_name = column_text(statement.get(), 5);
        channel.tcp_host = column_text(statement.get(), 6);
        const auto loaded_tcp_port = sqlite3_column_int(statement.get(), 7);
        channel.tcp_port = loaded_tcp_port <= 0 ? 502 : static_cast<std::uint16_t>(loaded_tcp_port);
        const auto loaded_connect_timeout_ms = sqlite3_column_int(statement.get(), 8);
        channel.connect_timeout_ms =
            loaded_connect_timeout_ms <= 0 ? 3000U : static_cast<std::uint32_t>(loaded_connect_timeout_ms);
        if (channel.port_name.empty()) {
            channel.port_name = channel.device_path;
        }
        channel.baud_rate = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 9));
        channel.data_bits = static_cast<std::uint8_t>(sqlite3_column_int(statement.get(), 10));
        channel.stop_bits = static_cast<std::uint8_t>(sqlite3_column_int(statement.get(), 11));
        if (!parse_serial_parity(column_text(statement.get(), 12), &channel.parity)) {
            if (error_message != nullptr) {
                *error_message = "通道 " + channel.channel_id + " 的 parity 非法";
            }
            return StatusCode::kInvalidArgument;
        }
        channel.response_timeout_ms = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 13));
        channel.retry_count = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 14));

        const auto validation_status = validate_channel_config(channel, error_message);
        if (!is_ok(validation_status)) {
            return validation_status;
        }
        loaded.push_back(std::move(channel));
        // 增量检查可避免被手工篡改的数据库在启动时加载无界通道列表。
        if (!validate_channel_count_limits(loaded, error_message)) {
            return StatusCode::kInvalidArgument;
        }
    }

    *channels = std::move(loaded);
    return StatusCode::kOk;
}

// 在持锁状态下保存全部通道配置。
StatusCode ConfigStore::save_channels_locked(
    const std::vector<ChannelConfig>& channels,
    std::string* error_message)
{
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }
    // 必须在开启事务和清空旧表之前拒绝超限配置，确保已有合法配置不受影响。
    if (!validate_channel_count_limits(channels, error_message)) {
        return StatusCode::kInvalidArgument;
    }
    for (const auto& channel : channels) {
        const auto validation_status = validate_channel_config(channel, error_message);
        if (!is_ok(validation_status)) {
            return validation_status;
        }
    }

    const bool owns_transaction = sqlite3_get_autocommit(database_) != 0;
    if (owns_transaction) {
        const auto begin_status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
        if (!is_ok(begin_status)) {
            return begin_status;
        }
    }

    auto rollback = [&]() {
        if (owns_transaction) {
            std::string ignored;
            execute_sql_locked("ROLLBACK;", &ignored);
        }
    };

    std::string delete_error;
    const auto delete_status = execute_sql_locked("DELETE FROM channels;", &delete_error);
    if (!is_ok(delete_status)) {
        rollback();
        if (error_message != nullptr) {
            *error_message = "清空 channels 失败：" + delete_error;
        }
        return delete_status;
    }

    const char* sql =
        "INSERT INTO channels "
        "(id, name, type, enabled, device_path, port_name, tcp_host, tcp_port, connect_timeout_ms, "
        "baud_rate, data_bits, stop_bits, parity, timeout_ms, retry_count, display_order, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        rollback();
        if (error_message != nullptr) {
            *error_message = "准备写入 channels 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto now = time_utils::local_time_string();
    for (std::size_t index = 0; index < channels.size(); ++index) {
        const auto& channel = channels[index];
        if (!bind_text(statement.get(), 1, channel.channel_id) ||
            !bind_text(statement.get(), 2, channel.channel_name) ||
            !bind_text(statement.get(), 3, to_string(channel.channel_type)) ||
            !bind_int(statement.get(), 4, channel.enabled ? 1 : 0) ||
            !bind_text(statement.get(), 5, channel.device_path) ||
            !bind_text(statement.get(), 6, channel.port_name.empty() ? channel.device_path : channel.port_name) ||
            !bind_text(statement.get(), 7, channel.tcp_host) ||
            !bind_int(statement.get(), 8, static_cast<int>(channel.tcp_port)) ||
            !bind_int(statement.get(), 9, static_cast<int>(channel.connect_timeout_ms)) ||
            !bind_int(statement.get(), 10, static_cast<int>(channel.baud_rate)) ||
            !bind_int(statement.get(), 11, static_cast<int>(channel.data_bits)) ||
            !bind_int(statement.get(), 12, static_cast<int>(channel.stop_bits)) ||
            !bind_text(statement.get(), 13, to_string(channel.parity)) ||
            !bind_int(statement.get(), 14, static_cast<int>(channel.response_timeout_ms)) ||
            !bind_int(statement.get(), 15, static_cast<int>(channel.retry_count)) ||
            !bind_int(statement.get(), 16, static_cast<int>(index)) ||
            !bind_text(statement.get(), 17, now) ||
            !bind_text(statement.get(), 18, now)) {
            rollback();
            if (error_message != nullptr) {
                *error_message = "绑定 channels 参数失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            rollback();
            if (error_message != nullptr) {
                *error_message = "写入 channels 失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        sqlite3_reset(statement.get());
        sqlite3_clear_bindings(statement.get());
    }

    if (owns_transaction) {
        const auto commit_status = execute_sql_locked("COMMIT;", error_message);
        if (!is_ok(commit_status)) {
            rollback();
            return commit_status;
        }
    }
    return StatusCode::kOk;
}

// 在持锁状态下读取全部主站配置。
StatusCode ConfigStore::load_masters_locked(
    std::vector<MasterNodeConfig>* masters,
    std::string* error_message) const
{
    if (masters == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少主控配置输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    const char* sql =
        "SELECT id, channel_id, name, enabled, protocol, slave_id, template_id, "
        "start_register, device_count, poll_interval_ms, timeout_ms, retry_count, remark "
        "FROM masters ORDER BY display_order ASC, id ASC;";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备读取 masters 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    std::vector<MasterNodeConfig> loaded;
    while (true) {
        const auto step_status = sqlite3_step(statement.get());
        if (step_status == SQLITE_DONE) {
            break;
        }
        if (step_status != SQLITE_ROW) {
            if (error_message != nullptr) {
                *error_message = "读取 masters 失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }

        MasterNodeConfig master;
        master.master_id = column_text(statement.get(), 0);
        master.channel_id = column_text(statement.get(), 1);
        master.master_name = column_text(statement.get(), 2);
        master.enabled = sqlite3_column_int(statement.get(), 3) != 0;
        if (!parse_master_protocol(column_text(statement.get(), 4), &master.protocol)) {
            if (error_message != nullptr) {
                *error_message = "主控 " + master.master_id + " 的 protocol 非法";
            }
            return StatusCode::kInvalidArgument;
        }
        master.target_address = static_cast<std::uint16_t>(sqlite3_column_int(statement.get(), 5));
        master.device_template = column_text(statement.get(), 6);
        master.block_start_register = static_cast<RegisterAddress>(sqlite3_column_int(statement.get(), 7));
        const auto persisted_device_count = sqlite3_column_int64(statement.get(), 8);
        if (persisted_device_count <= 0 ||
            persisted_device_count > static_cast<sqlite3_int64>(kMaxDevicesPerMaster)) {
            if (error_message != nullptr) {
                *error_message =
                    "主控 " + master.master_id + " 的设备数量必须在 1-" +
                    std::to_string(kMaxDevicesPerMaster) + " 范围内";
            }
            return StatusCode::kInvalidState;
        }
        master.device_count = static_cast<std::uint32_t>(persisted_device_count);
        master.poll_interval_ms = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 9));
        master.response_timeout_ms = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 10));
        master.retry_count = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 11));
        master.remark = column_text(statement.get(), 12);

        const auto validation_status = validate_master_config(master, error_message);
        if (!is_ok(validation_status)) {
            return validation_status;
        }
        loaded.push_back(std::move(master));
    }

    *masters = std::move(loaded);
    return StatusCode::kOk;
}

// 在持锁状态下保存全部主站配置。
StatusCode ConfigStore::save_masters_locked(
    const std::vector<MasterNodeConfig>& masters,
    std::string* error_message)
{
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }
    for (const auto& master : masters) {
        const auto validation_status = validate_master_config(master, error_message);
        if (!is_ok(validation_status)) {
            return validation_status;
        }
    }

    const bool owns_transaction = sqlite3_get_autocommit(database_) != 0;
    if (owns_transaction) {
        const auto begin_status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
        if (!is_ok(begin_status)) {
            return begin_status;
        }
    }

    auto rollback = [&]() {
        if (owns_transaction) {
            std::string ignored;
            execute_sql_locked("ROLLBACK;", &ignored);
        }
    };

    std::string delete_error;
    const auto delete_status = execute_sql_locked("DELETE FROM masters;", &delete_error);
    if (!is_ok(delete_status)) {
        rollback();
        if (error_message != nullptr) {
            *error_message = "清空 masters 失败：" + delete_error;
        }
        return delete_status;
    }

    const char* sql =
        "INSERT INTO masters "
        "(id, channel_id, name, enabled, protocol, slave_id, template_id, start_register, "
        "device_count, poll_interval_ms, timeout_ms, retry_count, remark, display_order, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        rollback();
        if (error_message != nullptr) {
            *error_message = "准备写入 masters 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto now = time_utils::local_time_string();
    for (std::size_t index = 0; index < masters.size(); ++index) {
        const auto& master = masters[index];
        if (!bind_text(statement.get(), 1, master.master_id) ||
            !bind_text(statement.get(), 2, master.channel_id) ||
            !bind_text(statement.get(), 3, master.master_name) ||
            !bind_int(statement.get(), 4, master.enabled ? 1 : 0) ||
            !bind_text(statement.get(), 5, to_string(master.protocol)) ||
            !bind_int(statement.get(), 6, static_cast<int>(master.target_address)) ||
            !bind_text(statement.get(), 7, master.device_template) ||
            !bind_int(statement.get(), 8, static_cast<int>(master.block_start_register)) ||
            !bind_int64(statement.get(), 9, static_cast<sqlite3_int64>(master.device_count)) ||
            !bind_int(statement.get(), 10, static_cast<int>(master.poll_interval_ms)) ||
            !bind_int(statement.get(), 11, static_cast<int>(master.response_timeout_ms)) ||
            !bind_int(statement.get(), 12, static_cast<int>(master.retry_count)) ||
            !bind_text(statement.get(), 13, master.remark) ||
            !bind_int(statement.get(), 14, static_cast<int>(index)) ||
            !bind_text(statement.get(), 15, now) ||
            !bind_text(statement.get(), 16, now)) {
            rollback();
            if (error_message != nullptr) {
                *error_message = "绑定 masters 参数失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            rollback();
            if (error_message != nullptr) {
                *error_message = "写入 masters 失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        sqlite3_reset(statement.get());
        sqlite3_clear_bindings(statement.get());
    }

    if (owns_transaction) {
        const auto commit_status = execute_sql_locked("COMMIT;", error_message);
        if (!is_ok(commit_status)) {
            rollback();
            return commit_status;
        }
    }
    return StatusCode::kOk;
}

// 处理设备别名。
StatusCode ConfigStore::load_device_aliases(
    std::vector<DeviceAlias>* aliases,
    std::string* error_message) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return load_device_aliases_locked(aliases, error_message);
}

// 在持锁状态下加载设备显示名称别名。
StatusCode ConfigStore::load_device_aliases_locked(
    std::vector<DeviceAlias>* aliases,
    std::string* error_message) const
{
    if (aliases == nullptr) {
        if (error_message != nullptr) *error_message = "设备别名输出参数为空";
        return StatusCode::kInvalidArgument;
    }
    aliases->clear();
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;

    Statement statement(
        database_,
        "SELECT device_id, display_name, updated_at_ms FROM device_aliases ORDER BY device_id;");
    if (!statement.ok()) {
        if (error_message != nullptr) *error_message = "准备读取设备别名失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        DeviceAlias alias;
        alias.device_id = column_text(statement.get(), 0);
        alias.display_name = column_text(statement.get(), 1);
        alias.updated_at_ms = static_cast<TimestampMs>(column_int64(statement.get(), 2));
        aliases->push_back(std::move(alias));
    }
    if (step != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "读取设备别名失败：" + sqlite_error(database_);
        aliases->clear();
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 保存设备别名。
StatusCode ConfigStore::save_device_alias(
    const DeviceId& device_id,
    const std::string& display_name,
    TimestampMs updated_at_ms,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return save_device_alias_locked(device_id, display_name, updated_at_ms, error_message);
}

// 保存设备批量。
StatusCode ConfigStore::save_device_aliases_batch(
    const std::vector<DeviceAlias>& aliases,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;
    if (aliases.empty()) return StatusCode::kOk;

    auto status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(status)) return status;
    for (const auto& alias : aliases) {
        status = save_device_alias_locked(
            alias.device_id, alias.display_name, alias.updated_at_ms, error_message);
        if (!is_ok(status)) break;
    }
    if (is_ok(status)) status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(status)) execute_sql_locked("ROLLBACK;", nullptr);
    return status;
}

// 在持锁状态下保存设备显示名称别名。
StatusCode ConfigStore::save_device_alias_locked(
    const DeviceId& device_id,
    const std::string& display_name,
    TimestampMs updated_at_ms,
    std::string* error_message)
{
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;
    const char* sql = display_name.empty()
        ? "DELETE FROM device_aliases WHERE device_id = ?;"
        : "INSERT INTO device_aliases(device_id,display_name,updated_at_ms) VALUES(?,?,?) "
          "ON CONFLICT(device_id) DO UPDATE SET display_name=excluded.display_name,updated_at_ms=excluded.updated_at_ms;";
    Statement statement(database_, sql);
    if (!statement.ok() || !bind_text(statement.get(), 1, device_id) ||
        (!display_name.empty() &&
         (!bind_text(statement.get(), 2, display_name) ||
          !bind_int64(statement.get(), 3, static_cast<sqlite3_int64>(updated_at_ms))))) {
        if (error_message != nullptr) *error_message = "准备保存设备名称失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "保存设备名称失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

}  // namespace edge_controller
