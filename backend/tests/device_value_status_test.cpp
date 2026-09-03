#include "communication/collect/register_mapper.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "data/model/builtin_device_templates.h"
#include "data/model/device_value_status.h"

namespace {

using namespace edge_controller;

bool expect(bool condition, const std::string& message)
{
    if (condition) {
        return true;
    }
    std::cerr << message << '\n';
    return false;
}

DeviceReadResult make_read_result(
    const DeviceTemplateDefinition& device_template,
    const MasterNodeConfig& master,
    const DeviceConfig& device,
    std::size_t device_index,
    std::uint16_t raw_value)
{
    DeviceReadResult result;
    result.device_id = device.device_id;
    result.device_index = device_index;
    result.device_base_address =
        master.block_start_register +
        static_cast<std::uint32_t>(device_index) * device_template.device_address_stride;

    const auto& block = device_template.read_blocks.front();
    DeviceReadBlockResult block_result;
    block_result.block_key = block.block_key;
    block_result.display_name = block.display_name;
    block_result.function_code = block.function_code;
    block_result.request_address = result.device_base_address + block.start_offset;
    block_result.register_count = block.register_count;
    block_result.success = true;
    block_result.registers = {raw_value};
    result.read_blocks.emplace(block.block_key, std::move(block_result));
    return result;
}

}  // namespace

