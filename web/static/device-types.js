// 设备类型页控制器：编辑读取区块、数据项及实时/历史展示偏好。
// 编辑器在内存中维护完整草稿，提交时一次性交给后端校验，避免产生半配置状态。
(function () {
    "use strict";

    const EdgeApp = window.EdgeApp;
    if (!EdgeApp) return;

    const readApiResponse = EdgeApp.readApiResponse;
    const showToast = EdgeApp.showToast;
    const friendlyApiMessage = EdgeApp.friendlyApiMessage;
    const setFeedback = EdgeApp.setFeedback;
    const confirmAction = EdgeApp.confirmAction;
    let pageScope = null;

    // mutation 也必须归属当前页面 scope，软导航会中止请求并阻断旧页副作用。
    function csrfFetch(url, options) {
        const scope = pageScope;
        if (!scope || typeof scope.csrfFetch !== "function") {
            const error = new Error("stale page task");
            error.edgeStale = true;
            return Promise.reject(error);
        }
        return scope.csrfFetch(url, options);
    }

    function mount(scope) {
        pageScope = scope;
        initTemplateRealtimeDisplayControls();
        initTemplateHistoryControls();
        initDeviceTemplateManagement();
        initDeviceTemplateDeleteControls();
    }


    // ---------- 设备类型区块与数据项编辑 ----------
    function initDeviceTemplateManagement() {
        const root = document.querySelector("[data-template-editor-page]");
        const form = root ? root.querySelector("[data-template-editor-form]") : null;
        const fieldPanel = root ? root.querySelector("[data-template-field-panel]") : null;
        const fieldForm = fieldPanel ? fieldPanel.querySelector("[data-template-field-form]") : null;
        const blockPanel = root ? root.querySelector("[data-template-block-panel]") : null;
        const blockForm = blockPanel ? blockPanel.querySelector("[data-template-block-form]") : null;
        if (!root || !form || !fieldPanel || !fieldForm || !blockPanel || !blockForm) return;

        // 为非标准表单容器补充统一的表单操作接口。
        function attachFormFacade(container) {
            const controls = Array.from(container.querySelectorAll("input,select,textarea,button"));
            const elements = {};
            controls.forEach(function (control) {
                if (control.name) elements[control.name] = control;
            });
            container.elements = elements;
            container.reset = function () {
                controls.forEach(function (control) {
                    if (control.matches("input[type=checkbox],input[type=radio]")) control.checked = false;
                    else if (control.matches("input,textarea")) control.value = control.defaultValue || "";
                    else if (control.matches("select")) control.selectedIndex = 0;
                });
            };
            container.reportValidity = function () {
                const invalid = controls.find(function (control) {
                    return !control.disabled && typeof control.checkValidity === "function" && !control.checkValidity();
                });
                if (invalid && typeof invalid.reportValidity === "function") invalid.reportValidity();
                return !invalid;
            };
        }
        attachFormFacade(fieldForm);
        attachFormFacade(blockForm);

        const fieldsContainer = form.querySelector("[data-template-fields]");
        const fieldsEmpty = form.querySelector("[data-template-fields-empty]");
        const fieldsTable = form.querySelector(".template-fields-table");
        const blocksContainer = form.querySelector("[data-template-read-blocks]");
        const submitButton = form.querySelector("[data-template-editor-submit]");
        const feedback = form.querySelector("[data-template-editor-feedback]");
        const strideHint = form.querySelector("[data-template-stride-hint]");
        const fieldTitle = fieldPanel.querySelector("[data-template-field-editor-title]");
        const fieldFeedback = fieldForm.querySelector("[data-template-field-feedback]");
        const fieldBlockField = fieldForm.querySelector("[data-template-field-block-field]");
        const fieldBlockSelect = fieldForm.elements.field_read_block;
        const blockTitle = blockPanel.querySelector("[data-template-block-editor-title]");
        const blockFeedback = blockForm.querySelector("[data-template-block-feedback]");
        let editingID = "";
        let templateReadBlocks = [];
        let templateFields = [];
        let editingBlockIndex = -1;
        let editingFieldIndex = -1;
        let currentEnumItems = [];
        let realtimeGroups = [];
        let currentStep = 1;
        let dirty = false;
        let saving = false;

        const dataOrders = {
            ABCD: { byteOrder: "big_endian", wordOrder: "high_word_first" },
            BADC: { byteOrder: "little_endian", wordOrder: "high_word_first" },
            CDAB: { byteOrder: "big_endian", wordOrder: "low_word_first" },
            DCBA: { byteOrder: "little_endian", wordOrder: "low_word_first" }
        };
        const invalidRuleTypes = new Set(["none", "equal", "greater_or_equal", "less_or_equal", "inside_range"]);

        // 读取数值字段，无效时返回兜底值。
        function numberValue(field, fallback) {
            const value = Number(field && field.value);
            return Number.isFinite(value) ? value : fallback;
        }

        // 规范化无效规则类型。
        function normalizeInvalidRuleType(value) {
            const ruleType = String(value || "none").trim();
            return invalidRuleTypes.has(ruleType) ? ruleType : "none";
        }

        // 解析并校验告警规则中的有限数值。
        function finiteRuleNumber(value, label) {
            if (typeof value === "string" && value.trim() === "") throw new Error(label + "不能为空");
            const number = Number(value);
            if (!Number.isFinite(number)) throw new Error(label + "必须是有限数值");
            return number;
        }

        // 校验无效规则。
        function validateInvalidRule(ruleTypeValue, value, minimum, maximum) {
            const ruleType = String(ruleTypeValue || "").trim();
            if (!invalidRuleTypes.has(ruleType)) throw new Error("无效规则类型不受支持");
            if (ruleType === "none") {
                return { type: "none", value: 0, minimum: 0, maximum: 0 };
            }
            if (ruleType === "inside_range") {
                const min = finiteRuleNumber(minimum, "无效范围最小值");
                const max = finiteRuleNumber(maximum, "无效范围最大值");
                if (min > max) throw new Error("无效范围最小值不能大于最大值");
                return { type: ruleType, value: 0, minimum: min, maximum: max };
            }
            return {
                type: ruleType,
                value: finiteRuleNumber(value, "无效规则值"),
                minimum: 0,
                maximum: 0
            };
        }

        // 生成无效值规则摘要。
        function invalidRuleSummary(field) {
            switch (field.invalid_rule_type) {
            case "equal": return "无效：= " + field.invalid_rule_value;
            case "greater_or_equal": return "无效：≥ " + field.invalid_rule_value;
            case "less_or_equal": return "无效：≤ " + field.invalid_rule_value;
            case "inside_range": return "无效：" + field.invalid_rule_min + "～" + field.invalid_rule_max;
            default: return "无效规则：无";
            }
        }

        // 规范化读取区块。
        function normalizeReadBlock(value, index) {
            const block = value || {};
            return {
                block_key: String(block.block_key || "").trim(),
                display_name: String(block.display_name || "").trim(),
                function_code: Number(block.function_code),
                start_offset: Number(block.start_offset),
                register_count: Number(block.register_count),
                sort_order: Number(block.sort_order)
            };
        }

        // 生成区块标识。
        function generateBlockKey() {
            let sequence = 1;
            const keys = new Set(templateReadBlocks.map(function (block) { return block.block_key; }));
            while (keys.has("read_block_" + sequence)) sequence += 1;
            return "read_block_" + sequence;
        }

        // 按标识查找读取区块。
        function blockByKey(key) {
            return templateReadBlocks.find(function (block) { return block.block_key === key; }) || null;
        }

        // 计算读取区块的结束偏移。
        function blockEnd(block) {
            return Number(block.start_offset) + Number(block.register_count);
        }

        // 计算设备地址的最小允许跨度。
        function minimumStride() {
            return templateReadBlocks.reduce(function (maximum, block) {
                return Math.max(maximum, blockEnd(block));
            }, 1);
        }

        // 更新跨度提示。
        function updateStrideHint() {
            const minimum = minimumStride();
            const stride = numberValue(form.elements.device_address_stride, 0);
            const valid = Number.isInteger(stride) && stride >= minimum && stride <= 65536;
            form.elements.device_address_stride.min = String(minimum);
            form.elements.device_address_stride.setAttribute("aria-invalid", valid ? "false" : "true");
            if (strideHint) {
                strideHint.textContent = "相邻两台设备基地址之间的寄存器间隔。当前最小允许值：" + minimum + "。";
                strideHint.classList.toggle("is-error", !valid);
            }
        }

        // 返回读取区块的显示名称。
        function blockName(key) {
            const block = blockByKey(key);
            return block ? block.display_name : "未选择读取区块";
        }

        function parseModbusInteger(value, label) {
            const text = String(value || "").trim();
            if (!/^(?:0x[0-9a-f]+|\d+)$/i.test(text)) throw new Error(label + "必须是十进制或 0x 十六进制整数");
            const number = Number.parseInt(text, /^0x/i.test(text) ? 16 : 10);
            if (!Number.isInteger(number) || number < 0 || number > 65535) throw new Error(label + "必须在 0～65535 范围内");
            return number;
        }

        // 判断是否为32位字段类型。
        function is32BitFieldType(value) {
            return value === "uint32" || value === "int32" || value === "float32";
        }

        // 判断是否支持枚举字段类型。
        function supportsEnumFieldType(value) {
            return value !== "float32";
        }

        // 推导编辑器使用的数据类型。
        function editorDataType(field) {
            const requested = String(field.editor_data_type || field.data_type || "").trim().toLowerCase();
            if (requested === "high_uint8" || requested === "low_uint8") return requested;
            const parser = String(field.parser_id || "").trim().toLowerCase();
            if (parser === "scaled_high_uint8") return "high_uint8";
            if (parser === "scaled_low_uint8") return "low_uint8";
            if (parser === "bit_uint16") return "bool";
            if (["bool", "uint16", "int16", "uint32", "int32", "float32"].includes(requested)) return requested;
            return "uint16";
        }

        // 返回字段数据类型对应的解析器标识。
        function parserForFieldType(dataType) {
            switch (dataType) {
            case "int16": return "scaled_int16";
            case "uint32": return "scaled_uint32";
            case "int32": return "scaled_int32";
            case "float32": return "scaled_float32";
            case "high_uint8": return "scaled_high_uint8";
            case "low_uint8": return "scaled_low_uint8";
            case "bool": return "bit_uint16";
            default: return "scaled_uint16";
            }
        }

        // 将编辑器数据类型转换为协议数据类型。
        function protocolDataType(dataType) {
            return dataType === "high_uint8" || dataType === "low_uint8" ? "uint16" : dataType;
        }

        // 返回枚举字段允许的数值范围。
        function enumValueRange(dataType) {
            switch (dataType) {
            case "bool": return [0, 1];
            case "high_uint8":
            case "low_uint8": return [0, 255];
            case "uint16": return [0, 65535];
            case "int16": return [-32768, 32767];
            case "uint32": return [0, 4294967295];
            case "int32": return [-2147483648, 2147483647];
            default: return null;
            }
        }

        // 返回字段类型占用的寄存器数量。
        function registerCountForFieldType(dataType) {
            return is32BitFieldType(dataType) ? 2 : 1;
        }

        // 根据字段配置推导数据字节序。
        function dataOrderFromField(field) {
            const direct = String(field.data_order || "").trim().toUpperCase();
            if (dataOrders[direct]) return direct;
            const byteOrder = String(field.byte_order || "big_endian").trim().toLowerCase();
            const wordOrder = String(field.word_order || "high_word_first").trim().toLowerCase();
            return Object.keys(dataOrders).find(function (key) {
                return dataOrders[key].byteOrder === byteOrder && dataOrders[key].wordOrder === wordOrder;
            }) || "ABCD";
        }

        // 返回字段类型的中文名称。
        function fieldTypeText(dataType) {
            switch (dataType) {
            case "high_uint8": return "高 8 位";
            case "low_uint8": return "低 8 位";
            case "bool": return "状态位（单比特）";
            default: return dataType;
            }
        }

        // 规范化字段。
        function normalizeField(value) {
            const field = value || {};
            const fieldType = editorDataType(field);
            const dataOrder = is32BitFieldType(fieldType) ? dataOrderFromField(field) : "ABCD";
            const order = dataOrders[dataOrder];
            const invalidRuleType = normalizeInvalidRuleType(field.invalid_rule_type);
            const invalidRuleValue = Number(field.invalid_rule_value);
            const invalidRuleMin = Number(field.invalid_rule_min);
            const invalidRuleMax = Number(field.invalid_rule_max);
            const enumItems = Array.isArray(field.enum_items) ? field.enum_items.map(function (item, index) {
                return {
                    value: Number(item.value),
                    label: String(item.label || "").trim(),
                    sort_order: Number.isInteger(Number(item.sort_order)) ? Number(item.sort_order) : index
                };
            }).sort(function (left, right) {
                return left.sort_order - right.sort_order || left.value - right.value;
            }).map(function (item, index) { item.sort_order = index; return item; }) : [];
            const canonicalNumeric = fieldType === "bool" || enumItems.length > 0;
            return {
                key: String(field.key || "").trim(),
                display_name: String(field.display_name || "").trim(),
                unit: String(field.unit || "").trim(),
                editor_data_type: fieldType,
                data_type: protocolDataType(fieldType),
                parser_id: parserForFieldType(fieldType),
                read_block_key: String(field.read_block_key || "").trim(),
                register_offset: Number(field.register_offset) || 0,
                register_count: registerCountForFieldType(fieldType),
                data_order: dataOrder,
                byte_order: order.byteOrder,
                word_order: order.wordOrder,
                bit_index: fieldType === "bool" ? (Number.isInteger(Number(field.bit_index)) ? Number(field.bit_index) : -1) : -1,
                enum_items: enumItems,
                scale: canonicalNumeric ? 1 : (field.scale === undefined ? 1 : Number(field.scale)),
                offset: canonicalNumeric ? 0 : (field.offset === undefined ? 0 : Number(field.offset)),
                precision: canonicalNumeric ? 0 : (Number(field.precision) || 0),
                summary: field.summary === true,
                history_enabled: field.history_enabled === true,
                show_in_realtime: field.show_in_realtime === true,
                realtime_group_id: String(field.realtime_group_id || "").trim(),
                invalid_rule_type: invalidRuleType,
                invalid_rule_value: Number.isFinite(invalidRuleValue) ? invalidRuleValue : 0,
                invalid_rule_min: Number.isFinite(invalidRuleMin) ? invalidRuleMin : 0,
                invalid_rule_max: Number.isFinite(invalidRuleMax) ? invalidRuleMax : 0
            };
        }

        // 更新字段范围提示。
        function updateFieldRangeHint(targetForm) {
            const dataType = targetForm.querySelector('[name="data_type"]');
            const registerOffset = targetForm.querySelector('[name="register_offset"]');
            const rangeHint = targetForm.querySelector("[data-template-field-range]");
            if (!dataType || !registerOffset || !rangeHint) return;
            const count = registerCountForFieldType(dataType.value);
            const start = numberValue(registerOffset, 0);
            const end = start + count - 1;
            const selectedBlock = blockByKey(fieldBlockSelect.value);
            const blockRegisterCount = selectedBlock ? Number(selectedBlock.register_count) : 0;
            const valid = Number.isInteger(start) && start >= 0 && end < blockRegisterCount;
            registerOffset.max = String(Math.max(0, blockRegisterCount - count));
            registerOffset.setAttribute("aria-invalid", valid ? "false" : "true");
            rangeHint.classList.toggle("is-error", !valid);
            rangeHint.textContent = (selectedBlock ? selectedBlock.display_name + "内" : "") + (count === 2
                ? "占用偏移 " + start + "～" + end
                : "占用偏移 " + start);
            if (!valid) {
                rangeHint.textContent += blockRegisterCount > 0
                    ? "；已超出所属读取区块范围 0～" + (blockRegisterCount - 1)
                    : "；请先选择读取区块";
            }
        }

        // 同步字段区块控制。
        function syncFieldBlockControl(selectedKey) {
            const preferred = String(selectedKey || fieldBlockSelect.value || "").trim();
            fieldBlockSelect.replaceChildren();
            const placeholder = document.createElement("option");
            placeholder.value = "";
            placeholder.textContent = "请选择读取区块";
            placeholder.disabled = true;
            fieldBlockSelect.appendChild(placeholder);
            templateReadBlocks.forEach(function (block) {
                const option = document.createElement("option");
                option.value = block.block_key;
                option.textContent = block.display_name + "（FC0" + block.function_code + "，偏移 " +
                    block.start_offset + "～" + (blockEnd(block) - 1) + "）";
                fieldBlockSelect.appendChild(option);
            });
            const hasPreferred = templateReadBlocks.some(function (block) { return block.block_key === preferred; });
            fieldBlockSelect.value = hasPreferred ? preferred : "";
            fieldBlockField.hidden = false;
            fieldBlockSelect.required = true;
            updateFieldRangeHint(fieldForm);
        }

        // 同步字段类型控件。
        function syncFieldTypeControls(targetForm) {
            const dataType = targetForm.querySelector('[name="data_type"]');
            const dataOrder = targetForm.querySelector('[name="data_order"]');
            const dataOrderField = targetForm.querySelector("[data-template-data-order]");
            if (!dataType || !dataOrder || !dataOrderField) return;
            const uses32Bits = is32BitFieldType(dataType.value);
            dataOrderField.hidden = !uses32Bits;
            dataOrder.disabled = !uses32Bits;
            if (!uses32Bits) dataOrder.value = "ABCD";
            const isBit = dataType.value === "bool";
            const bitIndexField = targetForm.querySelector("[data-template-bit-index]");
            if (bitIndexField) bitIndexField.hidden = !isBit;
            if (targetForm.elements.bit_index) targetForm.elements.bit_index.disabled = !isBit;
            const enumToggle = targetForm.elements.enum_enabled;
            const enumSupported = supportsEnumFieldType(dataType.value);
            if (enumToggle) {
                enumToggle.disabled = !enumSupported;
                if (!enumSupported) enumToggle.checked = false;
            }
            syncEnumControls(targetForm);
            updateFieldRangeHint(targetForm);
            syncInvalidRuleControls(targetForm);
        }

        // 同步枚举控件。
        function syncEnumControls(targetForm) {
            const dataType = targetForm.elements.data_type ? targetForm.elements.data_type.value : "";
            const toggle = targetForm.elements.enum_enabled;
            const enabled = Boolean(toggle && !toggle.disabled && toggle.checked);
            const editor = targetForm.querySelector("[data-template-enum-editor]");
            if (editor) {
                editor.hidden = !enabled;
                editor.querySelectorAll("input,button").forEach(function (control) {
                    control.disabled = !enabled;
                });
            }
            const canonicalNumeric = dataType === "bool" || enabled;
            targetForm.querySelectorAll("[data-template-numeric-transform]").forEach(function (container) {
                container.hidden = canonicalNumeric;
                const input = container.querySelector("input,select");
                if (input) input.disabled = canonicalNumeric;
            });
            if (canonicalNumeric) {
                targetForm.elements.scale.value = "1";
                targetForm.elements.offset.value = "0";
                targetForm.elements.precision.value = "0";
            }
            const hint = targetForm.querySelector("[data-template-enum-hint]");
            if (hint) hint.textContent = supportsEnumFieldType(dataType)
                ? "仅改变实时与 MQTT 的展示文字，历史、告警和北向仍使用数值。"
                : "float32 不支持枚举显示。";
        }

        // 渲染字段枚举值列表。
        function renderEnumItems() {
            const container = fieldForm.querySelector("[data-template-enum-items]");
            if (!container) return;
            container.replaceChildren();
            currentEnumItems.forEach(function (item, index) {
                item.sort_order = index;
                const row = document.createElement("div");
                row.className = "template-enum-row";
                const valueInput = document.createElement("input");
                valueInput.type = "number";
                valueInput.step = "1";
                valueInput.required = true;
                valueInput.value = String(item.value);
                valueInput.setAttribute("aria-label", "枚举数值");
                pageScope.listen(valueInput, "input", function () { item.value = Number(valueInput.value); });
                const labelInput = document.createElement("input");
                labelInput.type = "text";
                labelInput.maxLength = 64;
                labelInput.required = true;
                labelInput.value = item.label;
                labelInput.placeholder = "例如：运行";
                labelInput.setAttribute("aria-label", "枚举显示文字");
                labelInput.setAttribute("data-keyboard", "text-cn");
                pageScope.listen(labelInput, "input", function () { item.label = labelInput.value; });
                const actions = document.createElement("div");
                actions.className = "template-enum-actions";
                [
                    ["上移", index > 0, -1],
                    ["下移", index + 1 < currentEnumItems.length, 1]
                ].forEach(function (definition) {
                    const button = document.createElement("button");
                    button.type = "button";
                    button.className = "btn btn-small btn-secondary";
                    button.textContent = definition[0];
                    button.disabled = !definition[1];
                    pageScope.listen(button, "click", function () {
                        const target = index + definition[2];
                        const moved = currentEnumItems[index];
                        currentEnumItems[index] = currentEnumItems[target];
                        currentEnumItems[target] = moved;
                        renderEnumItems();
                    });
                    actions.appendChild(button);
                });
                const remove = document.createElement("button");
                remove.type = "button";
                remove.className = "btn btn-small btn-danger";
                remove.textContent = "删除";
                pageScope.listen(remove, "click", function () {
                    currentEnumItems.splice(index, 1);
                    renderEnumItems();
                });
                actions.appendChild(remove);
                row.append(valueInput, labelInput, actions);
                container.appendChild(row);
            });
        }

        // 校验枚举值的范围、唯一性和显示文本。
        function validateEnumItems(dataType, enabled) {
            if (!enabled) return [];
            if (!supportsEnumFieldType(dataType)) throw new Error("float32 不支持枚举显示");
            if (!currentEnumItems.length) throw new Error("启用枚举显示后至少需要 1 个枚举项");
            if (currentEnumItems.length > 32) throw new Error("单字段枚举项不能超过 32 条");
            const range = enumValueRange(dataType);
            const values = new Set();
            return currentEnumItems.map(function (item, index) {
                const value = Number(item.value);
                const label = String(item.label || "").trim();
                if (!Number.isSafeInteger(value)) throw new Error("第 " + (index + 1) + " 个枚举值必须是整数");
                if (!range || value < range[0] || value > range[1]) throw new Error("第 " + (index + 1) + " 个枚举值超出当前数据类型范围");
                if (values.has(value)) throw new Error("枚举值不能重复：" + value);
                if (!label) throw new Error("第 " + (index + 1) + " 个枚举显示文字不能为空");
                values.add(value);
                return { value: value, label: label, sort_order: index };
            });
        }

        // 根据无效值规则类型同步相关表单控件。
        function syncInvalidRuleControls(targetForm) {
            const ruleSelect = targetForm.elements.invalid_rule_type;
            if (!ruleSelect) return;
            const ruleType = normalizeInvalidRuleType(ruleSelect.value);
            ruleSelect.value = ruleType;
            const usesValue = ruleType === "equal" || ruleType === "greater_or_equal" || ruleType === "less_or_equal";
            const usesRange = ruleType === "inside_range";
            [
                ["[data-template-invalid-value-field]", targetForm.elements.invalid_rule_value, usesValue],
                ["[data-template-invalid-min-field]", targetForm.elements.invalid_rule_min, usesRange],
                ["[data-template-invalid-max-field]", targetForm.elements.invalid_rule_max, usesRange]
            ].forEach(function (entry) {
                const container = targetForm.querySelector(entry[0]);
                const control = entry[1];
                const visible = entry[2];
                if (container) container.hidden = !visible;
                if (control) {
                    control.disabled = !visible;
                    control.required = visible;
                }
            });
            const dataType = targetForm.elements.data_type ? targetForm.elements.data_type.value : "";
            const shortcut = targetForm.querySelector("[data-template-uint16-invalid-shortcut]");
            if (shortcut) shortcut.hidden = dataType !== "uint16";
        }

        // 向编辑器表格行追加文本单元格。
        function appendTextCell(row, className, text, label) {
            const cell = document.createElement("div");
            cell.className = "template-field-list-cell " + className;
            cell.dataset.label = label;
            cell.textContent = text;
            row.appendChild(cell);
            return cell;
        }

        // 按标识查找实时显示分组。
        function groupByID(id) {
            return realtimeGroups.find(function (group) { return group.id === id; }) || null;
        }

        // 生成未被占用的实时显示分组标识。
        function generateGroupID() {
            let sequence = Date.now().toString(36);
            const ids = new Set(realtimeGroups.map(function (group) { return group.id; }));
            let candidate = "group_" + sequence;
            let suffix = 1;
            while (ids.has(candidate)) {
                candidate = "group_" + sequence + "_" + suffix;
                suffix += 1;
            }
            return candidate;
        }

        // 同步字段所属实时分组的选择控件。
        function syncFieldGroupControl(selectedID) {
            const container = fieldForm.querySelector("[data-template-realtime-group-field]");
            const select = fieldForm.elements.realtime_group_id;
            if (!container || !select) return;
            const groupingEnabled = form.elements.realtime_grouping_enabled.checked;
            const realtimeVisible = fieldForm.elements.show_in_realtime.checked;
            container.hidden = !(groupingEnabled && realtimeVisible);
            select.disabled = container.hidden;
            select.required = !container.hidden;
            const preferred = String(selectedID === undefined ? select.value : selectedID || "");
            select.replaceChildren();
            const placeholder = document.createElement("option");
            placeholder.value = "";
            placeholder.textContent = "请选择实时展示分组";
            select.appendChild(placeholder);
            realtimeGroups.forEach(function (group) {
                const option = document.createElement("option");
                option.value = group.id;
                option.textContent = group.name || "未命名分组";
                select.appendChild(option);
            });
            select.value = groupByID(preferred) ? preferred : "";
        }

        // 渲染实时显示分组列表。
        function renderRealtimeGroups() {
            const enabled = form.elements.realtime_grouping_enabled.checked;
            const manager = form.querySelector("[data-template-group-manager]");
            const list = form.querySelector("[data-template-group-list]");
            const empty = form.querySelector("[data-template-groups-empty]");
            if (manager) manager.hidden = !enabled;
            if (!list) return;
            list.replaceChildren();
            realtimeGroups.forEach(function (group, index) {
                group.order = index;
                const count = templateFields.filter(function (field) { return field.realtime_group_id === group.id; }).length;
                const row = document.createElement("div");
                row.className = "template-group-row";
                const identity = document.createElement("div");
                identity.className = "template-group-identity";
                const input = document.createElement("input");
                input.type = "text";
                input.maxLength = 100;
                input.required = true;
                input.value = group.name;
                input.setAttribute("aria-label", "分组名称");
                input.setAttribute("data-keyboard", "text-cn");
                pageScope.listen(input, "input", function () {
                    group.name = input.value;
                    dirty = true;
                    syncFieldGroupControl();
                });
                const meta = document.createElement("small");
                meta.textContent = count + " 个数据项";
                identity.append(input, meta);
                const actions = document.createElement("div");
                actions.className = "template-group-actions";
                [["上移", -1], ["下移", 1]].forEach(function (definition) {
                    const button = document.createElement("button");
                    button.type = "button";
                    button.className = "btn btn-small btn-secondary";
                    button.textContent = definition[0];
                    const target = index + definition[1];
                    button.disabled = target < 0 || target >= realtimeGroups.length;
                    pageScope.listen(button, "click", function () {
                        if (button.disabled) return;
                        const moved = realtimeGroups[index];
                        realtimeGroups[index] = realtimeGroups[target];
                        realtimeGroups[target] = moved;
                        dirty = true;
                        renderRealtimeGroups();
                    });
                    actions.appendChild(button);
                });
                const remove = document.createElement("button");
                remove.type = "button";
                remove.className = "btn btn-small btn-danger";
                remove.textContent = "删除";
                remove.disabled = count > 0;
                remove.title = count > 0 ? "请先迁移或取消该组内数据项的分组引用" : "删除空分组";
                pageScope.listen(remove, "click", function () {
                    if (count > 0) {
                        showToast("error", "该分组仍包含数据项，请先迁移或取消分组引用");
                        return;
                    }
                    realtimeGroups.splice(index, 1);
                    dirty = true;
                    renderRealtimeGroups();
                    renderFields();
                });
                actions.appendChild(remove);
                row.append(identity, actions);
                list.appendChild(row);
            });
            if (empty) empty.hidden = realtimeGroups.length > 0;
            syncFieldGroupControl();
        }

        // 渲染设备类型读取区块列表。
        function renderReadBlocks() {
            blocksContainer.replaceChildren();
            templateReadBlocks.forEach(function (block, index) {
                block.sort_order = index;
                const row = document.createElement("div");
                row.className = "template-read-block-list-row";
                row.setAttribute("role", "row");

                const nameCell = appendTextCell(row, "template-read-block-list-name", block.display_name, "区块名称");
                nameCell.title = block.display_name;
                appendTextCell(row, "", "FC0" + block.function_code, "功能码");
                appendTextCell(row, "", block.start_offset + "～" + (blockEnd(block) - 1), "偏移范围");
                const fieldCount = templateFields.filter(function (field) { return field.read_block_key === block.block_key; }).length;
                appendTextCell(row, "", String(fieldCount), "字段数量");

                const actions = appendTextCell(row, "template-read-block-list-actions", "", "操作");
                const moveUp = document.createElement("button");
                moveUp.type = "button";
                moveUp.className = "btn btn-small btn-secondary";
                moveUp.textContent = "上移";
                moveUp.disabled = index === 0;
                pageScope.listen(moveUp, "click", function () {
                    if (index === 0) return;
                    const previous = templateReadBlocks[index - 1];
                    templateReadBlocks[index - 1] = templateReadBlocks[index];
                    templateReadBlocks[index] = previous;
                    dirty = true;
                    renderReadBlocks();
                    renderFields();
                });
                const moveDown = document.createElement("button");
                moveDown.type = "button";
                moveDown.className = "btn btn-small btn-secondary";
                moveDown.textContent = "下移";
                moveDown.disabled = index === templateReadBlocks.length - 1;
                pageScope.listen(moveDown, "click", function () {
                    if (index >= templateReadBlocks.length - 1) return;
                    const next = templateReadBlocks[index + 1];
                    templateReadBlocks[index + 1] = templateReadBlocks[index];
                    templateReadBlocks[index] = next;
                    dirty = true;
                    renderReadBlocks();
                    renderFields();
                });
                const editButton = document.createElement("button");
                editButton.type = "button";
                editButton.className = "btn btn-small btn-secondary";
                editButton.textContent = "编辑";
                pageScope.listen(editButton, "click", function () { openBlockEditor(index); });
                const deleteButton = document.createElement("button");
                deleteButton.type = "button";
                deleteButton.className = "btn btn-small btn-danger";
                deleteButton.textContent = "删除";
                deleteButton.disabled = templateReadBlocks.length <= 1;
                deleteButton.title = deleteButton.disabled ? "设备类型至少保留一个读取区块" : "删除当前读取区块";
                pageScope.listen(deleteButton, "click", async function () {
                    if (templateReadBlocks.length <= 1) return;
                    if (templateFields.some(function (field) { return field.read_block_key === block.block_key; })) {
                        const message = "该读取区块仍包含字段，请先移动或删除相关字段。";
                        setFeedback(feedback, "error", message);
                        showToast("error", message);
                        return;
                    }
                    if (!await confirmAction({
                        title: "确认删除读取区块",
                        message: "删除读取区块“" + block.display_name + "”后无法恢复，确定继续吗？",
                        confirmText: "确认删除",
                        danger: true
                    })) return;
                    templateReadBlocks.splice(index, 1);
                    dirty = true;
                    renderReadBlocks();
                    renderFields();
                    updateStrideHint();
                });
                actions.append(moveUp, moveDown, editButton, deleteButton);
                blocksContainer.appendChild(row);
            });
            updateStrideHint();
        }

        // 渲染设备类型字段列表。
        function renderFields() {
            // 重置空状态，并为每个字段创建基础信息和寄存器描述。
            fieldsContainer.replaceChildren();
            const hasFields = templateFields.length > 0;
            fieldsTable.hidden = !hasFields;
            fieldsEmpty.hidden = hasFields;
            templateFields.forEach(function (field, index) {
                const row = document.createElement("div");
                row.className = "template-field-list-row";
                row.setAttribute("role", "row");

                const identity = appendTextCell(row, "template-field-list-identity", "", "数据项");
                const sequence = document.createElement("small");
                sequence.textContent = "数据项 " + String(index + 1);
                const name = document.createElement("strong");
                name.textContent = field.display_name || "未命名数据项";
                name.title = name.textContent;
                const key = document.createElement("code");
                key.textContent = field.key || "未设置 key";
                key.title = key.textContent;
                identity.append(sequence, name, key);

                appendTextCell(row, "template-field-list-block", blockName(field.read_block_key), "读取区块");
                appendTextCell(row, "", field.unit || "-", "单位");
                const registerEnd = field.register_offset + field.register_count - 1;
                const registerRange = registerEnd > field.register_offset
                    ? field.register_offset + "～" + registerEnd
                    : String(field.register_offset);
                appendTextCell(row, "template-field-list-offset", registerRange, "寄存器范围");
                const typeAndOrder = fieldTypeText(field.editor_data_type) +
                    (is32BitFieldType(field.editor_data_type) ? " / " + field.data_order : "") +
                    (field.editor_data_type === "bool" ? " bit" + field.bit_index : "");
                const typeCell = appendTextCell(row, "template-field-list-parser", typeAndOrder, "数据类型 / 排列");
                typeCell.title = typeCell.textContent;

                // 汇总关键字段、历史、实时分组、无效规则和枚举状态。
                const states = appendTextCell(row, "template-field-list-states", "", "状态 / 无效规则");
                const summaryState = document.createElement("span");
                summaryState.className = field.summary ? "template-list-state is-on" : "template-list-state";
                summaryState.textContent = field.summary ? "关键数据" : "非关键数据";
                const historyState = document.createElement("span");
                historyState.className = field.history_enabled ? "template-list-state is-on" : "template-list-state";
                historyState.textContent = field.history_enabled ? "历史记录" : "不记录历史";
                const realtimeState = document.createElement("span");
                realtimeState.className = field.show_in_realtime ? "template-list-state is-on" : "template-list-state";
                realtimeState.textContent = field.show_in_realtime ? "实时展示" : "不实时展示";
                const groupState = document.createElement("span");
                const fieldGroup = groupByID(field.realtime_group_id);
                groupState.className = fieldGroup ? "template-list-state is-on" : "template-list-state";
                groupState.textContent = field.show_in_realtime
                    ? (fieldGroup ? "分组：" + fieldGroup.name : "未分配分组")
                    : "无展示分组";
                const invalidState = document.createElement("span");
                invalidState.className = field.invalid_rule_type === "none" ? "template-list-state" : "template-list-state is-on template-invalid-rule-state";
                invalidState.textContent = invalidRuleSummary(field);
                states.append(summaryState, historyState, realtimeState);
                if (form.elements.realtime_grouping_enabled.checked) {
                    states.appendChild(groupState);
                }
                states.appendChild(invalidState);
                if (field.enum_items.length > 0) {
                    const enumState = document.createElement("span");
                    enumState.className = "template-list-state is-on";
                    enumState.textContent = field.enum_items.length <= 2
                        ? field.enum_items.map(function (item) { return item.value + "=" + item.label; }).join("，")
                        : field.enum_items.length + " 个枚举项";
                    states.appendChild(enumState);
                }

                // 绑定字段编辑和删除操作。
                const actions = appendTextCell(row, "template-field-list-actions", "", "操作");
                const editButton = document.createElement("button");
                editButton.type = "button";
                editButton.className = "btn btn-small btn-secondary";
                editButton.textContent = "编辑";
                pageScope.listen(editButton, "click", function () { openFieldEditor(index); });
                const deleteButton = document.createElement("button");
                deleteButton.type = "button";
                deleteButton.className = "btn btn-small btn-danger";
                deleteButton.textContent = "删除";
                deleteButton.disabled = templateFields.length <= 1;
                deleteButton.title = deleteButton.disabled ? "至少保留一个数据项" : "删除当前数据项";
                pageScope.listen(deleteButton, "click", function () {
                    if (templateFields.length <= 1) return;
                    templateFields.splice(index, 1);
                    dirty = true;
                    renderFields();
                    renderReadBlocks();
                });
                actions.append(editButton, deleteButton);
                fieldsContainer.appendChild(row);
            });
        }

        // 设置字段编辑表单中的单个值。
        function setFieldFormValue(name, value) {
            const control = fieldForm.elements[name];
            if (control) control.value = value === undefined || value === null ? "" : value;
        }

        // 更新读取区块地址范围提示。
        function updateBlockRangeHint() {
            const start = numberValue(blockForm.elements.block_start_offset, 0);
            const count = numberValue(blockForm.elements.read_block_quantity, 0);
            const end = start + count;
            const hint = blockForm.querySelector("[data-template-block-range]");
            const stride = numberValue(form.elements.device_address_stride, 0);
            const valid = Number.isInteger(start) && start >= 0 && Number.isInteger(count) && count >= 1 && count <= 125 &&
                end <= stride && end <= 65536;
            if (hint) {
                hint.textContent = count > 0 ? "设备内偏移范围：" + start + "～" + (end - 1) : "请输入寄存器数量";
                if (end > stride) hint.textContent += "；超出当前设备地址跨度 " + stride;
                hint.classList.toggle("is-error", !valid);
            }
        }

        // 判断指定编辑弹窗是否处于打开状态。
        function editorModalOpen(panel) {
            return panel.classList.contains("open");
        }

        // 切换指定编辑弹窗的打开状态。
        function setEditorModalOpen(panel, open) {
            if (window.EdgeVirtualKeyboard) {
                window.EdgeVirtualKeyboard.hide();
            }
            panel.querySelectorAll("input,select,textarea,button").forEach(function (control) {
                control.disabled = !open;
            });
            if (open) {
                panel._pageScrollY = window.scrollY;
                panel._returnFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null;
                panel.classList.add("open");
                panel.setAttribute("aria-hidden", "false");
                document.body.classList.add("modal-open");
                const focusTarget = panel.querySelector("input:not([type='hidden']),select,textarea,button");
                if (focusTarget) pageScope.setTimeout(function () { focusTarget.focus(); }, 0);
                return;
            }
            const scrollY = Number.isFinite(panel._pageScrollY) ? panel._pageScrollY : window.scrollY;
            const returnFocus = panel._returnFocus;
            panel._pageScrollY = null;
            panel._returnFocus = null;
            panel.classList.remove("open");
            panel.setAttribute("aria-hidden", "true");
            if (!document.querySelector(".channel-modal.open")) document.body.classList.remove("modal-open");
            window.requestAnimationFrame(function () {
                window.scrollTo({ top: scrollY, left: 0, behavior: "auto" });
                if (returnFocus && returnFocus.isConnected) returnFocus.focus({ preventScroll: true });
            });
        }

        // 打开读取区块编辑器并填充草稿。
        function openBlockEditor(index) {
            editingBlockIndex = Number.isInteger(index) ? index : -1;
            const block = editingBlockIndex >= 0 ? templateReadBlocks[editingBlockIndex] : normalizeReadBlock({
                block_key: generateBlockKey(),
                display_name: "",
                function_code: 3,
                start_offset: 0,
                register_count: 1,
                sort_order: templateReadBlocks.length
            }, templateReadBlocks.length);
            blockForm.reset();
            setFeedback(blockFeedback, "", "");
            blockTitle.textContent = editingBlockIndex >= 0 ? "编辑读取区块" : "添加读取区块";
            blockForm.elements.block_display_name.value = block.display_name;
            blockForm.elements.block_function_code.value = String(block.function_code);
            blockForm.elements.block_start_offset.value = String(block.start_offset);
            blockForm.elements.read_block_quantity.value = String(block.register_count);
            updateBlockRangeHint();
            setEditorModalOpen(blockPanel, true);
        }

        // 打开字段编辑器并填充草稿。
        function openFieldEditor(index) {
            editingFieldIndex = Number.isInteger(index) ? index : -1;
            const value = editingFieldIndex >= 0 ? templateFields[editingFieldIndex] : normalizeField({});
            fieldForm.reset();
            setFeedback(fieldFeedback, "", "");
            fieldTitle.textContent = editingFieldIndex >= 0 ? "编辑数据项" : "添加数据项";
            setFieldFormValue("key", value.key);
            setFieldFormValue("field_display_name", value.display_name);
            setFieldFormValue("unit", value.unit);
            setFieldFormValue("data_type", value.editor_data_type);
            setFieldFormValue("data_order", value.data_order);
            syncFieldBlockControl(value.read_block_key);
            setFieldFormValue("register_offset", value.register_offset);
            setFieldFormValue("bit_index", value.bit_index);
            setFieldFormValue("scale", value.scale);
            setFieldFormValue("offset", value.offset);
            setFieldFormValue("precision", value.precision);
            setFieldFormValue("invalid_rule_type", value.invalid_rule_type);
            setFieldFormValue("invalid_rule_value", value.invalid_rule_value);
            setFieldFormValue("invalid_rule_min", value.invalid_rule_min);
            setFieldFormValue("invalid_rule_max", value.invalid_rule_max);
            fieldForm.elements.summary.checked = value.summary;
            fieldForm.elements.history_enabled.checked = value.history_enabled;
            fieldForm.elements.show_in_realtime.checked = value.show_in_realtime;
            syncFieldGroupControl(value.realtime_group_id);
            currentEnumItems = value.enum_items.map(function (item) { return Object.assign({}, item); });
            fieldForm.elements.enum_enabled.checked = currentEnumItems.length > 0;
            renderEnumItems();
            syncFieldTypeControls(fieldForm);
            const advanced = fieldForm.querySelector(".template-advanced-settings");
            if (advanced) advanced.open = value.invalid_rule_type !== "none" || currentEnumItems.length > 0;
            setEditorModalOpen(fieldPanel, true);
        }

        // 打开设备类型编辑器并载入定义。
        function openEditor(item) {
            form.reset();
            setFeedback(feedback, "", "");
            editingID = item ? String(item.id || "") : "";
            submitButton.textContent = editingID ? "保存修改" : "创建设备类型";
            form.elements.id.value = editingID;
            form.elements.id.readOnly = editingID !== "";
            form.elements.display_name.value = item ? item.display_name || "" : "";
            form.elements.description.value = item ? item.description || "" : "";
            form.elements.default_start_register.value = item ? item.default_start_register : 0;
            const blocks = item
                ? (Array.isArray(item.read_blocks) ? item.read_blocks.slice().sort(function (left, right) {
                    return Number(left.sort_order) - Number(right.sort_order) || String(left.block_key).localeCompare(String(right.block_key));
                }) : [])
                : [{ block_key: "default", display_name: "默认读取区块", function_code: 3, start_offset: 0, register_count: 1, sort_order: 0 }];
            templateReadBlocks = blocks.map(normalizeReadBlock);
            form.elements.device_address_stride.value = item ? item.device_address_stride : 1;
            const fields = item && Array.isArray(item.fields) ? item.fields : [];
            templateFields = fields.map(normalizeField);
            renderReadBlocks();
            renderFields();
            form.elements.realtime_grouping_enabled.checked = Boolean(item && item.realtime_grouping_enabled);
            realtimeGroups = item && Array.isArray(item.realtime_groups) ? item.realtime_groups.map(function (group, index) {
                return {
                    id: String(group.id || group.group_id || "").trim(),
                    name: String(group.name || group.display_name || "").trim(),
                    order: Number.isInteger(Number(group.order)) ? Number(group.order) : index
                };
            }).sort(function (left, right) { return left.order - right.order || left.id.localeCompare(right.id); }) : [];
            renderRealtimeGroups();
            setEditorModalOpen(blockPanel, false);
            setEditorModalOpen(fieldPanel, false);
        }

        // 从编辑草稿构造完整设备类型请求。
        function buildPayload() {
            // 校验设备类型基础信息及地址跨度。
            if (!form.reportValidity()) throw new Error("请完整填写所有必填项，并检查输入格式");
            const id = form.elements.id.value.trim();
            if (!/^[a-z0-9_-]+$/.test(id)) throw new Error("模板 ID 仅允许小写字母、数字、下划线和短横线");
            if (!templateReadBlocks.length) throw new Error("设备类型至少需要 1 个读取区块");
            const stride = numberValue(form.elements.device_address_stride, 0);
            if (!Number.isInteger(stride) || stride <= 0 || stride > 65536) throw new Error("设备地址跨度必须为 1～65536 的整数");
            // 规范化读取区块，并检查标识、范围和功能码。
            const blockKeys = new Set();
            const readBlocks = templateReadBlocks.map(function (block, index) {
                const blockKey = String(block.block_key || "").trim();
                const displayName = String(block.display_name || "").trim();
                const functionCode = Number(block.function_code);
                const startOffset = Number(block.start_offset);
                const registerCount = Number(block.register_count);
                if (!blockKey || !/^[a-z0-9_-]+$/.test(blockKey)) throw new Error("第 " + (index + 1) + " 个读取区块内部标识无效，请删除后重新添加");
                if (blockKeys.has(blockKey)) throw new Error("读取区块内部标识重复，请删除重复区块后重新添加");
                blockKeys.add(blockKey);
                if (!displayName) throw new Error("第 " + (index + 1) + " 个读取区块名称不能为空");
                if (functionCode !== 3 && functionCode !== 4) throw new Error("第 " + (index + 1) + " 个读取区块功能码仅支持 FC03 或 FC04");
                if (!Number.isInteger(startOffset) || startOffset < 0 || startOffset > 65535) throw new Error("第 " + (index + 1) + " 个读取区块起始偏移不合法");
                if (!Number.isInteger(registerCount) || registerCount < 1 || registerCount > 125) throw new Error("第 " + (index + 1) + " 个读取区块寄存器数量必须为 1～125");
                if (startOffset + registerCount > 65536) throw new Error("第 " + (index + 1) + " 个读取区块超出 Modbus 地址空间");
                if (startOffset + registerCount > stride) throw new Error("第 " + (index + 1) + " 个读取区块超出设备地址跨度；当前最小允许值为 " + minimumStride());
                return {
                    block_key: blockKey,
                    display_name: displayName,
                    function_code: functionCode,
                    start_offset: startOffset,
                    register_count: registerCount,
                    sort_order: index
                };
            });
            for (let leftIndex = 0; leftIndex < readBlocks.length; leftIndex += 1) {
                for (let rightIndex = leftIndex + 1; rightIndex < readBlocks.length; rightIndex += 1) {
                    const left = readBlocks[leftIndex];
                    const right = readBlocks[rightIndex];
                    if (left.function_code !== right.function_code) continue;
                    const overlaps = left.start_offset < right.start_offset + right.register_count &&
                        right.start_offset < left.start_offset + left.register_count;
                    if (overlaps) throw new Error("相同功能码读取区块地址重叠：“" + left.display_name + "”与“" + right.display_name + "”");
                }
            }
            // 校验实时展示分组并生成稳定排序。
            const groupingEnabled = form.elements.realtime_grouping_enabled.checked;
            if (groupingEnabled && !realtimeGroups.length) throw new Error("开启实时展示分组后至少需要创建 1 个分组");
            const groupIDs = new Set();
            const groups = realtimeGroups.map(function (group, index) {
                const id = String(group.id || "").trim();
                const name = String(group.name || "").trim();
                if (!/^[a-z0-9_-]+$/.test(id) || groupIDs.has(id)) throw new Error("第 " + (index + 1) + " 个实时展示分组内部 ID 无效或重复");
                if (!name) throw new Error("第 " + (index + 1) + " 个实时展示分组名称不能为空");
                groupIDs.add(id);
                return { id: id, name: name, order: index };
            });
            // 转换字段类型、解析器、无效规则和展示属性。
            if (!templateFields.length) throw new Error("至少需要添加 1 个数据项");
            const keys = new Set();
            const fields = templateFields.map(function (field, index) {
                const key = field.key.trim();
                if (!/^[a-z0-9_-]+$/.test(key)) throw new Error("第 " + (index + 1) + " 个数据项 key 格式不正确");
                if (keys.has(key)) throw new Error("数据项 key 不能重复：" + key);
                keys.add(key);
                const fieldRegisterCount = registerCountForFieldType(field.editor_data_type);
                const readBlock = readBlocks.find(function (block) { return block.block_key === field.read_block_key; });
                if (!readBlock) throw new Error("第 " + (index + 1) + " 个数据项引用的读取区块不存在");
                const realtimeGroupID = String(field.realtime_group_id || "").trim();
                if (realtimeGroupID && !groupIDs.has(realtimeGroupID)) throw new Error("第 " + (index + 1) + " 个数据项引用的实时展示分组不存在");
                if (groupingEnabled && field.show_in_realtime && !realtimeGroupID) throw new Error("实时展示数据项“" + (field.display_name || key) + "”必须选择展示分组");
                if (!Number.isInteger(field.register_offset) || field.register_offset < 0 || field.register_offset + fieldRegisterCount > readBlock.register_count) {
                    throw new Error("第 " + (index + 1) + " 个数据项超出所属读取区块范围");
                }
                const dataOrder = is32BitFieldType(field.editor_data_type) ? field.data_order : "ABCD";
                const order = dataOrders[dataOrder] || dataOrders.ABCD;
                let invalidRule;
                try {
                    invalidRule = validateInvalidRule(
                        field.invalid_rule_type,
                        field.invalid_rule_value,
                        field.invalid_rule_min,
                        field.invalid_rule_max
                    );
                } catch (error) {
                    throw new Error("第 " + (index + 1) + " 个数据项：" + error.message);
                }
                return {
                    key: key,
                    display_name: field.display_name,
                    unit: field.unit,
                    data_type: protocolDataType(field.editor_data_type),
                    parser_id: parserForFieldType(field.editor_data_type),
                    read_block_key: readBlock.block_key,
                    register_offset: field.register_offset,
                    register_count: fieldRegisterCount,
                    byte_order: order.byteOrder,
                    word_order: order.wordOrder,
                    bit_index: field.editor_data_type === "bool" ? field.bit_index : -1,
                    enum_items: field.enum_items,
                    scale: field.editor_data_type === "bool" || field.enum_items.length > 0 ? 1 : field.scale,
                    offset: field.editor_data_type === "bool" || field.enum_items.length > 0 ? 0 : field.offset,
                    precision: field.editor_data_type === "bool" || field.enum_items.length > 0 ? 0 : field.precision,
                    summary: field.summary,
                    history_enabled: field.history_enabled,
                    show_in_realtime: field.show_in_realtime,
                    realtime_group_id: realtimeGroupID,
                    display_order: index + 1,
                    invalid_rule_type: invalidRule.type,
                    invalid_rule_value: invalidRule.value,
                    invalid_rule_min: invalidRule.minimum,
                    invalid_rule_max: invalidRule.maximum
                };
            });
            // 同一区块中的字段不可重叠，但允许不同位或高低字节共享寄存器。
            for (let leftIndex = 0; leftIndex < fields.length; leftIndex += 1) {
                for (let rightIndex = leftIndex + 1; rightIndex < fields.length; rightIndex += 1) {
                    const left = fields[leftIndex];
                    const right = fields[rightIndex];
                    if (left.read_block_key !== right.read_block_key) continue;
                    const overlaps = left.register_offset < right.register_offset + right.register_count &&
                        right.register_offset < left.register_offset + left.register_count;
                    if (!overlaps) continue;
                    const sameRegister = left.register_count === 1 && right.register_count === 1 &&
                        left.register_offset === right.register_offset;
                    const distinctBits = sameRegister && left.data_type === "bool" && right.data_type === "bool" &&
                        left.bit_index !== right.bit_index;
                    const bytePair = sameRegister && new Set([left.parser_id, right.parser_id]).size === 2 &&
                        [left.parser_id, right.parser_id].every(function (parser) {
                            return parser === "scaled_high_uint8" || parser === "scaled_low_uint8";
                        });
                    if (!distinctBits && !bytePair) {
                        throw new Error("数据项“" + left.display_name + "”与“" + right.display_name + "”寄存器范围重叠");
                    }
                }
            }
            if (!fields.some(function (field) { return field.summary; })) throw new Error("至少需要 1 个关键数据项，用于设备摘要、告警候选及 MQTT 实时数据发布");
            // 汇总为后端设备类型接口所需的完整请求对象。
            return {
                id: id,
                display_name: form.elements.display_name.value.trim(),
                description: form.elements.description.value.trim(),
                default_start_register: numberValue(form.elements.default_start_register, 0),
                device_address_stride: stride,
                read_blocks: readBlocks,
                builtin: false,
                fields: fields,
                realtime_grouping_enabled: groupingEnabled,
                realtime_groups: groups
            };
        }

        pageScope.listen(form.querySelector("[data-template-block-add]"), "click", function () { openBlockEditor(-1); });
        pageScope.listen(form.querySelector("[data-template-field-add]"), "click", function () { openFieldEditor(-1); });
        pageScope.listen(fieldForm.querySelector('[name="data_type"]'), "change", function () { syncFieldTypeControls(fieldForm); });
        pageScope.listen(fieldForm.elements.enum_enabled, "change", function () {
            renderEnumItems();
            syncEnumControls(fieldForm);
        });
        pageScope.listen(fieldForm.querySelector("[data-template-enum-add]"), "click", function () {
            if (currentEnumItems.length >= 32) {
                setFeedback(fieldFeedback, "error", "单字段枚举项不能超过 32 条");
                return;
            }
            const dataType = fieldForm.elements.data_type.value;
            const range = enumValueRange(dataType) || [0, 0];
            let value = range[0] <= 0 && range[1] >= 0 ? 0 : range[0];
            const used = new Set(currentEnumItems.map(function (item) { return Number(item.value); }));
            while (used.has(value) && value < range[1]) value += 1;
            currentEnumItems.push({ value: value, label: "", sort_order: currentEnumItems.length });
            renderEnumItems();
        });
        pageScope.listen(fieldForm.elements.invalid_rule_type, "change", function () { syncInvalidRuleControls(fieldForm); });
        pageScope.listen(fieldForm.elements.show_in_realtime, "change", function () { syncFieldGroupControl(); });
        pageScope.listen(fieldForm.querySelector("[data-template-invalid-uint16-shortcut]"), "click", function () {
            fieldForm.elements.invalid_rule_type.value = "greater_or_equal";
            fieldForm.elements.invalid_rule_value.value = "65520";
            syncInvalidRuleControls(fieldForm);
        });
        pageScope.listen(fieldForm.querySelector('[name="register_offset"]'), "input", function () { updateFieldRangeHint(fieldForm); });
        pageScope.listen(fieldBlockSelect, "change", function () { updateFieldRangeHint(fieldForm); });
        pageScope.listen(form.elements.device_address_stride, "input", function () {
            updateStrideHint();
            updateBlockRangeHint();
        });
        pageScope.listen(blockForm.elements.block_start_offset, "input", updateBlockRangeHint);
        pageScope.listen(blockForm.elements.read_block_quantity, "input", updateBlockRangeHint);
            pageScope.listen(document, "keydown", function (event) {
            if (event.key === "Escape" && editorModalOpen(fieldPanel)) {
                event.preventDefault();
                event.stopImmediatePropagation();
                setEditorModalOpen(fieldPanel, false);
            } else if (event.key === "Escape" && editorModalOpen(blockPanel)) {
                event.preventDefault();
                event.stopImmediatePropagation();
                setEditorModalOpen(blockPanel, false);
            }
        }, true);

        root.querySelectorAll("[data-template-block-cancel]").forEach(function (button) {
            pageScope.listen(button, "click", function () { setEditorModalOpen(blockPanel, false); });
        });
        root.querySelectorAll("[data-template-field-cancel]").forEach(function (button) {
            pageScope.listen(button, "click", function () { setEditorModalOpen(fieldPanel, false); });
        });
        pageScope.listen(root.querySelector("[data-template-block-save]"), "click", function () {
            blockForm.dispatchEvent(new Event("submit", { cancelable: true }));
        });
        pageScope.listen(root.querySelector("[data-template-field-save]"), "click", function () {
            fieldForm.dispatchEvent(new Event("submit", { cancelable: true }));
        });
        pageScope.listen(form.elements.realtime_grouping_enabled, "change", function () {
            dirty = true;
            renderRealtimeGroups();
            renderFields();
        });
        pageScope.listen(form.querySelector("[data-template-group-add]"), "click", function () {
            realtimeGroups.push({ id: generateGroupID(), name: "新分组", order: realtimeGroups.length });
            dirty = true;
            renderRealtimeGroups();
        });

        // 校验设备类型编辑向导第一步。
        function validateStepOne() {
            const controls = [form.elements.id, form.elements.display_name, form.elements.default_start_register, form.elements.device_address_stride];
            const invalid = controls.find(function (control) { return !control.checkValidity(); });
            if (invalid) {
                invalid.reportValidity();
                throw new Error("请先完整填写基础信息并检查输入范围");
            }
            if (!templateReadBlocks.length) throw new Error("至少需要添加 1 个读取区块");
            const stride = numberValue(form.elements.device_address_stride, 0);
            if (!Number.isInteger(stride) || stride < minimumStride() || stride > 65536) {
                throw new Error("设备地址跨度必须覆盖全部读取区块，当前最小允许值为 " + minimumStride());
            }
            templateReadBlocks.forEach(function (block, index) {
                if ((block.function_code !== 3 && block.function_code !== 4) || block.start_offset < 0 ||
                    block.register_count < 1 || block.register_count > 125 || blockEnd(block) > stride) {
                    throw new Error("第 " + (index + 1) + " 个读取区块配置不合法");
                }
            });
            for (let left = 0; left < templateReadBlocks.length; left += 1) {
                for (let right = left + 1; right < templateReadBlocks.length; right += 1) {
                    const first = templateReadBlocks[left];
                    const second = templateReadBlocks[right];
                    if (first.function_code === second.function_code && first.start_offset < blockEnd(second) && second.start_offset < blockEnd(first)) {
                        throw new Error("相同功能码读取区块不能重叠：“" + first.display_name + "”与“" + second.display_name + "”");
                    }
                }
            }
        }

        // 切换编辑向导步骤，并按需校验前一步。
        function showStep(step, validateForward) {
            if (step === 2 && validateForward) validateStepOne();
            currentStep = step === 2 ? 2 : 1;
            form.querySelectorAll("[data-template-step-panel]").forEach(function (panel) {
                panel.hidden = Number(panel.dataset.templateStepPanel) !== currentStep;
            });
            root.querySelectorAll("[data-template-step-target]").forEach(function (tab) {
                const active = Number(tab.dataset.templateStepTarget) === currentStep;
                tab.classList.toggle("is-active", active);
                if (active) tab.setAttribute("aria-current", "step");
                else tab.removeAttribute("aria-current");
            });
            root.querySelector("[data-template-step-previous]").hidden = currentStep === 1;
            root.querySelector("[data-template-step-next]").hidden = currentStep === 2;
            submitButton.hidden = currentStep !== 2;
            blockPanel.querySelectorAll("input,select,textarea,button").forEach(function (control) {
                control.disabled = currentStep !== 1 || !editorModalOpen(blockPanel);
            });
            fieldPanel.querySelectorAll("input,select,textarea,button").forEach(function (control) {
                control.disabled = currentStep !== 2 || !editorModalOpen(fieldPanel);
            });
            window.scrollTo({ top: 0, behavior: "smooth" });
        }

        root.querySelectorAll("[data-template-step-target]").forEach(function (tab) {
            pageScope.listen(tab, "click", function () {
                try { showStep(Number(tab.dataset.templateStepTarget), currentStep === 1); }
                catch (error) { setFeedback(feedback, "error", error.message); }
            });
        });
        pageScope.listen(root.querySelector("[data-template-step-next]"), "click", function () {
            try { showStep(2, true); setFeedback(feedback, "", ""); }
            catch (error) { setFeedback(feedback, "error", error.message); }
        });
        pageScope.listen(root.querySelector("[data-template-step-previous]"), "click", function () { showStep(1, false); });

        // 标记页面草稿已被用户修改。
        function markPageDraftDirty(event) {
            if (event.target.closest("[data-template-block-panel],[data-template-field-panel]")) return;
            dirty = true;
        }
        pageScope.listen(form, "input", markPageDraftDirty);
        pageScope.listen(form, "change", markPageDraftDirty);
        const backLink = document.querySelector("[data-template-editor-back]");
        if (backLink) {
            pageScope.listen(backLink, "click", async function (event) {
                if (!dirty) return;
                event.preventDefault();
                if (await confirmAction({
                    title: "确认离开编辑",
                    message: "当前设备类型存在未保存修改，离开后这些修改将丢失。",
                    confirmText: "确认离开"
                })) window.location.href = backLink.href;
            });
        }
        pageScope.listen(blockForm, "submit", function (event) {
            event.preventDefault();
            setFeedback(blockFeedback, "", "");
            if (!blockForm.reportValidity()) {
                setFeedback(blockFeedback, "error", "请完整填写读取区块配置，并检查输入范围");
                return;
            }
            const displayName = blockForm.elements.block_display_name.value.trim();
            const functionCode = numberValue(blockForm.elements.block_function_code, 0);
            const startOffset = numberValue(blockForm.elements.block_start_offset, -1);
            const registerCount = numberValue(blockForm.elements.read_block_quantity, 0);
            const stride = numberValue(form.elements.device_address_stride, 0);
            if (!displayName) {
                setFeedback(blockFeedback, "error", "读取区块名称不能为空");
                return;
            }
            if (functionCode !== 3 && functionCode !== 4) {
                setFeedback(blockFeedback, "error", "读取区块功能码仅支持 FC03 或 FC04");
                return;
            }
            if (!Number.isInteger(startOffset) || startOffset < 0 || startOffset > 65535) {
                setFeedback(blockFeedback, "error", "读取区块起始偏移必须为 0～65535 的整数");
                return;
            }
            if (!Number.isInteger(registerCount) || registerCount < 1 || registerCount > 125) {
                setFeedback(blockFeedback, "error", "读取区块寄存器数量必须为 1～125");
                return;
            }
            const end = startOffset + registerCount;
            if (end > 65536) {
                setFeedback(blockFeedback, "error", "读取区块超出 Modbus 地址空间");
                return;
            }
            if (!Number.isInteger(stride) || stride <= 0 || end > stride) {
                setFeedback(blockFeedback, "error", "读取区块超出设备地址跨度，请先将设备地址跨度调整到至少 " + end);
                return;
            }
            const overlaps = templateReadBlocks.some(function (block, index) {
                return index !== editingBlockIndex && block.function_code === functionCode &&
                    startOffset < blockEnd(block) && block.start_offset < end;
            });
            if (overlaps) {
                setFeedback(blockFeedback, "error", "相同功能码的读取区块地址不能重叠；FC03 与 FC04 可使用相同偏移");
                return;
            }
            const blockKey = editingBlockIndex >= 0 ? templateReadBlocks[editingBlockIndex].block_key : generateBlockKey();
            const referencedFieldOutside = templateFields.find(function (field) {
                return field.read_block_key === blockKey && field.register_offset + field.register_count > registerCount;
            });
            if (referencedFieldOutside) {
                setFeedback(blockFeedback, "error", "字段“" + (referencedFieldOutside.display_name || referencedFieldOutside.key) + "”将超出该读取区块，请先调整字段偏移");
                return;
            }
            const block = {
                block_key: blockKey,
                display_name: displayName,
                function_code: functionCode,
                start_offset: startOffset,
                register_count: registerCount,
                sort_order: editingBlockIndex >= 0 ? editingBlockIndex : templateReadBlocks.length
            };
            if (editingBlockIndex >= 0) templateReadBlocks[editingBlockIndex] = block;
            else templateReadBlocks.push(block);
            renderReadBlocks();
            renderFields();
            dirty = true;
            setEditorModalOpen(blockPanel, false);
        });

        pageScope.listen(fieldForm, "submit", function (event) {
            event.preventDefault();
            setFeedback(fieldFeedback, "", "");
            if (!fieldForm.reportValidity()) {
                setFeedback(fieldFeedback, "error", "请完整填写所有必填项，并检查输入格式");
                return;
            }
            const key = fieldForm.elements.key.value.trim();
            if (!/^[a-z0-9_-]+$/.test(key)) {
                setFeedback(fieldFeedback, "error", "数据项标识仅允许小写字母、数字、下划线和短横线");
                return;
            }
            const duplicate = templateFields.some(function (field, index) { return index !== editingFieldIndex && field.key === key; });
            if (duplicate) {
                setFeedback(fieldFeedback, "error", "数据项标识不能重复：" + key);
                return;
            }
            const registerOffset = numberValue(fieldForm.elements.register_offset, -1);
            const selectedBlock = blockByKey(fieldBlockSelect.value);
            const selectedFieldType = fieldForm.elements.data_type.value;
            const fieldRegisterCount = registerCountForFieldType(selectedFieldType);
            if (!selectedBlock) {
                setFeedback(fieldFeedback, "error", "请先添加读取区块");
                return;
            }
            if (!Number.isInteger(registerOffset) || registerOffset < 0 || registerOffset + fieldRegisterCount > selectedBlock.register_count) {
                setFeedback(fieldFeedback, "error", "当前字段占用范围不能超出所属读取区块");
                return;
            }
            const bitIndex = selectedFieldType === "bool" ? numberValue(fieldForm.elements.bit_index, -1) : -1;
            if (selectedFieldType === "bool" && (!Number.isInteger(bitIndex) || bitIndex < 0 || bitIndex > 15)) {
                setFeedback(fieldFeedback, "error", "位索引必须为 0～15");
                return;
            }
            let enumItems;
            try {
                enumItems = validateEnumItems(selectedFieldType, fieldForm.elements.enum_enabled.checked);
            } catch (error) {
                setFeedback(fieldFeedback, "error", error.message);
                return;
            }
            const scale = numberValue(fieldForm.elements.scale, 0);
            if (scale === 0) {
                setFeedback(fieldFeedback, "error", "比例系数不能为 0");
                return;
            }
            let invalidRule;
            try {
                invalidRule = validateInvalidRule(
                    fieldForm.elements.invalid_rule_type.value,
                    fieldForm.elements.invalid_rule_value.value,
                    fieldForm.elements.invalid_rule_min.value,
                    fieldForm.elements.invalid_rule_max.value
                );
            } catch (error) {
                setFeedback(fieldFeedback, "error", error.message);
                return;
            }
            const field = normalizeField({
                key: key,
                display_name: fieldForm.elements.field_display_name.value.trim(),
                unit: fieldForm.elements.unit.value.trim(),
                editor_data_type: selectedFieldType,
                data_type: protocolDataType(selectedFieldType),
                read_block_key: selectedBlock.block_key,
                data_order: is32BitFieldType(selectedFieldType) ? fieldForm.elements.data_order.value : "ABCD",
                register_offset: registerOffset,
                bit_index: bitIndex,
                enum_items: enumItems,
                scale: scale,
                offset: numberValue(fieldForm.elements.offset, 0),
                precision: numberValue(fieldForm.elements.precision, 0),
                summary: fieldForm.elements.summary.checked,
                history_enabled: fieldForm.elements.history_enabled.checked,
                show_in_realtime: fieldForm.elements.show_in_realtime.checked,
                realtime_group_id: fieldForm.elements.realtime_group_id ? fieldForm.elements.realtime_group_id.value : "",
                invalid_rule_type: invalidRule.type,
                invalid_rule_value: invalidRule.value,
                invalid_rule_min: invalidRule.minimum,
                invalid_rule_max: invalidRule.maximum
            });
            const overlap = templateFields.find(function (existing, index) {
                if (index === editingFieldIndex || existing.read_block_key !== field.read_block_key) return false;
                const overlaps = field.register_offset < existing.register_offset + existing.register_count &&
                    existing.register_offset < field.register_offset + field.register_count;
                if (!overlaps) return false;
                const sameRegister = field.register_count === 1 && existing.register_count === 1 &&
                    field.register_offset === existing.register_offset;
                if (sameRegister && field.editor_data_type === "bool" && existing.editor_data_type === "bool") {
                    return field.bit_index === existing.bit_index;
                }
                if (sameRegister && new Set([field.editor_data_type, existing.editor_data_type]).size === 2 &&
                    [field.editor_data_type, existing.editor_data_type].every(function (type) { return type === "high_uint8" || type === "low_uint8"; })) {
                    return false;
                }
                return true;
            });
            if (overlap) {
                setFeedback(fieldFeedback, "error", field.editor_data_type === "bool" && overlap.editor_data_type === "bool"
                    ? "同一寄存器的位索引不能重复"
                    : "单比特字段不能与非单比特字段共享寄存器，其他字段寄存器范围也不能重叠");
                return;
            }
            if (editingFieldIndex >= 0) templateFields[editingFieldIndex] = field;
            else templateFields.push(field);
            renderFields();
            renderReadBlocks();
            dirty = true;
            renderRealtimeGroups();
            setEditorModalOpen(fieldPanel, false);
        });

        pageScope.listen(form, "submit", async function (event) {
            event.preventDefault();
            setFeedback(feedback, "", "");
            let payload;
            try { payload = buildPayload(); }
            catch (error) {
                setFeedback(feedback, "error", error.message);
                return;
            }
            submitButton.disabled = true;
            submitButton.textContent = "保存中...";
            try {
                const url = editingID ? "/api/device-templates/" + encodeURIComponent(editingID) : "/api/device-templates";
                const response = await csrfFetch(url, {
                    method: editingID ? "PUT" : "POST",
                    headers: { "Accept": "application/json", "Content-Type": "application/json" },
                    body: JSON.stringify(payload)
                });
                const result = await readApiResponse(response, "设备类型保存失败");
                if (!result.ok) {
                    setFeedback(feedback, "error", result.message, {
                        toastOptions: { autohide: false }
                    });
                    return;
                }
                dirty = false;
                saving = true;
                const message = editingID ? "自定义设备类型已更新" : "自定义设备类型已创建";
                const target = new URL(root.dataset.templateReturnPath || "/settings/device-types", window.location.origin);
                target.searchParams.set("flash_type", "success");
                target.searchParams.set("flash_message", message);
                window.location.assign(target.toString());
            } catch (error) {
                if (error && error.edgeStale) return;
                const message = friendlyApiMessage(error.message, "设备类型保存失败：网络请求异常");
                setFeedback(feedback, "error", message, {
                    toastOptions: { autohide: false }
                });
            } finally {
                submitButton.disabled = false;
                submitButton.textContent = editingID ? "保存修改" : "创建设备类型";
            }
        });

        let initialDefinition = null;
        try {
            const parsed = JSON.parse(root.dataset.templateInitialJson || "{}");
            initialDefinition = parsed && parsed.id ? parsed : null;
        } catch (_error) {
            showToast("error", "设备类型初始数据解析失败，请返回列表后重试");
        }
        openEditor(initialDefinition);
        renderFields();
        showStep(1, false);
        dirty = false;

    }

    // 初始化设备类型删除按钮和确认流程。
    function initDeviceTemplateDeleteControls() {
        document.querySelectorAll("[data-template-delete]").forEach(function (button) {
            pageScope.listen(button, "click", async function () {
                const id = button.dataset.templateId || "";
                const name = button.dataset.templateName || id;
                if (!await confirmAction({
                    title: "确认删除设备类型",
                    message: "删除自定义设备类型“" + name + "”后无法恢复，确定继续吗？",
                    confirmText: "确认删除",
                    danger: true
                })) return;
                button.disabled = true;
                try {
                    const response = await csrfFetch("/api/device-templates/" + encodeURIComponent(id), {
                        method: "DELETE", headers: { "Accept": "application/json" }
                    });
                    const result = await response.json();
                    if (!response.ok || !result.success) throw new Error(result && result.error ? result.error.message : "删除失败");
                    showToast("success", "自定义设备类型已删除");
                    const scope = pageScope;
                    if (scope && scope.isActive()) {
                        scope.setTimeout(function () { window.location.reload(); }, 500);
                    }
                } catch (error) {
                    if (error && error.edgeStale) return;
                    button.disabled = false;
                    showToast("error", friendlyApiMessage(error.message, "设备类型删除失败"));
                }
            });
        });
    }

    // 初始化设备类型实时显示偏好控件。
    function initTemplateRealtimeDisplayControls() {
        document.querySelectorAll("[data-template-realtime-toggle]").forEach(function (input) {
            pageScope.listen(input, "change", async function () {
                const previous = !input.checked;
                const container = input.closest(".template-realtime-toggle");
                const state = container ? container.querySelector("[data-template-realtime-state]") : null;
                input.disabled = true;
                if (state) state.textContent = "保存中...";
                try {
                    const url = "/api/device-templates/" + encodeURIComponent(input.dataset.templateId || "") +
                        "/fields/" + encodeURIComponent(input.dataset.fieldKey || "") + "/realtime-display";
                    const response = await csrfFetch(url, {
                        method: "PUT",
                        headers: { "Accept": "application/json", "Content-Type": "application/json" },
                        body: JSON.stringify({ show_in_realtime: input.checked })
                    });
                    const payload = await response.json();
                    if (!response.ok || !payload.success) {
                        throw new Error(payload && payload.error ? payload.error.message : "保存失败");
                    }
                    if (state) state.textContent = input.checked ? "显示" : "隐藏";
                    showToast("success", "实时展示配置已保存");
                } catch (error) {
                    if (error && error.edgeStale) return;
                    input.checked = previous;
                    if (state) state.textContent = input.checked ? "显示" : "隐藏";
                    showToast("error", friendlyApiMessage(error.message, "实时展示配置保存失败"));
                } finally {
                    input.disabled = false;
                }
            });
        });
    }

    // 初始化设备类型历史记录偏好控件。
    function initTemplateHistoryControls() {
        document.querySelectorAll("[data-template-history-toggle]").forEach(function (input) {
            pageScope.listen(input, "change", async function () {
                const previous = !input.checked;
                const container = input.closest(".template-realtime-toggle");
                const state = container ? container.querySelector("[data-template-history-state]") : null;
                input.disabled = true;
                if (state) state.textContent = "保存中...";
                try {
                    const url = "/api/device-templates/" + encodeURIComponent(input.dataset.templateId || "") +
                        "/fields/" + encodeURIComponent(input.dataset.fieldKey || "") + "/history-enabled";
                    const response = await csrfFetch(url, {
                        method: "PUT",
                        headers: { "Accept": "application/json", "Content-Type": "application/json" },
                        body: JSON.stringify({ history_enabled: input.checked })
                    });
                    const payload = await response.json();
                    if (!response.ok || !payload.success) {
                        throw new Error(payload && payload.error ? payload.error.message : "保存失败");
                    }
                    if (state) state.textContent = input.checked ? "启用" : "停用";
                    showToast("success", "历史记录配置已保存");
                } catch (error) {
                    if (error && error.edgeStale) return;
                    input.checked = previous;
                    if (state) state.textContent = input.checked ? "启用" : "停用";
                    showToast("error", friendlyApiMessage(error.message, "历史记录配置保存失败"));
                } finally {
                    input.disabled = false;
                }
            });
        });
    }

    EdgeApp.registerPageController("device-types", ["settings"], { mount: mount });
})();
