// Modbus 北向配置页：运行态轻量轮询、设置即时应用、映射 CRUD 与地址辅助。
(function () {
    "use strict";

    const app = window.EdgeApp;
    if (!app) return;

    function mount(scope) {
    const root = document.querySelector("[data-modbus-server-page]");
    if (!root) return;

    const motion = window.EdgeMotion;
    const isPageHidden = app.isPageHidden || function () { return false; };
    const canManageServer = root.dataset.canManageServer === "true";
    const canManageMappings = root.dataset.canManageMappings === "true";
    let pointsAvailable = root.dataset.pointsAvailable === "true";
    let snapshot = parseJSON(root.dataset.snapshot, { settings: {}, runtime_status: {}, mappings: [] });
    let points = parseJSON(root.dataset.points, []);
    if (!snapshot || typeof snapshot !== "object") snapshot = { settings: {}, runtime_status: {}, mappings: [] };
    if (!Array.isArray(points)) points = [];
    let timer = null;
    let runtimeRequestPending = false;
    let editingMapping = null;
    let deletingMapping = null;
    let activeTab = "";

    const settingsForm = root.querySelector("[data-modbus-settings-form]");
    const mappingForm = root.querySelector("[data-mapping-form]");
    const mappingModal = document.getElementById("modbus-mapping-modal");
    const deleteModal = document.getElementById("modbus-delete-modal");
    const tabButtons = Array.from(root.querySelectorAll("[data-modbus-tab]"));
    const tabPanels = Array.from(root.querySelectorAll("[data-modbus-tab-panel]"));

    // 解析 JSON，格式错误时返回指定默认值。
    function parseJSON(value, fallback) {
        try { return JSON.parse(value || ""); } catch (error) { return fallback; }
    }

    // 请求 Modbus 服务端 API，并统一处理错误响应。
    async function api(url, options) {
        const response = await scope.csrfFetch(url, options || {});
        let payload = null;
        try { payload = await response.json(); } catch (error) { /* use status fallback */ }
        if (!response.ok || !payload || !payload.success) {
            const message = payload && payload.error && payload.error.message
                ? payload.error.message : "请求失败（HTTP " + response.status + "）";
            throw new Error(message);
        }
        return payload.data;
    }

    function setText(selector, value, kind) {
        const node = root.querySelector(selector);
        if (!node) return;
        const next = value == null || value === "" ? "-" : String(value);
        if (motion && kind) motion.updateText(node, next, kind);
        else node.textContent = next;
    }

    // 返回运行状态的显示文本和样式。
    function statePresentation(status) {
        const state = String(status && status.state || "").toLowerCase();
        if (!status || !status.configured_enabled || state === "disabled") return ["已关闭", "status-neutral"];
        if (state === "starting") return ["启动中", "status-warn"];
        if (status.listening || state === "listening" || state === "running") return ["正在监听", "status-ok"];
        if (state === "stopping") return ["正在停止", "status-warn"];
        if (state === "error") return ["运行异常", "status-bad"];
        return ["已停止", "status-neutral"];
    }

    // 渲染运行态。
    function renderRuntime(status) {
        status = status || {};
        const badge = root.querySelector("[data-modbus-state]");
        const presentation = statePresentation(status);
        if (motion) motion.updateStatus(badge, presentation[0], "pill " + presentation[1]);
        else {
            badge.textContent = presentation[0];
            badge.className = "pill " + presentation[1];
        }
        setText("[data-runtime-endpoint]", (status.listen_address || "-") + ":" + (status.listen_port || 0));
        setText("[data-runtime-unit]", status.unit_id == null ? "-" : status.unit_id);
        setText("[data-runtime-connections]", (status.current_connections || 0) + " / " + (status.total_connections || 0), "value");
        setText("[data-runtime-requests]", status.total_requests || 0, "value");
        setText("[data-runtime-responses]", (status.successful_requests || 0) + " / " + (status.exception_responses || 0), "value");
        const last = status.last_request_time_ms ? app.formatTimestamp(status.last_request_time_ms) : "暂无请求";
        setText("[data-runtime-last]", last + " / " + (status.last_client_ip || "-") );
        const errorNode = root.querySelector("[data-runtime-error]");
        errorNode.hidden = !status.last_error_message;
        errorNode.textContent = status.last_error_message ? "最近错误：" + status.last_error_message : "";
        root.querySelector("[data-runtime-unavailable]").hidden = true;
    }

    // 填充设置。
    function fillSettings(settings) {
        if (!settingsForm || !settings) return;
        ["listen_address", "listen_port", "unit_id", "max_clients", "idle_timeout_seconds", "max_read_registers"].forEach(function (name) {
            if (settingsForm.elements[name]) settingsForm.elements[name].value = settings[name] == null ? "" : settings[name];
        });
        settingsForm.elements.enabled.checked = !!settings.enabled;
        settingsForm.elements.strict_unit_id.checked = !!settings.strict_unit_id;
        renderEnabledToggle();
    }

    // 渲染启用开关。
    function renderEnabledToggle() {
        if (!settingsForm || !settingsForm.elements.enabled) return;
        const text = root.querySelector("[data-modbus-enabled-text]");
        if (text) text.textContent = settingsForm.elements.enabled.checked ? "已启用" : "已停用";
    }

    // 建立可导出点位索引。
    function pointIndex() {
        const devices = new Set();
        const targets = new Map();
        points.forEach(function (point) {
            devices.add(point.device_id);
            targets.set(point.device_id + "\n" + point.point_key, point);
        });
        return { devices: devices, targets: targets };
    }

    // 汇总映射目标的当前状态。
    function targetState(mapping) {
        if (!mapping.enabled) return ["映射禁用", "status-neutral"];
        if (!pointsAvailable) return ["状态未知", "status-neutral"];
        const index = pointIndex();
        if (!index.devices.has(mapping.device_id)) return ["设备不存在", "status-bad"];
        if (!index.targets.has(mapping.device_id + "\n" + mapping.point_key)) return ["数据项不存在", "status-warn"];
        return ["目标有效", "status-ok"];
    }

    // 统计指定寄存器类型占用数量。
    function typeCount(type) { return type === "uint16" || type === "int16" ? 1 : 2; }
    // 计算传统 Modbus 地址。
    function traditional(address) { return 40001 + Number(address || 0); }
    // 格式化寄存器地址。
    function addressText(start, count) {
        return count === 1 ? String(start) : start + "～" + (start + count - 1);
    }
    // 计算十六进制寄存器地址。
    function hexadecimalAddress(address) {
        return "0x" + Number(address).toString(16).toUpperCase().padStart(4, "0");
    }
    // 格式化十六进制寄存器地址。
    function hexadecimalAddressText(start, count) {
        return count === 1
            ? hexadecimalAddress(start)
            : hexadecimalAddress(start) + "～" + hexadecimalAddress(start + count - 1);
    }

    // 创建单元格。
    function createCell(row, className, label) {
        const cell = document.createElement("td");
        if (className) cell.className = className;
        if (label) cell.dataset.label = label;
        row.appendChild(cell);
        return cell;
    }

    // 追加单元格文本。
    function appendCellText(cell, tagName, text, className) {
        const element = document.createElement(tagName);
        if (className) element.className = className;
        element.textContent = text;
        cell.appendChild(element);
        return element;
    }

    // 追加映射行。
    function appendMappingLine(cell, label, value) {
        const line = document.createElement("div");
        line.className = "modbus-table-line";
        appendCellText(line, "small", label);
        appendCellText(line, "span", value);
        cell.appendChild(line);
    }

    // 渲染映射。
    function renderMappings() {
        const body = root.querySelector("[data-mapping-rows]");
        const empty = root.querySelector("[data-mapping-empty]");
        body.textContent = "";
        const mappings = Array.isArray(snapshot.mappings) ? snapshot.mappings : [];
        empty.hidden = mappings.length !== 0;
        mappings.forEach(function (mapping) {
            const row = document.createElement("tr");
            const state = targetState(mapping);
            const target = createCell(row, "modbus-target-cell", "映射对象");
            appendCellText(target, "strong", (mapping.device_name_snapshot || mapping.device_id) + " / " + (mapping.point_name_snapshot || mapping.point_key), "table-primary");
            target.title = mapping.device_id + " / " + mapping.point_key;
            const count = typeCount(mapping.data_type);
            appendCellText(target, "span", mapping.data_type + " · " + count + " 个寄存器", "table-secondary");

            const addresses = createCell(row, "modbus-address-cell", "寄存器");
            appendMappingLine(addresses, "数据", addressText(mapping.start_address, count) + " / " + hexadecimalAddressText(mapping.start_address, count) + " / " + addressText(traditional(mapping.start_address), count));
            appendMappingLine(addresses, "质量", mapping.quality_address + " / " + hexadecimalAddress(mapping.quality_address) + " / " + traditional(mapping.quality_address));

            const transform = createCell(row, "modbus-transform-cell", "数值处理");
            const offset = Number(mapping.value_offset);
            appendMappingLine(transform, "换算", "×" + mapping.value_multiplier + " " + (offset < 0 ? "−" + Math.abs(offset) : "+" + offset));
            appendMappingLine(transform, "字序", byteOrderText(mapping.byte_order) + " · " + wordOrderText(mapping.word_order));

            const status = createCell(row, "modbus-state-cell", "状态");
            appendCellText(status, "span", mapping.enabled ? "启用" : "禁用", mapping.enabled ? "status-ok" : "status-neutral");
            appendCellText(status, "span", state[0], state[1]);

            const actionCell = createCell(row, "modbus-action-cell", "操作");
            const actions = document.createElement("div");
            actions.className = "modbus-row-actions";
            actionCell.appendChild(actions);
            if (canManageMappings) {
                const edit = document.createElement("button");
                edit.type = "button"; edit.className = "btn btn-secondary btn-small"; edit.textContent = "编辑";
                edit.setAttribute("aria-haspopup", "dialog");
                edit.setAttribute("aria-controls", "modbus-mapping-modal");
                edit.addEventListener("click", function () { openMappingEditor(mapping); });
                const remove = document.createElement("button");
                remove.type = "button"; remove.className = "btn btn-danger btn-small"; remove.textContent = "删除";
                remove.setAttribute("aria-haspopup", "dialog");
                remove.setAttribute("aria-controls", "modbus-delete-modal");
                remove.addEventListener("click", function () { openDelete(mapping); });
                actions.append(edit, remove);
            } else actions.textContent = "只读";
            body.appendChild(row);
        });
    }

    // 返回字节序的中文名称。
    function byteOrderText(value) { return value === "little_endian" ? "小端" : "大端"; }
    // 返回字序的中文名称。
    function wordOrderText(value) { return value === "low_word_first" ? "低字在前" : "高字在前"; }

    // 更新快照。
    function updateSnapshot(next) {
        snapshot = next || { settings: {}, runtime_status: {}, mappings: [] };
        snapshot.mappings = snapshot.mappings || [];
        fillSettings(snapshot.settings);
        renderRuntime(snapshot.runtime_status);
        renderMappings();
    }

    // 刷新 Modbus 服务端运行状态，避免并发和后台页面请求。
    async function refreshRuntime() {
        if (activeTab !== "settings" || runtimeRequestPending || isPageHidden()) return;
        runtimeRequestPending = true;
        try {
            snapshot.runtime_status = await api("/api/modbus-server/runtime-status");
            renderRuntime(snapshot.runtime_status);
        } catch (error) {
            const unavailable = root.querySelector("[data-runtime-unavailable]");
            unavailable.hidden = false;
            if (motion) motion.reveal(unavailable);
        } finally { runtimeRequestPending = false; }
    }

    // 启动定时器。
    function startTimer() {
        stopTimer();
        if (!isPageHidden()) timer = window.setInterval(refreshRuntime, 4000);
    }
    // 停止定时器。
    function stopTimer() { if (timer) window.clearInterval(timer); timer = null; }

    // 选择页签。
    function selectTab(name) {
        if (name === activeTab || !tabPanels.some(function (panel) { return panel.dataset.modbusTabPanel === name; })) return;
        const animate = activeTab !== "";
        activeTab = name;
        tabButtons.forEach(function (button) {
            const selected = button.dataset.modbusTab === name;
            button.classList.toggle("is-active", selected);
            button.setAttribute("aria-selected", selected ? "true" : "false");
            button.tabIndex = selected ? 0 : -1;
        });
        tabPanels.forEach(function (panel) {
            const selected = panel.dataset.modbusTabPanel === name;
            panel.hidden = !selected;
            if (selected && animate && motion) motion.reveal(panel);
        });
        if (name === "settings" && !isPageHidden()) {
            refreshRuntime();
            startTimer();
        } else {
            stopTimer();
        }
    }

    // 刷新页面快照及可映射点位，并按需提示局部失败。
    async function fullRefresh(showFailure) {
        try {
            const nextSnapshot = await api("/api/modbus-server/page-snapshot");
            if (canManageMappings) {
                try {
                    const nextPoints = await api("/api/modbus-server/exportable-points");
                    if (Array.isArray(nextPoints)) { points = nextPoints; pointsAvailable = true; }
                } catch (pointError) {
                    pointsAvailable = false;
                    if (showFailure) app.showToast("error", pointError.message);
                }
            }
            updateSnapshot(nextSnapshot);
        } catch (error) {
            if (showFailure) app.showToast("error", error.message);
        }
    }

    // 解析并校验整数输入。
    function integerValue(form, name) { return Number.parseInt(form.elements[name].value, 10); }

    if (settingsForm && canManageServer) {
        settingsForm.elements.enabled.addEventListener("change", renderEnabledToggle);
        settingsForm.addEventListener("submit", async function (event) {
            event.preventDefault();
            const button = settingsForm.querySelector("[data-settings-submit]");
            if (button.disabled) return;
            button.disabled = true; button.textContent = "正在应用…";
            const request = {
                enabled: settingsForm.elements.enabled.checked,
                listen_address: settingsForm.elements.listen_address.value.trim(),
                listen_port: integerValue(settingsForm, "listen_port"),
                unit_id: integerValue(settingsForm, "unit_id"),
                strict_unit_id: settingsForm.elements.strict_unit_id.checked,
                max_clients: integerValue(settingsForm, "max_clients"),
                idle_timeout_seconds: integerValue(settingsForm, "idle_timeout_seconds"),
                max_read_registers: integerValue(settingsForm, "max_read_registers")
            };
            try {
                await api("/api/modbus-server/settings", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(request) });
                app.showToast("success", "Modbus 北向服务设置已保存。");
            } catch (error) { app.showToast("error", error.message); }
            finally {
                button.disabled = false; button.textContent = "保存并立即应用";
                await fullRefresh(false);
            }
        });
    }

    // 去重设备。
    function uniqueDevices() {
        const seen = new Set();
        return points.filter(function (point) {
            if (seen.has(point.device_id)) return false;
            seen.add(point.device_id); return true;
        });
    }

    // 填充设备选项。
    function fillDeviceOptions(selected, danglingName) {
        const select = mappingForm.elements.device_id;
        select.textContent = "";
        uniqueDevices().forEach(function (point) {
            const option = new Option(point.device_name + "（" + point.device_type_name + "）", point.device_id);
            select.add(option);
        });
        if (selected && !Array.from(select.options).some(function (option) { return option.value === selected; })) {
            select.add(new Option((danglingName || selected) + "（当前不存在）", selected));
        }
        select.value = selected || (select.options[0] ? select.options[0].value : "");
    }

    // 填充点位选项。
    function fillPointOptions(selected, danglingName) {
        const deviceID = mappingForm.elements.device_id.value;
        const select = mappingForm.elements.point_key;
        select.textContent = "";
        points.filter(function (point) { return point.device_id === deviceID; }).forEach(function (point) {
            const suffix = [point.point_key, point.unit, point.summary ? "关键数据" : "普通数据"].filter(Boolean).join(" · ");
            select.add(new Option(point.point_name + "（" + suffix + "）", point.point_key));
        });
        if (selected && !Array.from(select.options).some(function (option) { return option.value === selected; })) {
            select.add(new Option((danglingName || selected) + "（当前不存在）", selected));
        }
        select.value = selected || (select.options[0] ? select.options[0].value : "");
        renderPointDetail();
    }

    // 返回映射编辑器当前选中的点位。
    function selectedPoint() {
        return points.find(function (point) {
            return point.device_id === mappingForm.elements.device_id.value && point.point_key === mappingForm.elements.point_key.value;
        });
    }

    // 渲染点位详情。
    function renderPointDetail() {
        const point = selectedPoint();
        root.querySelector("[data-point-detail]").textContent = point
            ? [point.point_key, point.unit || "无单位", point.summary ? "关键数据" : "非关键数据"].join(" · ")
            : "当前目标不存在，可重新选择有效目标";
    }

    // 打开映射编辑器。
    function openMappingEditor(mapping) {
        editingMapping = mapping || null;
        mappingForm.reset();
        mappingForm.elements.enabled.checked = true;
        mappingForm.elements.value_multiplier.value = "1";
        mappingForm.elements.value_offset.value = "0";
        mappingForm.elements.byte_order.value = "big_endian";
        mappingForm.elements.word_order.value = "high_word_first";
        root.querySelector("[data-mapping-feedback]").textContent = "";
        root.querySelector("[data-mapping-modal-title]").textContent = mapping ? "编辑映射" : "新增映射";
        fillDeviceOptions(mapping && mapping.device_id, mapping && mapping.device_name_snapshot);
        fillPointOptions(mapping && mapping.point_key, mapping && mapping.point_name_snapshot);
        if (mapping) {
            mappingForm.elements.mapping_id.value = mapping.mapping_id;
            ["data_type", "start_address", "quality_address", "value_multiplier", "value_offset", "byte_order", "word_order"].forEach(function (name) {
                mappingForm.elements[name].value = mapping[name];
            });
            mappingForm.elements.enabled.checked = !!mapping.enabled;
        } else recommendAddress();
        updateAddressPreview();
        app.openModal(mappingModal);
    }

    // 渲染映射启用开关。
    function renderMappingEnabledToggle() {
        if (!mappingForm || !mappingForm.elements.enabled) return;
        const text = root.querySelector("[data-mapping-enabled-text]");
        if (text) text.textContent = mappingForm.elements.enabled.checked ? "已启用" : "已禁用";
    }

    // 计算现有映射已占用的寄存器地址。
    function occupiedAddresses(excludedID) {
        const occupied = new Map();
        (snapshot.mappings || []).forEach(function (mapping) {
            if (mapping.mapping_id === excludedID) return;
            const label = (mapping.device_name_snapshot || mapping.device_id) + " / " + (mapping.point_name_snapshot || mapping.point_key);
            for (let i = 0; i < typeCount(mapping.data_type); i += 1) occupied.set(mapping.start_address + i, label + " 的数据地址");
            occupied.set(mapping.quality_address, label + " 的质量地址");
        });
        return occupied;
    }

    // 推荐地址。
    function recommendAddress() {
        if (!mappingForm) return;
        const count = typeCount(mappingForm.elements.data_type.value);
        const occupied = occupiedAddresses(editingMapping && editingMapping.mapping_id);
        for (let start = 0; start + count <= 65535; start += 1) {
            let free = true;
            for (let offset = 0; offset <= count; offset += 1) if (occupied.has(start + offset)) { free = false; break; }
            if (free) {
                mappingForm.elements.start_address.value = start;
                mappingForm.elements.quality_address.value = start + count;
                updateAddressPreview(); return;
            }
        }
        root.querySelector("[data-mapping-feedback]").textContent = "没有可同时容纳数据和质量寄存器的连续空闲地址。";
    }

    // 获取当前冲突。
    function currentConflict(start, count, quality) {
        const occupied = occupiedAddresses(editingMapping && editingMapping.mapping_id);
        for (let offset = 0; offset < count; offset += 1) {
            if (occupied.has(start + offset)) return "地址 " + (start + offset) + " 已被“" + occupied.get(start + offset) + "”占用";
        }
        if (occupied.has(quality)) return "地址 " + quality + " 已被“" + occupied.get(quality) + "”占用";
        if (quality >= start && quality < start + count) return "质量地址与当前数据地址重叠";
        return "";
    }

    // 更新地址预览。
    function updateAddressPreview() {
        if (!mappingForm) return;
        const type = mappingForm.elements.data_type.value;
        const count = typeCount(type);
        const is16 = count === 1;
        const wordOrderField = root.querySelector("[data-word-order-field]");
        const wordOrderHelp = root.querySelector("[data-word-order-help]");
        mappingForm.elements.word_order.disabled = is16;
        if (is16) mappingForm.elements.word_order.value = "high_word_first";
        if (wordOrderField) wordOrderField.hidden = is16;
        if (wordOrderHelp) wordOrderHelp.hidden = is16;

        const byte = mappingForm.elements.byte_order.value;
        const word = mappingForm.elements.word_order.value;
        const orderText = is16
            ? (byte === "big_endian" ? "AB" : "BA")
            : (byte === "big_endian" ? (word === "high_word_first" ? "ABCD" : "CDAB") : (word === "high_word_first" ? "BADC" : "DCBA"));
        setText("[data-order-preview]", orderText);
        setText("[data-preview-order-label]", is16 ? "当前 16 位排列" : "当前 32 位排列");

        const multiplierText = mappingForm.elements.value_multiplier.value.trim() || "—";
        const offsetRaw = mappingForm.elements.value_offset.value.trim();
        const offsetValue = Number(offsetRaw);
        const offsetText = offsetRaw && Number.isFinite(offsetValue)
            ? (offsetValue < 0 ? " − " + Math.abs(offsetValue) : " + " + offsetValue)
            : " + —";
        setText("[data-preview-formula]", "寄存器值 = 工程值 × " + multiplierText + offsetText);
        renderMappingEnabledToggle();

        const startRaw = mappingForm.elements.start_address.value.trim();
        const qualityRaw = mappingForm.elements.quality_address.value.trim();
        const start = Number(startRaw);
        const quality = Number(qualityRaw);
        const preview = root.querySelector("[data-address-preview]");
        if (!startRaw || !qualityRaw || !Number.isInteger(start) || !Number.isInteger(quality)) {
            preview.classList.remove("has-conflict");
            setText("[data-preview-data-address]", "待输入");
            setText("[data-preview-quality-address]", "待输入");
            setText("[data-preview-traditional-address]", "待输入");
            setText("[data-preview-conflict]", "请输入数据地址和质量地址。");
            return;
        }
        const conflict = currentConflict(start, count, quality);
        preview.classList.toggle("has-conflict", !!conflict);
        setText("[data-preview-data-address]", addressText(start, count) + " / " + hexadecimalAddressText(start, count));
        setText("[data-preview-quality-address]", quality + " / " + hexadecimalAddress(quality));
        setText("[data-preview-traditional-address]", "数据 " + addressText(traditional(start), count) + "；质量 " + traditional(quality));
        setText("[data-preview-conflict]", conflict ? "冲突：" + conflict : "当前预检查无冲突。");
    }

    // 从表单构造寄存器映射请求。
    function mappingRequest() {
        const point = selectedPoint();
        const selectedDevice = points.find(function (item) { return item.device_id === mappingForm.elements.device_id.value; });
        const count = typeCount(mappingForm.elements.data_type.value);
        return {
            device_id: mappingForm.elements.device_id.value,
            point_key: mappingForm.elements.point_key.value,
            device_name_snapshot: point ? point.device_name : (editingMapping ? editingMapping.device_name_snapshot : (selectedDevice ? selectedDevice.device_name : "")),
            point_name_snapshot: point ? point.point_name : (editingMapping ? editingMapping.point_name_snapshot : ""),
            start_address: integerValue(mappingForm, "start_address"),
            data_type: mappingForm.elements.data_type.value,
            value_multiplier: Number(mappingForm.elements.value_multiplier.value),
            value_offset: Number(mappingForm.elements.value_offset.value),
            byte_order: mappingForm.elements.byte_order.value,
            word_order: count === 1 ? "high_word_first" : mappingForm.elements.word_order.value,
            quality_address: integerValue(mappingForm, "quality_address"),
            enabled: mappingForm.elements.enabled.checked
        };
    }

    if (mappingForm) {
        mappingForm.elements.device_id.addEventListener("change", function () { fillPointOptions("", ""); updateAddressPreview(); });
        mappingForm.elements.point_key.addEventListener("change", renderPointDetail);
        ["data_type", "start_address", "quality_address", "value_multiplier", "value_offset", "byte_order", "word_order"].forEach(function (name) {
            mappingForm.elements[name].addEventListener("input", updateAddressPreview);
            mappingForm.elements[name].addEventListener("change", updateAddressPreview);
        });
        mappingForm.elements.enabled.addEventListener("change", renderMappingEnabledToggle);
        mappingForm.addEventListener("submit", async function (event) {
            event.preventDefault();
            const request = mappingRequest();
            const conflict = currentConflict(request.start_address, typeCount(request.data_type), request.quality_address);
            const feedback = root.querySelector("[data-mapping-feedback]");
            if (!selectedPoint() && (!editingMapping || request.device_id !== editingMapping.device_id || request.point_key !== editingMapping.point_key)) {
                feedback.textContent = "当前设备或数据项已不存在，请重新选择。"; return;
            }
            if (!Number.isFinite(request.value_multiplier) || request.value_multiplier === 0 || !Number.isFinite(request.value_offset)) {
                feedback.textContent = "倍率必须为非零有限数，偏移必须为有限数。"; return;
            }
            if (conflict) { feedback.textContent = conflict; return; }
            const submit = root.querySelector("[data-mapping-submit]");
            submit.disabled = true; submit.textContent = "正在保存…";
            try {
                const editing = !!editingMapping;
                const url = editing ? "/api/modbus-server/mappings/" + encodeURIComponent(editingMapping.mapping_id) : "/api/modbus-server/mappings";
                await api(url, { method: editing ? "PUT" : "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(request) });
                app.closeModal(mappingModal);
                app.showToast("success", editing ? "寄存器映射已更新。" : "寄存器映射已创建。");
                await fullRefresh(false);
            } catch (error) { feedback.textContent = error.message; }
            finally { submit.disabled = false; submit.textContent = "保存映射"; }
        });
    }

    // 打开删除。
    function openDelete(mapping) {
        deletingMapping = mapping;
        root.querySelector("[data-delete-target]").textContent = (mapping.device_name_snapshot || mapping.device_id) + " / " + (mapping.point_name_snapshot || mapping.point_key);
        app.openModal(deleteModal);
    }

    const deleteConfirm = root.querySelector("[data-delete-confirm]");
    if (deleteConfirm) deleteConfirm.addEventListener("click", async function () {
        if (!deletingMapping || deleteConfirm.disabled) return;
        deleteConfirm.disabled = true; deleteConfirm.textContent = "正在删除…";
        try {
            await api("/api/modbus-server/mappings/" + encodeURIComponent(deletingMapping.mapping_id), { method: "DELETE" });
            app.closeModal(deleteModal); app.showToast("success", "寄存器映射已删除。");
            await fullRefresh(false);
        } catch (error) { app.showToast("error", error.message); }
        finally { deleteConfirm.disabled = false; deleteConfirm.textContent = "确认删除"; }
    });

    const createButton = root.querySelector("[data-mapping-create]");
    if (createButton) createButton.addEventListener("click", function () { openMappingEditor(null); });
    const recommendButton = root.querySelector("[data-address-recommend]");
    if (recommendButton) recommendButton.addEventListener("click", recommendAddress);
    const refreshButton = document.querySelector("[data-modbus-refresh]");
    if (refreshButton) refreshButton.addEventListener("click", async function () {
        refreshButton.disabled = true;
        await fullRefresh(true);
        refreshButton.disabled = false;
    });

    tabButtons.forEach(function (button, index) {
        button.addEventListener("click", function () { selectTab(button.dataset.modbusTab); });
        button.addEventListener("keydown", function (event) {
            let nextIndex = index;
            if (event.key === "ArrowRight") nextIndex = (index + 1) % tabButtons.length;
            else if (event.key === "ArrowLeft") nextIndex = (index - 1 + tabButtons.length) % tabButtons.length;
            else if (event.key === "Home") nextIndex = 0;
            else if (event.key === "End") nextIndex = tabButtons.length - 1;
            else return;
            event.preventDefault();
            const nextButton = tabButtons[nextIndex];
            selectTab(nextButton.dataset.modbusTab);
            nextButton.focus();
        });
    });

    scope.onVisibilityChange(function () {
        if (isPageHidden() || activeTab !== "settings") stopTimer(); else { refreshRuntime(); startTimer(); }
    });
    scope.listen(window, "pagehide", stopTimer);
    scope.listen(window, "pageshow", function (event) {
        if (event.persisted && activeTab === "settings" && !isPageHidden()) {
            refreshRuntime();
            startTimer();
        }
    });
    scope.onDispose(stopTimer);
    updateSnapshot(snapshot);
    selectTab("mappings");
    }

    app.registerPageController("modbus-server", ["settings"], { mount: mount });
})();
