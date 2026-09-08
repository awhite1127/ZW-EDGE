// 持久化告警规则和告警运行状态，并维护设备加点位唯一键。
// 边界：一致性由类内锁或 SQLite 事务保证，错误通过 StatusCode 返回。

#include "data/datastore/alarm_store.h"

#include <cmath>
#include "data/model/service_event_codec.h"

#include "shared/common/sqlite_compat.h"
#include "data/datastore/sqlite_helpers.h"
#include "data/model/alarm_validation.h"

namespace edge_controller {
namespace {

using sqlite_helpers::Statement;
using sqlite_helpers::bind_text;
using sqlite_helpers::schema_migration_required_message;
inline constexpr auto db_error = sqlite_helpers::sqlite_error;
inline constexpr auto text_column = sqlite_helpers::column_text;

StatusCode set_error(std::string* output, const std::string& message, StatusCode code = StatusCode::kIoError)
{
    if (output != nullptr) *output = message;
    return code;
}

// 校验告警运行状态能否安全持久化。
bool valid_runtime_state(const AlarmRuntimeState& state, std::string* error)
{
    if (state.device_id.empty() || state.point_key.empty()) { set_error(error, "告警运行状态的 device_id 和 point_key 不能为空", StatusCode::kInvalidArgument); return false; }
    if (!std::isfinite(state.current_value) || !std::isfinite(state.threshold_value)) { set_error(error, "告警运行状态数值必须为有限数", StatusCode::kInvalidArgument); return false; }
    if (state.last_evaluated_at_ms > state.updated_at_ms) { set_error(error, "告警运行状态的评估时间不能晚于更新时间", StatusCode::kInvalidArgument); return false; }
    if ((state.consecutive_trigger_count > 0 || state.consecutive_recovery_count > 0) && state.last_evaluated_at_ms == 0) { set_error(error, "存在连续计数时必须记录评估时间", StatusCode::kInvalidArgument); return false; }
    if (state.acknowledged) {
        if (state.state != "active" || state.acknowledged_at_ms == 0 || state.acknowledged_by.empty()) { set_error(error, "只有活动告警可以记录有效的确认信息", StatusCode::kInvalidArgument); return false; }
    } else if (state.acknowledged_at_ms != 0 || !state.acknowledged_by.empty()) {
        set_error(error, "未确认告警不能包含确认时间或确认人", StatusCode::kInvalidArgument); return false;
    }
    if (state.state == "normal") {
        if (state.direction != "none" || state.active_since_ms != 0 || state.consecutive_trigger_count != 0 || state.consecutive_recovery_count != 0) { set_error(error, "normal 状态必须使用 none 方向并清零激活时间和连续计数", StatusCode::kInvalidArgument); return false; }
        return true;
    }
    if (state.direction != "high" && state.direction != "low") { set_error(error, "pending 或 active 状态必须使用 high 或 low 方向", StatusCode::kInvalidArgument); return false; }
    if (state.last_evaluated_at_ms == 0 || state.updated_at_ms == 0) { set_error(error, "pending 或 active 状态必须记录评估和更新时间", StatusCode::kInvalidArgument); return false; }
    if (state.state == "pending") {
        if (state.active_since_ms != 0 || state.consecutive_trigger_count == 0 || state.consecutive_recovery_count != 0) { set_error(error, "pending 状态必须尚未激活、具有触发计数且没有恢复计数", StatusCode::kInvalidArgument); return false; }
        return true;
    }
    if (state.state == "active") {
        if (state.active_since_ms == 0 || state.active_since_ms > state.last_evaluated_at_ms) { set_error(error, "active 状态必须具有不晚于评估时间的激活时间", StatusCode::kInvalidArgument); return false; }
        if (state.consecutive_trigger_count > 0 && state.consecutive_recovery_count > 0) { set_error(error, "active 状态的触发计数和恢复计数不能同时非零", StatusCode::kInvalidArgument); return false; }
        return true;
    }
    return set_error(error, "告警运行状态只能是 normal、pending 或 active", StatusCode::kInvalidArgument), false;
}

// 按 alarm_runtime_states 写入语句的稳定列顺序绑定一条运行态。
bool bind_runtime_state(sqlite3_stmt* statement, const AlarmRuntimeState& state)
{
    return bind_text(statement, 1, state.device_id) &&
           bind_text(statement, 2, state.point_key) &&
           bind_text(statement, 3, state.state) &&
           bind_text(statement, 4, state.direction) &&
           sqlite3_bind_double(statement, 5, state.current_value) == SQLITE_OK &&
           sqlite3_bind_double(statement, 6, state.threshold_value) == SQLITE_OK &&
           sqlite3_bind_int64(statement, 7, state.consecutive_trigger_count) == SQLITE_OK &&
           sqlite3_bind_int64(statement, 8, state.consecutive_recovery_count) == SQLITE_OK &&
           sqlite3_bind_int64(statement, 9, static_cast<sqlite3_int64>(state.active_since_ms)) == SQLITE_OK &&
           sqlite3_bind_int64(statement, 10, static_cast<sqlite3_int64>(state.last_evaluated_at_ms)) == SQLITE_OK &&
           sqlite3_bind_int(statement, 11, state.acknowledged ? 1 : 0) == SQLITE_OK &&
           sqlite3_bind_int64(statement, 12, static_cast<sqlite3_int64>(state.acknowledged_at_ms)) == SQLITE_OK &&
           bind_text(statement, 13, state.acknowledged_by) &&
           sqlite3_bind_int64(statement, 14, static_cast<sqlite3_int64>(state.updated_at_ms)) == SQLITE_OK;
}

// 读取规则。
AlarmRule read_rule(sqlite3_stmt* s)
{
    AlarmRule r; r.device_id=text_column(s,0); r.point_key=text_column(s,1); r.enabled=sqlite3_column_int(s,2)!=0;
    r.high_enabled=sqlite3_column_int(s,3)!=0; r.high_threshold=sqlite3_column_double(s,4); r.low_enabled=sqlite3_column_int(s,5)!=0;
    r.low_threshold=sqlite3_column_double(s,6); r.level=text_column(s,7); r.hysteresis=sqlite3_column_double(s,8);
    r.trigger_count=static_cast<std::uint32_t>(sqlite3_column_int64(s,9)); r.recovery_count=static_cast<std::uint32_t>(sqlite3_column_int64(s,10));
    r.updated_at_ms=static_cast<TimestampMs>(sqlite3_column_int64(s,11)); return r;
}

// 读取状态。
AlarmRuntimeState read_state(sqlite3_stmt* s)
{
    AlarmRuntimeState v; v.device_id=text_column(s,0); v.point_key=text_column(s,1); v.state=text_column(s,2); v.direction=text_column(s,3);
    v.current_value=sqlite3_column_double(s,4); v.threshold_value=sqlite3_column_double(s,5);
    v.consecutive_trigger_count=static_cast<std::uint32_t>(sqlite3_column_int64(s,6)); v.consecutive_recovery_count=static_cast<std::uint32_t>(sqlite3_column_int64(s,7));
    v.active_since_ms=static_cast<TimestampMs>(sqlite3_column_int64(s,8)); v.last_evaluated_at_ms=static_cast<TimestampMs>(sqlite3_column_int64(s,9));
    v.acknowledged=sqlite3_column_int(s,10)!=0; v.acknowledged_at_ms=static_cast<TimestampMs>(sqlite3_column_int64(s,11));
    v.acknowledged_by=text_column(s,12); v.updated_at_ms=static_cast<TimestampMs>(sqlite3_column_int64(s,13)); return v;
}

constexpr const char* kRuleColumns = "device_id,point_key,enabled,high_enabled,high_threshold,low_enabled,low_threshold,level,hysteresis,trigger_count,recovery_count,updated_at_ms";
constexpr const char* kStateColumns = "device_id,point_key,state,direction,current_value,threshold_value,consecutive_trigger_count,consecutive_recovery_count,active_since_ms,last_evaluated_at_ms,acknowledged,acknowledged_at_ms,acknowledged_by,updated_at_ms";
}

// 关闭数据库连接并释放存储资源；内部加锁保证析构安全。
AlarmStore::~AlarmStore() { std::lock_guard<std::mutex> lock(mutex_); close_locked(); }

// 打开报警数据库并初始化表结构。
StatusCode AlarmStore::initialize(const std::string& path, std::string* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (database_ != nullptr && database_path_ == path) return initialize_schema_locked(error);
    close_locked();
    if (path.empty()) return set_error(error, "告警 SQLite 数据库路径不能为空", StatusCode::kInvalidArgument);
    if (sqlite3_open_v2(path.c_str(), &database_, SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) { auto message=db_error(database_); close_locked(); return set_error(error, "打开告警配置 SQLite 数据库失败: "+message); }
    database_path_=path; sqlite3_busy_timeout(database_, 5000);
    return initialize_schema_locked(error);
}

// 在持锁状态下初始化数据库结构。
StatusCode AlarmStore::initialize_schema_locked(std::string* error)
{
    const auto configure = execute_locked(
        "PRAGMA busy_timeout=5000;PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;"
        "PRAGMA wal_autocheckpoint=100;", error);
    if (!is_ok(configure)) return configure;

    // 告警规则属于配置数据；表结构和配置库版本仅由 ConfigStore 管理。
    Statement statement(
        database_,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name IN ('alarm_rules','alarm_runtime_states','alarm_event_outbox');");
    if (!statement.ok() || sqlite3_step(statement.get()) != SQLITE_ROW) {
        return set_error(error, "检查告警配置表失败: " + db_error(database_));
    }
    if (sqlite3_column_int(statement.get(), 0) != 3) {
        return set_error(
            error,
            schema_migration_required_message("edge-config.db 缺少告警配置表"),
            StatusCode::kInvalidState);
    }
    return StatusCode::kOk;
}

// 新增或更新规则。
StatusCode AlarmStore::upsert_rule(const AlarmRule& r, std::string* error)
{
    const auto validation_status = validate_alarm_rule(r, error);
    if (!is_ok(validation_status)) return validation_status;
    // 未启用的方向仍按 0 入库，避免 NaN/Inf 进入 SQLite 后通过 JSON 暴露给前端。
    const auto stored_high_threshold = std::isfinite(r.high_threshold) ? r.high_threshold : 0.0;
    const auto stored_low_threshold = std::isfinite(r.low_threshold) ? r.low_threshold : 0.0;
    std::lock_guard<std::mutex> lock(mutex_); if (!available_locked(error)) return StatusCode::kInvalidState;
    Statement s(database_, "INSERT INTO alarm_rules VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(device_id,point_key) DO UPDATE SET enabled=excluded.enabled,high_enabled=excluded.high_enabled,high_threshold=excluded.high_threshold,low_enabled=excluded.low_enabled,low_threshold=excluded.low_threshold,level=excluded.level,hysteresis=excluded.hysteresis,trigger_count=excluded.trigger_count,recovery_count=excluded.recovery_count,updated_at_ms=excluded.updated_at_ms;");
    if (!s.ok()) return set_error(error,"准备告警规则写入失败: "+db_error(database_));
    bool ok=bind_text(s.get(),1,r.device_id)&&bind_text(s.get(),2,r.point_key)&&sqlite3_bind_int(s.get(),3,r.enabled)==SQLITE_OK&&sqlite3_bind_int(s.get(),4,r.high_enabled)==SQLITE_OK&&sqlite3_bind_double(s.get(),5,stored_high_threshold)==SQLITE_OK&&sqlite3_bind_int(s.get(),6,r.low_enabled)==SQLITE_OK&&sqlite3_bind_double(s.get(),7,stored_low_threshold)==SQLITE_OK&&bind_text(s.get(),8,r.level)&&sqlite3_bind_double(s.get(),9,r.hysteresis)==SQLITE_OK&&sqlite3_bind_int64(s.get(),10,r.trigger_count)==SQLITE_OK&&sqlite3_bind_int64(s.get(),11,r.recovery_count)==SQLITE_OK&&sqlite3_bind_int64(s.get(),12,static_cast<sqlite3_int64>(r.updated_at_ms))==SQLITE_OK;
    if (!ok || sqlite3_step(s.get()) != SQLITE_DONE) {
        return set_error(error, "写入告警规则失败: " + db_error(database_));
    }
    return StatusCode::kOk;
}

// 读取规则。
StatusCode AlarmStore::get_rule(const DeviceId& d,const std::string& p,std::optional<AlarmRule>* out,std::string* error) const
{
    if (out == nullptr) {
        return set_error(error, "告警规则输出参数为空", StatusCode::kInvalidArgument);
    }
    out->reset();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_locked(error)) return StatusCode::kInvalidState;
    std::string sql=std::string("SELECT ")+kRuleColumns+" FROM alarm_rules WHERE device_id=? AND point_key=?;"; Statement s(database_,sql.c_str()); if(!s.ok()||!bind_text(s.get(),1,d)||!bind_text(s.get(),2,p))return set_error(error,"准备告警规则查询失败: "+db_error(database_)); int rc=sqlite3_step(s.get()); if(rc==SQLITE_ROW){*out=read_rule(s.get());return StatusCode::kOk;} if(rc==SQLITE_DONE)return StatusCode::kNotFound; return set_error(error,"查询告警规则失败: "+db_error(database_));
}

// 列出规则。
StatusCode AlarmStore::list_rules(std::vector<AlarmRule>* out,std::string* error) const { if(out==nullptr)return set_error(error,"告警规则输出参数为空",StatusCode::kInvalidArgument); out->clear(); std::lock_guard<std::mutex> lock(mutex_); if(!available_locked(error))return StatusCode::kInvalidState; std::string sql=std::string("SELECT ")+kRuleColumns+" FROM alarm_rules ORDER BY device_id,point_key;"; Statement s(database_,sql.c_str()); if(!s.ok())return set_error(error,"准备告警规则列表查询失败: "+db_error(database_)); int rc; while((rc=sqlite3_step(s.get()))==SQLITE_ROW)out->push_back(read_rule(s.get())); return rc==SQLITE_DONE?StatusCode::kOk:set_error(error,"查询告警规则列表失败: "+db_error(database_)); }
// 列出规则按设备。
StatusCode AlarmStore::list_rules_by_device(const DeviceId& d,std::vector<AlarmRule>* out,std::string* error) const { if(out==nullptr)return set_error(error,"告警规则输出参数为空",StatusCode::kInvalidArgument); out->clear(); std::lock_guard<std::mutex> lock(mutex_); if(!available_locked(error))return StatusCode::kInvalidState; std::string sql=std::string("SELECT ")+kRuleColumns+" FROM alarm_rules WHERE device_id=? ORDER BY point_key;"; Statement s(database_,sql.c_str()); if(!s.ok()||!bind_text(s.get(),1,d))return set_error(error,"准备设备告警规则查询失败: "+db_error(database_)); int rc; while((rc=sqlite3_step(s.get()))==SQLITE_ROW)out->push_back(read_rule(s.get())); return rc==SQLITE_DONE?StatusCode::kOk:set_error(error,"查询设备告警规则失败: "+db_error(database_)); }

// 新增或更新报警运行状态。
StatusCode AlarmStore::upsert_runtime_state(const AlarmRuntimeState& v,std::string* error)
{
    if (!valid_runtime_state(v, error)) return StatusCode::kInvalidArgument;
    // 运行态是告警状态机的断点续传数据，保存前必须保证状态、方向和连续计数互相匹配。
    std::lock_guard<std::mutex> lock(mutex_); if(!available_locked(error))return StatusCode::kInvalidState; Statement s(database_,"INSERT INTO alarm_runtime_states(device_id,point_key,state,direction,current_value,threshold_value,consecutive_trigger_count,consecutive_recovery_count,active_since_ms,last_evaluated_at_ms,acknowledged,acknowledged_at_ms,acknowledged_by,updated_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(device_id,point_key) DO UPDATE SET state=excluded.state,direction=excluded.direction,current_value=excluded.current_value,threshold_value=excluded.threshold_value,consecutive_trigger_count=excluded.consecutive_trigger_count,consecutive_recovery_count=excluded.consecutive_recovery_count,active_since_ms=excluded.active_since_ms,last_evaluated_at_ms=excluded.last_evaluated_at_ms,acknowledged=excluded.acknowledged,acknowledged_at_ms=excluded.acknowledged_at_ms,acknowledged_by=excluded.acknowledged_by,updated_at_ms=excluded.updated_at_ms;"); if(!s.ok())return set_error(error,"准备告警状态写入失败: "+db_error(database_)); bool ok=bind_text(s.get(),1,v.device_id)&&bind_text(s.get(),2,v.point_key)&&bind_text(s.get(),3,v.state)&&bind_text(s.get(),4,v.direction)&&sqlite3_bind_double(s.get(),5,v.current_value)==SQLITE_OK&&sqlite3_bind_double(s.get(),6,v.threshold_value)==SQLITE_OK&&sqlite3_bind_int64(s.get(),7,v.consecutive_trigger_count)==SQLITE_OK&&sqlite3_bind_int64(s.get(),8,v.consecutive_recovery_count)==SQLITE_OK&&sqlite3_bind_int64(s.get(),9,static_cast<sqlite3_int64>(v.active_since_ms))==SQLITE_OK&&sqlite3_bind_int64(s.get(),10,static_cast<sqlite3_int64>(v.last_evaluated_at_ms))==SQLITE_OK&&sqlite3_bind_int(s.get(),11,v.acknowledged?1:0)==SQLITE_OK&&sqlite3_bind_int64(s.get(),12,static_cast<sqlite3_int64>(v.acknowledged_at_ms))==SQLITE_OK&&bind_text(s.get(),13,v.acknowledged_by)&&sqlite3_bind_int64(s.get(),14,static_cast<sqlite3_int64>(v.updated_at_ms))==SQLITE_OK; if(!ok||sqlite3_step(s.get())!=SQLITE_DONE)return set_error(error,"写入告警状态失败: "+db_error(database_)); return StatusCode::kOk;
}

// 读取指定点位的报警运行状态。
StatusCode AlarmStore::get_runtime_state(const DeviceId& d,const std::string& p,std::optional<AlarmRuntimeState>* out,std::string* error) const { if(out==nullptr)return set_error(error,"告警状态输出参数为空",StatusCode::kInvalidArgument); out->reset(); std::lock_guard<std::mutex> lock(mutex_); if(!available_locked(error))return StatusCode::kInvalidState; std::string sql=std::string("SELECT ")+kStateColumns+" FROM alarm_runtime_states WHERE device_id=? AND point_key=?;"; Statement s(database_,sql.c_str()); if(!s.ok()||!bind_text(s.get(),1,d)||!bind_text(s.get(),2,p))return set_error(error,"准备告警状态查询失败: "+db_error(database_)); int rc=sqlite3_step(s.get()); if(rc==SQLITE_ROW){*out=read_state(s.get());return StatusCode::kOk;} if(rc==SQLITE_DONE)return StatusCode::kNotFound; return set_error(error,"查询告警状态失败: "+db_error(database_)); }
// 列出全部报警运行状态。
StatusCode AlarmStore::list_runtime_states(std::vector<AlarmRuntimeState>* out,std::string* error) const { if(out==nullptr)return set_error(error,"告警状态输出参数为空",StatusCode::kInvalidArgument); out->clear(); std::lock_guard<std::mutex> lock(mutex_); if(!available_locked(error))return StatusCode::kInvalidState; std::string sql=std::string("SELECT ")+kStateColumns+" FROM alarm_runtime_states ORDER BY device_id,point_key;"; Statement s(database_,sql.c_str()); if(!s.ok())return set_error(error,"准备告警状态列表查询失败: "+db_error(database_)); int rc; while((rc=sqlite3_step(s.get()))==SQLITE_ROW)out->push_back(read_state(s.get())); return rc==SQLITE_DONE?StatusCode::kOk:set_error(error,"查询告警状态列表失败: "+db_error(database_)); }
// 列出当前活动报警状态。
StatusCode AlarmStore::list_active_runtime_states(std::vector<AlarmRuntimeState>* out,std::string* error) const { if(out==nullptr)return set_error(error,"告警状态输出参数为空",StatusCode::kInvalidArgument); out->clear(); std::lock_guard<std::mutex> lock(mutex_); if(!available_locked(error))return StatusCode::kInvalidState; std::string sql=std::string("SELECT ")+kStateColumns+" FROM alarm_runtime_states WHERE state='active' ORDER BY active_since_ms DESC;"; Statement s(database_,sql.c_str()); if(!s.ok())return set_error(error,"准备活动告警查询失败: "+db_error(database_)); int rc; while((rc=sqlite3_step(s.get()))==SQLITE_ROW)out->push_back(read_state(s.get())); return rc==SQLITE_DONE?StatusCode::kOk:set_error(error,"查询活动告警失败: "+db_error(database_)); }

StatusCode AlarmStore::delete_rule(const DeviceId& d,const std::string& p,std::string* e){std::lock_guard<std::mutex> l(mutex_);if(!available_locked(e))return StatusCode::kInvalidState;Statement s(database_,"DELETE FROM alarm_rules WHERE device_id=? AND point_key=?;");if(!s.ok()||!bind_text(s.get(),1,d)||!bind_text(s.get(),2,p)||sqlite3_step(s.get())!=SQLITE_DONE)return set_error(e,"删除告警规则失败: "+db_error(database_));return StatusCode::kOk;}
// 清空规则。
StatusCode AlarmStore::clear_rules(std::string* e){std::lock_guard<std::mutex> l(mutex_);return execute_locked("DELETE FROM alarm_rules;",e);}
StatusCode AlarmStore::delete_runtime_state(const DeviceId& d,const std::string& p,std::string* e){std::lock_guard<std::mutex> l(mutex_);if(!available_locked(e))return StatusCode::kInvalidState;Statement s(database_,"DELETE FROM alarm_runtime_states WHERE device_id=? AND point_key=?;");if(!s.ok()||!bind_text(s.get(),1,d)||!bind_text(s.get(),2,p)||sqlite3_step(s.get())!=SQLITE_DONE)return set_error(e,"删除告警状态失败: "+db_error(database_));return StatusCode::kOk;}

// 原子应用一批运行态变更；任一绑定、执行或提交失败都回滚全部写入。
StatusCode AlarmStore::apply_runtime_state_batch(
    const std::vector<AlarmRuntimeState>& upserts,
    const std::vector<AlarmRuntimeStateKey>& deletes,
    std::string* error,
    std::vector<ServiceEvent>* events)
{
    for (const auto& state : upserts) {
        if (!valid_runtime_state(state, error)) return StatusCode::kInvalidArgument;
    }
    for (const auto& key : deletes) {
        if (key.device_id.empty() || key.point_key.empty()) {
            return set_error(error, "批量删除告警运行态的键不能为空", StatusCode::kInvalidArgument);
        }
    }
    if (upserts.empty() && deletes.empty() && (events == nullptr || events->empty())) return StatusCode::kOk;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_locked(error)) return StatusCode::kInvalidState;
    auto status = execute_locked("BEGIN IMMEDIATE;", error);
    if (!is_ok(status)) return status;

    const auto rollback_with_error = [&](const std::string& message, StatusCode code = StatusCode::kIoError) {
        (void)execute_locked("ROLLBACK;", nullptr);
        return set_error(error, message, code);
    };

    if (!upserts.empty()) {
        Statement statement(
            database_,
            "INSERT INTO alarm_runtime_states(device_id,point_key,state,direction,current_value,"
            "threshold_value,consecutive_trigger_count,consecutive_recovery_count,active_since_ms,"
            "last_evaluated_at_ms,acknowledged,acknowledged_at_ms,acknowledged_by,updated_at_ms) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(device_id,point_key) DO UPDATE SET state=excluded.state,"
            "direction=excluded.direction,current_value=excluded.current_value,"
            "threshold_value=excluded.threshold_value,"
            "consecutive_trigger_count=excluded.consecutive_trigger_count,"
            "consecutive_recovery_count=excluded.consecutive_recovery_count,"
            "active_since_ms=excluded.active_since_ms,"
            "last_evaluated_at_ms=excluded.last_evaluated_at_ms,"
            "acknowledged=excluded.acknowledged,"
            "acknowledged_at_ms=excluded.acknowledged_at_ms,"
            "acknowledged_by=excluded.acknowledged_by,updated_at_ms=excluded.updated_at_ms;");
        if (!statement.ok()) {
            return rollback_with_error("准备批量写入告警运行态失败: " + db_error(database_));
        }
        for (const auto& state : upserts) {
            if (sqlite3_reset(statement.get()) != SQLITE_OK ||
                sqlite3_clear_bindings(statement.get()) != SQLITE_OK ||
                !bind_runtime_state(statement.get(), state) ||
                sqlite3_step(statement.get()) != SQLITE_DONE) {
                return rollback_with_error("批量写入告警运行态失败: " + db_error(database_));
            }
        }
    }

    if (!deletes.empty()) {
        Statement statement(
            database_,
            "DELETE FROM alarm_runtime_states WHERE device_id=? AND point_key=?;");
        if (!statement.ok()) {
            return rollback_with_error("准备批量删除告警运行态失败: " + db_error(database_));
        }
        for (const auto& key : deletes) {
            if (sqlite3_reset(statement.get()) != SQLITE_OK ||
                sqlite3_clear_bindings(statement.get()) != SQLITE_OK ||
                !bind_text(statement.get(), 1, key.device_id) ||
                !bind_text(statement.get(), 2, key.point_key) ||
                sqlite3_step(statement.get()) != SQLITE_DONE) {
                return rollback_with_error("批量删除告警运行态失败: " + db_error(database_));
            }
        }
    }

    // 与状态同事务保存事件，断电恢复后仍能搬运到事件库。
    if (events != nullptr) {
        try {
            for (auto& event : *events) {
                if (event.event_id.empty()) {
                    Statement id(database_, "SELECT 'alarm-' || lower(hex(randomblob(16)));");
                    if (!id.ok() || sqlite3_step(id.get()) != SQLITE_ROW)
                        return rollback_with_error("生成告警事件 ID 失败");
                    event.event_id = text_column(id.get(), 0);
                }
                const auto payload = encode_service_event(event).dump();
                Statement insert(database_, "INSERT INTO alarm_event_outbox(event_id,payload) VALUES(?,?);");
                if (!insert.ok() || !bind_text(insert.get(), 1, event.event_id) ||
                    !bind_text(insert.get(), 2, payload) || sqlite3_step(insert.get()) != SQLITE_DONE)
                    return rollback_with_error("保存告警事件待发送记录失败: " + db_error(database_));
            }
        } catch (const std::exception& exception) {
            return rollback_with_error(exception.what());
        }
    }
    status = execute_locked("COMMIT;", error);
    if (!is_ok(status)) {
        const auto message = error == nullptr ? std::string("提交告警运行态批处理失败") : *error;
        return rollback_with_error(message);
    }
    return StatusCode::kOk;
}
StatusCode AlarmStore::pending_events(std::vector<ServiceEvent>* events, std::string* error) const
{
    if (events == nullptr) return StatusCode::kInvalidArgument;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_locked(error)) return StatusCode::kInvalidState;
    Statement query(database_, "SELECT payload FROM alarm_event_outbox ORDER BY sequence LIMIT 32;");
    if (!query.ok()) return set_error(error, db_error(database_));
    std::vector<ServiceEvent> result;
    int step;
    try {
        while ((step = sqlite3_step(query.get())) == SQLITE_ROW)
            result.push_back(decode_service_event(text_column(query.get(), 0)));
    } catch (const std::exception& exception) { return set_error(error, exception.what()); }
    if (step != SQLITE_DONE) return set_error(error, db_error(database_));
    *events = std::move(result);
    return StatusCode::kOk;
}

