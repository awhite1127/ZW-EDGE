// 把按设备、按区块保存的寄存器结果解析为设备状态与中文数据项。
// 边界：只执行模板约束下的采集或写入，不负责轮询调度。

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "collect/device_read_result.h"
#include "model/device_config.h"
#include "model/device_status.h"
#include "model/device_template.h"
#include "model/master_node_config.h"

namespace edge_controller {

struct RegisterMapperResult {
    bool success{false};
    std::vector<DeviceStatus> device_statuses;
    std::vector<std::string> errors;
};

enum class RegisterFieldDecoderKind : std::uint8_t {
    kUint16,
    kInt16,
    kHighUint8,
    kLowUint8,
    kUint32,
    kInt32,
    kFloat32,
    kBitUint16,
};

enum class RegisterInvalidRuleKind : std::uint8_t {
    kNone,
    kEqual,
    kGreaterOrEqual,
    kLessOrEqual,
    kInsideRange,
};

// 配置世代内稳定的字段解析元数据；模板注册表更新后由 PollingService 重新构建。
struct RegisterMapperFieldRuntime {
    const DeviceTemplateFieldDefinition* definition{nullptr};
    RegisterFieldDecoderKind decoder_kind{RegisterFieldDecoderKind::kUint16};
    RegisterCount required_register_count{0};
    bool swap_bytes{false};
    bool low_word_first{false};
    RegisterInvalidRuleKind invalid_rule_kind{RegisterInvalidRuleKind::kNone};
    double invalid_rule_value{0.0};
    double invalid_rule_min{0.0};
    double invalid_rule_max{0.0};
    std::string invalid_rule_condition;
    bool definition_valid{false};
    std::string definition_error;
};

struct RegisterMapperRuntimePlan {
    // shared_ptr 保证模板注册表热替换后，正在执行的本轮映射仍持有完整旧快照。
    std::shared_ptr<const DeviceTemplateDefinition> device_template;
    std::vector<RegisterMapperFieldRuntime> ordered_fields;
};

class RegisterMapper {
public:
    // 从不可变设备模板快照预编译字段顺序、解析器、排列、无效规则和静态校验结果。
    static RegisterMapperRuntimePlan build_runtime_plan(
        std::shared_ptr<const DeviceTemplateDefinition> device_template);

    // 将按设备、block_key 保存的独立区块结果映射为设备状态和数据项。
    static RegisterMapperResult map_devices(
        const MasterNodeConfig& master_config,
        const std::vector<const DeviceConfig*>& devices,
        const std::vector<DeviceReadResult>& device_read_results,
        TimestampMs update_time_ms);

    // 使用配置世代内预编译的字段元数据执行映射，避免每轮查模板、排序和重复静态校验。
    static RegisterMapperResult map_devices(
        const MasterNodeConfig& master_config,
        const std::vector<const DeviceConfig*>& devices,
        const RegisterMapperRuntimePlan& runtime_plan,
        const std::vector<DeviceReadResult>& device_read_results,
        TimestampMs update_time_ms);

};

}  // namespace edge_controller