int main()
{
    bool passed = true;
    passed &= expect(
        std::string(to_string(DeviceValueStatusCode::kBuiltinUint16DeviceFault)) ==
            "BUILTIN_UINT16_DEVICE_FAULT",
        "uint16 device fault status prefix is incorrect");
    passed &= expect(
        std::string(to_string(DeviceValueStatusCode::kBuiltinUint16DeviceOpenCircuit)) ==
            "BUILTIN_UINT16_DEVICE_OPEN_CIRCUIT",
        "uint16 open-circuit status prefix is incorrect");
    passed &= expect(
        !classify_device_value_status(DeviceValueStatusScope::kNone, 65535.0).matched(),
        "an unregistered data-width scope must not reuse uint16 rules");

    for (const auto& builtin_template : builtin_device_templates()) {
        for (const auto& field : builtin_template.fields) {
            passed &= expect(
                field.invalid_rule_type == "none" &&
                    field.invalid_rule_value == 0.0 &&
                    field.invalid_rule_min == 0.0 &&
                    field.invalid_rule_max == 0.0,
                "built-in fields must not carry template-level invalid-value rules");
        }
    }

    const auto device_template =
        std::make_shared<DeviceTemplateDefinition>(default_device_template());
    MasterNodeConfig master;
    master.master_id = "master-1";
    master.master_name = "Master 1";
    master.device_template = device_template->template_id;
    master.block_start_register = device_template->default_start_register;
    master.device_count = 4;

    std::vector<DeviceConfig> devices(4);
    std::vector<const DeviceConfig*> device_pointers;
    device_pointers.reserve(devices.size());
    for (std::size_t index = 0; index < devices.size(); ++index) {
        devices[index].device_id = "device-" + std::to_string(index + 1);
        devices[index].device_name = "Device " + std::to_string(index + 1);
        devices[index].master_id = master.master_id;
        device_pointers.push_back(&devices[index]);
    }

    const std::vector<std::uint16_t> raw_values{1, 0xFFFFU, 0xFFFEU, 0xFFF0U};
    std::vector<DeviceReadResult> read_results;
    read_results.reserve(raw_values.size());
    for (std::size_t index = 0; index < raw_values.size(); ++index) {
        read_results.push_back(make_read_result(
            *device_template,
            master,
            devices[index],
            index,
            raw_values[index]));
    }

    const auto plan = RegisterMapper::build_runtime_plan(device_template);
    passed &= expect(
        plan.ordered_fields.size() == 1 &&
            plan.ordered_fields[0].value_status_scope == DeviceValueStatusScope::kBuiltinUint16,
        "built-in uint16 field did not receive the uint16 value-status scope");

    auto custom_template =
        std::make_shared<DeviceTemplateDefinition>(*device_template);
    custom_template->builtin = false;
    const auto custom_plan = RegisterMapper::build_runtime_plan(custom_template);
    passed &= expect(
        custom_plan.ordered_fields.size() == 1 &&
            custom_plan.ordered_fields[0].value_status_scope == DeviceValueStatusScope::kNone,
        "custom uint16 fields must not inherit built-in value-status rules");

    auto builtin_uint32_template =
        std::make_shared<DeviceTemplateDefinition>(*device_template);
    builtin_uint32_template->fields[0].parser_id = "scaled_uint32";
    builtin_uint32_template->fields[0].data_type = "uint32";
    builtin_uint32_template->fields[0].register_count = 2;
    builtin_uint32_template->read_blocks[0].register_count = 2;
    builtin_uint32_template->device_address_stride = 2;
    const auto builtin_uint32_plan =
        RegisterMapper::build_runtime_plan(builtin_uint32_template);
    passed &= expect(
        builtin_uint32_plan.ordered_fields.size() == 1 &&
            builtin_uint32_plan.ordered_fields[0].value_status_scope ==
                DeviceValueStatusScope::kNone,
        "non-uint16 built-in fields must not inherit uint16 value-status rules");

    const auto mapped = RegisterMapper::map_devices(
        master,
        device_pointers,
        plan,
        read_results,
        1000);

    passed &= expect(mapped.success, "valid responses should map successfully");
    passed &= expect(mapped.device_statuses.size() == 4, "unexpected device status count");
    if (mapped.device_statuses.size() != 4) {
        return 1;
    }

    const auto& normal = mapped.device_statuses[0];
    passed &= expect(normal.online && normal.last_collect_success,
                     "normal device should remain online");
    passed &= expect(normal.diagnosis.error_code == "NONE",
                     "normal value should not create a diagnosis");
    passed &= expect(normal.points.size() == 1 && normal.points[0].valid,
                     "normal point should be valid");

    const auto& fault = mapped.device_statuses[1];
    passed &= expect(fault.online && fault.last_collect_success &&
                         fault.communication_quality == DataQuality::kGood,
                     "device fault sentinel must not be treated as communication failure");
    passed &= expect(fault.diagnosis.error_code == "DEVICE_FAULT",
                     "0xFFFF should map to DEVICE_FAULT");
    passed &= expect(fault.points.size() == 1 && !fault.points[0].valid &&
                         fault.points[0].raw_value == 65535.0 &&
                         fault.points[0].display_text == "设备故障",
                     "0xFFFF point semantics are incorrect");

    const auto& open_circuit = mapped.device_statuses[2];
    passed &= expect(open_circuit.online && open_circuit.last_collect_success,
                     "open-circuit sentinel must not be treated as communication failure");
    passed &= expect(open_circuit.diagnosis.error_code == "DEVICE_OPEN_CIRCUIT",
                     "0xFFFE should map to DEVICE_OPEN_CIRCUIT");
    passed &= expect(open_circuit.points.size() == 1 && !open_circuit.points[0].valid &&
                         open_circuit.points[0].raw_value == 65534.0 &&
                         open_circuit.points[0].display_text == "设备开路",
                     "0xFFFE point semantics are incorrect");

    const auto& ordinary_high_value = mapped.device_statuses[3];
    passed &= expect(ordinary_high_value.diagnosis.error_code == "NONE",
                     "an unregistered uint16 value should not create a diagnosis");
    passed &= expect(ordinary_high_value.points.size() == 1 &&
                         ordinary_high_value.points[0].valid &&
                         ordinary_high_value.points[0].raw_value == 65520.0 &&
                         ordinary_high_value.points[0].value == 655.2 &&
                         ordinary_high_value.points[0].display_text.empty(),
                     "0xFFF0 should remain an ordinary valid engineering value");

    return passed ? 0 : 1;
}