StatusCode AlarmStore::clear_event_outbox(std::string* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return execute_locked("DELETE FROM alarm_event_outbox;", error);
}

StatusCode AlarmStore::acknowledge_event(const std::string& event_id, std::string* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_locked(error)) return StatusCode::kInvalidState;
    Statement statement(database_, "DELETE FROM alarm_event_outbox WHERE event_id=?;");
    if (!statement.ok() || !bind_text(statement.get(), 1, event_id) || sqlite3_step(statement.get()) != SQLITE_DONE)
        return set_error(error, db_error(database_));
    return StatusCode::kOk;
}

// 清空全部告警运行状态。
StatusCode AlarmStore::clear_runtime_states(std::string* e){std::lock_guard<std::mutex> l(mutex_);return execute_locked("DELETE FROM alarm_runtime_states;",e);}
// 原子替换全部告警运行状态；任一状态非法或写入失败时保留替换前数据。
StatusCode AlarmStore::replace_runtime_states(
    const std::vector<AlarmRuntimeState>& states,
    std::string* error)
{
    for (const auto& state : states) {
        if (!valid_runtime_state(state, error)) return StatusCode::kInvalidArgument;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_locked(error)) return StatusCode::kInvalidState;
    auto status = execute_locked("BEGIN IMMEDIATE;", error);
    if (!is_ok(status)) return status;

    auto rollback_with_error = [&](const std::string& message, StatusCode code = StatusCode::kIoError) {
        (void)execute_locked("ROLLBACK;", nullptr);
        return set_error(error, message, code);
    };
    status = execute_locked("DELETE FROM alarm_runtime_states;", error);
    if (!is_ok(status)) {
        const auto message = error == nullptr ? std::string("清空告警运行状态失败") : *error;
        return rollback_with_error(message);
    }

    Statement statement(
        database_,
        "INSERT INTO alarm_runtime_states(device_id,point_key,state,direction,current_value,"
        "threshold_value,consecutive_trigger_count,consecutive_recovery_count,active_since_ms,"
        "last_evaluated_at_ms,acknowledged,acknowledged_at_ms,acknowledged_by,updated_at_ms) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!statement.ok()) {
        return rollback_with_error("准备告警运行状态替换失败: " + db_error(database_));
    }
    for (const auto& state : states) {
        sqlite3_reset(statement.get());
        sqlite3_clear_bindings(statement.get());
        const bool bound =
            bind_text(statement.get(), 1, state.device_id) &&
            bind_text(statement.get(), 2, state.point_key) &&
            bind_text(statement.get(), 3, state.state) &&
            bind_text(statement.get(), 4, state.direction) &&
            sqlite3_bind_double(statement.get(), 5, state.current_value) == SQLITE_OK &&
            sqlite3_bind_double(statement.get(), 6, state.threshold_value) == SQLITE_OK &&
            sqlite3_bind_int64(statement.get(), 7, state.consecutive_trigger_count) == SQLITE_OK &&
            sqlite3_bind_int64(statement.get(), 8, state.consecutive_recovery_count) == SQLITE_OK &&
            sqlite3_bind_int64(statement.get(), 9, static_cast<sqlite3_int64>(state.active_since_ms)) == SQLITE_OK &&
            sqlite3_bind_int64(statement.get(), 10, static_cast<sqlite3_int64>(state.last_evaluated_at_ms)) == SQLITE_OK &&
            sqlite3_bind_int(statement.get(), 11, state.acknowledged ? 1 : 0) == SQLITE_OK &&
            sqlite3_bind_int64(statement.get(), 12, static_cast<sqlite3_int64>(state.acknowledged_at_ms)) == SQLITE_OK &&
            bind_text(statement.get(), 13, state.acknowledged_by) &&
            sqlite3_bind_int64(statement.get(), 14, static_cast<sqlite3_int64>(state.updated_at_ms)) == SQLITE_OK;
        if (!bound || sqlite3_step(statement.get()) != SQLITE_DONE) {
            return rollback_with_error("替换告警运行状态失败: " + db_error(database_));
        }
    }

    status = execute_locked("COMMIT;", error);
    if (!is_ok(status)) {
        const auto message = error == nullptr ? std::string("提交告警运行状态替换失败") : *error;
        return rollback_with_error(message);
    }
    return StatusCode::kOk;
}

// 在配置导入共享事务中替换规则及运行状态；事务边界由 ConfigImportTransaction 管理。
StatusCode AlarmStore::replace_alarm_data_for_import_locked(
    const std::vector<AlarmRule>& rules,
    const std::vector<AlarmRuntimeState>& states,
    std::string* error)
{
    for (const auto& rule : rules) {
        const auto validation_status = validate_alarm_rule(rule, error);
        if (!is_ok(validation_status)) return validation_status;
    }
    for (const auto& state : states) {
        if (!valid_runtime_state(state, error)) return StatusCode::kInvalidArgument;
    }
    if (!available_locked(error)) return StatusCode::kInvalidState;

    auto status = execute_locked("DELETE FROM alarm_rules;", error);
    if (!is_ok(status)) return status;
    status = execute_locked("DELETE FROM alarm_runtime_states;", error);
    if (!is_ok(status)) return status;

    Statement rule_statement(
        database_,
        "INSERT INTO alarm_rules(device_id,point_key,enabled,high_enabled,high_threshold,"
        "low_enabled,low_threshold,level,hysteresis,trigger_count,recovery_count,updated_at_ms) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!rule_statement.ok()) {
        return set_error(error, "准备导入告警规则失败: " + db_error(database_));
    }
    for (const auto& rule : rules) {
        sqlite3_reset(rule_statement.get());
        sqlite3_clear_bindings(rule_statement.get());
        const auto stored_high_threshold =
            std::isfinite(rule.high_threshold) ? rule.high_threshold : 0.0;
        const auto stored_low_threshold =
            std::isfinite(rule.low_threshold) ? rule.low_threshold : 0.0;
        const bool bound =
            bind_text(rule_statement.get(), 1, rule.device_id) &&
            bind_text(rule_statement.get(), 2, rule.point_key) &&
            sqlite3_bind_int(rule_statement.get(), 3, rule.enabled ? 1 : 0) == SQLITE_OK &&
            sqlite3_bind_int(rule_statement.get(), 4, rule.high_enabled ? 1 : 0) == SQLITE_OK &&
            sqlite3_bind_double(rule_statement.get(), 5, stored_high_threshold) == SQLITE_OK &&
            sqlite3_bind_int(rule_statement.get(), 6, rule.low_enabled ? 1 : 0) == SQLITE_OK &&
            sqlite3_bind_double(rule_statement.get(), 7, stored_low_threshold) == SQLITE_OK &&
            bind_text(rule_statement.get(), 8, rule.level) &&
            sqlite3_bind_double(rule_statement.get(), 9, rule.hysteresis) == SQLITE_OK &&
            sqlite3_bind_int64(rule_statement.get(), 10, rule.trigger_count) == SQLITE_OK &&
            sqlite3_bind_int64(rule_statement.get(), 11, rule.recovery_count) == SQLITE_OK &&
            sqlite3_bind_int64(
                rule_statement.get(), 12, static_cast<sqlite3_int64>(rule.updated_at_ms)) == SQLITE_OK;
        if (!bound || sqlite3_step(rule_statement.get()) != SQLITE_DONE) {
            return set_error(error, "导入告警规则失败: " + db_error(database_));
        }
    }

    Statement state_statement(
        database_,
        "INSERT INTO alarm_runtime_states(device_id,point_key,state,direction,current_value,"
        "threshold_value,consecutive_trigger_count,consecutive_recovery_count,active_since_ms,"
        "last_evaluated_at_ms,acknowledged,acknowledged_at_ms,acknowledged_by,updated_at_ms) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!state_statement.ok()) {
        return set_error(error, "准备导入告警运行状态失败: " + db_error(database_));
    }
    for (const auto& state : states) {
        sqlite3_reset(state_statement.get());
        sqlite3_clear_bindings(state_statement.get());
        if (!bind_runtime_state(state_statement.get(), state) ||
            sqlite3_step(state_statement.get()) != SQLITE_DONE) {
            return set_error(error, "导入告警运行状态失败: " + db_error(database_));
        }
    }
    return StatusCode::kOk;
}
// 在持锁状态下执行。
StatusCode AlarmStore::execute_locked(const char* sql, std::string* e) const {
    if (!available_locked(e)) {
        return StatusCode::kInvalidState;
    }
    char* m = nullptr;
    int rc = sqlite3_exec(database_, sql, nullptr, nullptr, &m);
    if (rc == SQLITE_OK) {
        return StatusCode::kOk;
    }
    std::string message = m == nullptr ? db_error(database_) : m;
    sqlite3_free(m);
    return set_error(e, message);
}
// 在持锁状态下检查数据库是否可用。
bool AlarmStore::available_locked(std::string* e) const {
    if (database_ != nullptr) {
        return true;
    }
    set_error(e, "告警 SQLite 数据库尚未初始化", StatusCode::kInvalidState);
    return false;
}
// 在持锁状态下关闭。
void AlarmStore::close_locked() {
    if (database_ != nullptr) {
        sqlite3_close(database_);
    }
    database_ = nullptr;
    database_path_.clear();
}
}  // namespace edge_controller
