// 采集管理页交互入口：覆盖通道、主站、设备名称、设备命令和通讯报文等现场操作。
// 配置写入统一调用 JSON API；页面只维护表单与展示状态，不在浏览器推导后端运行态。
(function () {
    "use strict";

    const EdgeApp = window.EdgeApp;
    if (!EdgeApp) return;

    const showToast = EdgeApp.showToast;
    const friendlyApiMessage = EdgeApp.friendlyApiMessage;
    const diagnosisSuggestion = EdgeApp.diagnosisSuggestion;
    const formatTimestamp = EdgeApp.formatTimestamp;
    const escapeHtml = EdgeApp.escapeHtml;
    const dataItemDisplayName = EdgeApp.dataItemDisplayName;
    const readFieldValue = EdgeApp.readFieldValue;
    const setFeedback = EdgeApp.setFeedback;
    const openModal = EdgeApp.openModal;
    const closeModal = EdgeApp.closeModal;
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
        initCommunicationTracePage();
        initCollectionManagementTabs();
        initChannelConfigForms();
        initMasterConfigForms();
        initDeviceNameEditor();
        initDeviceBatchNameEditor();
        initDeviceDetailModal();
    }


    // 初始化采集管理管理页签。
    function initCollectionManagementTabs() {
        const tabs = document.querySelectorAll("[data-collection-tab]");
        const panels = document.querySelectorAll("[data-collection-panel]");
        if (!tabs.length || !panels.length) {
            return;
        }

        const validPanels = new Set(["channels", "masters", "devices"]);
        // 规范化面板。
        function normalizePanel(value) {
            const key = String(value || "").replace(/^#/, "");
            return validPanels.has(key) ? key : "channels";
        }

        // 激活采集管理面板并按需同步地址栏锚点。
        function activatePanel(panelName, updateHash) {
            const target = normalizePanel(panelName);
            const currentPanel = document.querySelector("[data-collection-panel].is-active");
            const changed = !currentPanel || currentPanel.dataset.collectionPanel !== target;

            tabs.forEach(function (tab) {
                const selected = tab.dataset.collectionTab === target;
                tab.classList.toggle("is-active", selected);
                tab.setAttribute("aria-selected", selected ? "true" : "false");
                tab.tabIndex = selected ? 0 : -1;
            });

            panels.forEach(function (panel) {
                const selected = panel.dataset.collectionPanel === target;
                panel.classList.toggle("is-active", selected);
                panel.hidden = !selected;
                if (!selected && window.EdgeMotion && window.EdgeMotion.cancel) {
                    window.EdgeMotion.cancel(panel, "motion-content-enter");
                } else if (selected && changed && updateHash && window.EdgeMotion) {
                    window.EdgeMotion.reveal(panel);
                }
            });

            if (updateHash) {
                const nextHash = "#" + target;
                if (window.location.hash !== nextHash) {
                    window.history.pushState(null, "", nextHash);
                }
            }
        }

        tabs.forEach(function (tab) {
            pageScope.listen(tab, "click", function () {
                activatePanel(tab.dataset.collectionTab, true);
            });
            pageScope.listen(tab, "keydown", function (event) {
                const keys = ["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home", "End"];
                if (!keys.includes(event.key)) return;
                event.preventDefault();
                const tabList = Array.from(tabs);
                const currentIndex = Math.max(0, tabList.indexOf(tab));
                let nextIndex = currentIndex;
                if (event.key === "Home") nextIndex = 0;
                else if (event.key === "End") nextIndex = tabList.length - 1;
                else if (event.key === "ArrowLeft" || event.key === "ArrowUp") nextIndex = (currentIndex - 1 + tabList.length) % tabList.length;
                else nextIndex = (currentIndex + 1) % tabList.length;
                const nextTab = tabList[nextIndex];
                activatePanel(nextTab.dataset.collectionTab, true);
                nextTab.focus();
            });
        });

        pageScope.listen(window, "hashchange", function () {
            activatePanel(window.location.hash, false);
        });

        activatePanel(window.location.hash, false);
    }



    // ---------- 设备显示名称 ----------
    // 单设备改名只改变显示别名，不触发采集拓扑重建。
    function initDeviceNameEditor() {
        const modal = document.getElementById("device-name-modal");
        if (!modal) {
            return;
        }
        const form = modal.querySelector("[data-device-name-form]");
        const systemName = modal.querySelector("[data-device-system-name]");
        const displayName = modal.querySelector("[data-device-display-name]");
        const feedback = modal.querySelector("[data-device-name-feedback]");
        const submit = modal.querySelector("[data-device-name-submit]");
        let deviceId = "";

        document.querySelectorAll("[data-device-name-open]").forEach(function (button) {
            pageScope.listen(button, "click", function () {
                deviceId = button.dataset.deviceId || "";
                systemName.value = button.dataset.systemName || button.dataset.displayName || "";
                displayName.value = button.dataset.displayName || "";
                feedback.className = "device-detail-feedback hidden";
                feedback.textContent = "";
                openModal(modal);
                displayName.focus();
                displayName.select();
            });
        });
        modal.querySelectorAll("[data-device-name-close]").forEach(function (button) {
            pageScope.listen(button, "click", function () { closeModal(modal); });
        });
        pageScope.listen(form, "submit", async function (event) {
            event.preventDefault();
            const value = String(displayName.value || "").trim();
            if (Array.from(value).length > 40) {
                feedback.className = "device-detail-feedback flash flash-error";
                feedback.textContent = "自定义设备名称不能超过 40 个字符。";
                return;
            }
            submit.disabled = true;
            submit.textContent = "正在保存...";
            try {
                const response = await csrfFetch("/api/devices/" + encodeURIComponent(deviceId) + "/display-name", {
                    method: "PUT",
                    headers: { "Accept": "application/json", "Content-Type": "application/json" },
                    body: JSON.stringify({ display_name: value })
                });
                const payload = await response.json();
                if (!scope.isActive()) throw new Error("stale page task");
                if (!response.ok || !payload.success) {
                    throw new Error(payload && payload.error ? payload.error.message : "设备名称保存失败");
                }
                showToast("success", value ? "设备名称已更新" : "已恢复系统推导名称");
                pageScope.setTimeout(function () { window.location.reload(); }, 350);
            } catch (error) {
                if (error && error.edgeStale) return;
                feedback.className = "device-detail-feedback flash flash-error";
                feedback.textContent = friendlyApiMessage(error.message, "设备名称保存失败");
            } finally {
                submit.disabled = false;
                submit.textContent = "保存设备名称";
            }
        });
    }

    // 批量改名先在前端形成预览，最终成功/失败仍以后端逐项结果为准。
    function initDeviceBatchNameEditor() {
        const modal = document.getElementById("device-batch-name-modal");
        const openButton = document.querySelector("[data-device-batch-name-open]");
        if (!modal || !openButton) return;

        const form = modal.querySelector("[data-device-batch-name-form]");
        const scope = modal.querySelector("[data-device-batch-scope]");
        const master = modal.querySelector("[data-device-batch-master]");
        const masterField = modal.querySelector("[data-device-batch-master-field]");
        const prefix = modal.querySelector("[data-device-batch-prefix]");
        const start = modal.querySelector("[data-device-batch-start]");
        const digits = modal.querySelector("[data-device-batch-digits]");
        const separator = modal.querySelector("[data-device-batch-separator]");
        const previewBody = modal.querySelector("[data-device-batch-preview-body]");
        const count = modal.querySelector("[data-device-batch-count]");
        const feedback = modal.querySelector("[data-device-batch-name-feedback]");
        const failures = modal.querySelector("[data-device-batch-failures]");
        const submit = modal.querySelector("[data-device-batch-submit]");
        const restore = modal.querySelector("[data-device-batch-restore]");
        const devices = Array.from(document.querySelectorAll("[data-device-batch-row]")).map(function (row) {
            return {
                id: row.dataset.deviceId || "",
                currentName: row.dataset.currentName || row.dataset.systemName || "未命名设备",
                systemName: row.dataset.systemName || "-",
                masterId: row.dataset.masterId || "",
                masterName: row.dataset.masterName || row.dataset.masterId || "未绑定主站"
            };
        }).filter(function (device) { return Boolean(device.id); });

        const masters = new Map();
        devices.forEach(function (device) {
            if (device.masterId && !masters.has(device.masterId)) masters.set(device.masterId, device.masterName);
        });
        masters.forEach(function (name, id) {
            const option = document.createElement("option");
            option.value = id;
            option.textContent = name;
            master.appendChild(option);
        });

        // 返回批量操作中当前选中的设备。
        function selectedDevices() {
            if (scope.value !== "master") return devices;
            return devices.filter(function (device) { return device.masterId === master.value; });
        }

        // 构建预览。
        function buildPreview() {
            const selected = selectedDevices();
            const normalizedPrefix = String(prefix.value || "").trim();
            const startValue = Number(start.value);
            const digitValue = Number(digits.value);
            const validStart = Number.isInteger(startValue) && startValue >= 0;
            const names = selected.map(function (device, index) {
                const sequence = validStart ? String(startValue + index).padStart(digitValue, "0") : "";
                return Object.assign({}, device, { newName: normalizedPrefix + separator.value + sequence });
            });
            let error = "";
            if (!selected.length) error = "当前作用范围内没有设备。";
            else if (!normalizedPrefix) error = "名称前缀不能为空。";
            else if (!validStart) error = "起始序号必须是大于等于 0 的整数。";
            else if (names.some(function (item) { return Array.from(item.newName).length > 40; })) error = "生成的新名称超过 40 个字符，请缩短名称前缀。";

            previewBody.replaceChildren();
            names.forEach(function (item) {
                const row = document.createElement("tr");
                [item.currentName, item.systemName, item.newName].forEach(function (value, index) {
                    const cell = document.createElement("td");
                    cell.textContent = value;
                    cell.title = value;
                    if (index === 2 && Array.from(value).length > 40) cell.className = "device-batch-name-invalid";
                    row.appendChild(cell);
                });
                previewBody.appendChild(row);
            });
            count.textContent = selected.length + " 个设备";
            masterField.classList.toggle("hidden", scope.value !== "master");
            submit.disabled = Boolean(error);
            restore.disabled = !selected.length;
            feedback.className = error ? "device-detail-feedback flash flash-error" : "device-detail-feedback hidden";
            feedback.textContent = error;
            return { devices: names, error: error };
        }

        // 显示失败项。
        function showFailures(result) {
            const items = result && Array.isArray(result.failures) ? result.failures : [];
            failures.replaceChildren();
            failures.classList.toggle("hidden", !items.length);
            if (!items.length) return;
            const heading = document.createElement("strong");
            heading.textContent = "失败明细（" + items.length + "）";
            failures.appendChild(heading);
            const list = document.createElement("ul");
            items.forEach(function (item) {
                const entry = document.createElement("li");
                entry.textContent = (item.device_id || "未知设备") + "：" + (item.message || "更新失败");
                list.appendChild(entry);
            });
            failures.appendChild(list);
        }

        // 批量提交设备显示名称并更新操作反馈。
        async function submitItems(items, button, busyText, successText) {
            submit.disabled = true;
            restore.disabled = true;
            const originalText = button.textContent;
            button.textContent = busyText;
            try {
                const response = await csrfFetch("/api/devices/display-names/batch", {
                    method: "PUT",
                    headers: { "Accept": "application/json", "Content-Type": "application/json" },
                    body: JSON.stringify({ items: items })
                });
                const payload = await response.json();
                if (!response.ok || !payload.success) {
                    throw new Error(payload && payload.error ? payload.error.message : "批量设备名称保存失败");
                }
                const result = payload.data || {};
                showFailures(result);
                if (Number(result.failure_count || 0) > 0) {
                    showToast("warning", "已更新 " + Number(result.success_count || 0) + " 个设备，" + Number(result.failure_count || 0) + " 个失败");
                    buildPreview();
                    return;
                }
                showToast("success", successText + Number(result.success_count || 0) + " 个设备名称");
                pageScope.setTimeout(function () { window.location.reload(); }, 350);
            } catch (error) {
                if (error && error.edgeStale) return;
                feedback.className = "device-detail-feedback flash flash-error";
                feedback.textContent = friendlyApiMessage(error.message, "批量设备名称保存失败");
            } finally {
                button.textContent = originalText;
                buildPreview();
            }
        }

        pageScope.listen(openButton, "click", function () {
            failures.classList.add("hidden");
            failures.replaceChildren();
            buildPreview();
            openModal(modal);
            prefix.focus();
            prefix.select();
        });
        modal.querySelectorAll("[data-device-batch-name-close]").forEach(function (button) {
            pageScope.listen(button, "click", function () { closeModal(modal); });
        });
        [scope, master, prefix, start, digits, separator].forEach(function (field) {
            pageScope.listen(field, "input", buildPreview);
            pageScope.listen(field, "change", buildPreview);
        });
        pageScope.listen(form, "submit", function (event) {
            event.preventDefault();
            const preview = buildPreview();
            if (preview.error) return;
            submitItems(preview.devices.map(function (device) {
                return { device_id: device.id, display_name: device.newName };
            }), submit, "正在应用...", "已批量更新 ");
        });
        pageScope.listen(restore, "click", async function () {
            const selected = selectedDevices();
            if (!selected.length) return;
            if (!await confirmAction({
                title: "确认恢复设备名称",
                message: "将清空当前范围内 " + selected.length + " 个设备的自定义名称，并恢复系统推导名称。",
                confirmText: "确认恢复"
            })) return;
            submitItems(selected.map(function (device) {
                return { device_id: device.id, display_name: "" };
            }), restore, "正在恢复...", "已批量恢复 ");
        });
        buildPreview();
    }

    // ---------- 设备详情及受控的主动读写命令 ----------
    // 设备详情弹窗按需加载，避免采集管理首屏携带全部实时点位和命令定义。
    function initDeviceDetailModal() {
        const modal = document.getElementById("device-detail-modal");
        if (!modal) {
            return;
        }

        const title = modal.querySelector("[data-device-detail-title]");
        const subtitle = modal.querySelector("[data-device-detail-subtitle]");
        const feedback = modal.querySelector("[data-device-detail-feedback]");
        const summary = modal.querySelector("[data-device-detail-summary]");
        const condition = modal.querySelector("[data-device-detail-condition]");
        const points = modal.querySelector("[data-device-detail-points]");
        const tabs = modal.querySelector("[data-device-detail-tabs]");
        const tabButtons = Array.from(modal.querySelectorAll("[data-device-detail-tab]"));
        const tabPanels = Array.from(modal.querySelectorAll("[data-device-detail-panel]"));
        const commandsSection = modal.querySelector("[data-device-detail-commands-section]");
        const commands = modal.querySelector("[data-device-detail-commands]");
        let currentDetail = null;
        let deviceDetailPointValues = {};

        function selectDeviceDetailTab(name) {
            tabButtons.forEach(function (button) {
                const active = button.dataset.deviceDetailTab === name;
                button.classList.toggle("is-active", active);
                button.setAttribute("aria-selected", active ? "true" : "false");
                button.tabIndex = active ? 0 : -1;
            });
            tabPanels.forEach(function (panel) {
                panel.classList.toggle("hidden", panel.dataset.deviceDetailPanel !== name);
                if (panel.dataset.deviceDetailPanel === name) panel.scrollTop = 0;
            });
        }

        tabButtons.forEach(function (button) {
            pageScope.listen(button, "click", function () {
                selectDeviceDetailTab(button.dataset.deviceDetailTab || "basic");
            });
        });

        modal.querySelectorAll("[data-device-detail-close]").forEach(function (button) {
            pageScope.listen(button, "click", function () {
                closeModal(modal);
            });
        });

        document.querySelectorAll("[data-device-detail-open]").forEach(function (button) {
            pageScope.listen(button, "click", function () {
                const deviceId = button.dataset.deviceId || "";
                openModal(modal);
                renderDeviceDetailLoading(deviceId);
                loadDeviceDetail(deviceId);
            });
        });

        // 渲染设备详情加载状态。
        function renderDeviceDetailLoading(deviceId) {
            currentDetail = null;
            setDeviceDetailFeedback("", "");
            if (tabs) tabs.classList.add("hidden");
            selectDeviceDetailTab("basic");
            if (title) {
                title.textContent = "设备详情";
            }
            if (subtitle) {
                subtitle.textContent = "正在加载设备详情";
            }
            if (summary) {
                summary.innerHTML = "";
            }
            if (condition) {
                condition.textContent = "正在加载...";
            }
            if (points) {
                points.textContent = "正在加载...";
            }
            if (commandsSection) {
                commandsSection.classList.add("hidden");
            }
            if (commands) {
                commands.innerHTML = "";
            }
        }

        // 加载设备详情和轮询状态，并渲染详情弹窗。
        async function loadDeviceDetail(deviceId) {
            const scope = pageScope;
            try {
                const response = await fetchDeviceDetail(deviceId);
                const payload = await response.json();
                if (!response.ok || !payload.success) {
                    const message = payload && payload.error ? payload.error.message : "设备详情加载失败";
                    throw new Error(friendlyApiMessage(message, "设备详情加载失败"));
                }
                if (!scope.isActive()) return;
                currentDetail = payload.data || {};
                renderDeviceDetail(currentDetail);
            } catch (error) {
                if (!scope.isActive() || EdgeApp.isStalePageError(error)) return;
                setDeviceDetailFeedback("error", friendlyApiMessage(error.message, "设备详情加载失败"));
                if (condition) {
                    condition.textContent = "设备详情加载失败";
                }
                if (points) {
                    points.textContent = "暂无可显示的数据项";
                }
                if (commandsSection) {
                    commandsSection.classList.add("hidden");
                }
            }
        }

        // 请求并加载设备详情。
        function fetchDeviceDetail(deviceId) {
            return pageScope.fetch("/api/devices/" + encodeURIComponent(deviceId) + "/detail", {
                method: "GET",
                headers: { "Accept": "application/json" }
            });
        }

        // 渲染设备详情的全部区块。
        function renderDeviceDetail(detail) {
            if (title) {
                const device = detail.device || {};
                const titleName = device.device_name || (detail.template_name ? (detail.template_name + "设备") : "");
                title.textContent = titleName ? (titleName + "详情") : "设备详情";
            }
            if (subtitle) {
                const status = detail.status || {};
                const onlineText = status.has_status ? (status.online ? "在线" : "离线") : "状态未知";
                subtitle.textContent = (detail.template_name || "未知设备类型") + " / " + onlineText;
            }
            if (summary) {
                summary.innerHTML = renderDeviceDetailSummary(detail);
            }
            if (condition) {
                condition.innerHTML = renderDeviceCondition(detail);
            }
            if (points) {
                points.innerHTML = renderDeviceDetailPoints(detail.points || []);
                animateDeviceDetailPointChanges(detail, points);
                if (window.EdgeMotion) {
                    window.EdgeMotion.reveal(summary);
                    window.EdgeMotion.reveal(condition);
                    window.EdgeMotion.reveal(points);
                }
            }
            renderDeviceCommands(detail);
            selectDeviceDetailTab("basic");
        }

        // 渲染设备详情摘要。
        function renderDeviceDetailSummary(detail) {
            const device = detail.device || {};
            const master = detail.master || {};
            const channel = detail.channel || {};
            const readProfile = detail.read_profile || {};
            const status = detail.status || {};
            const onlineText = status.has_status ? (status.online ? "在线" : "异常") : "状态未知";
            const items = [
                ["设备名称", device.device_name || "未命名设备"],
                ["系统推导名称", device.system_name || device.device_name || "未命名设备"],
                ["所属通道", detail.channel_label || channel.channel_name || channel.channel_id || "-"],
                ["所属主站", detail.master_label || master.master_name || master.master_id || "-"],
                ["设备类型", detail.template_name || "未知设备类型"],
                ["在线状态", onlineText],
                ["设备地址", String(detail.slave_address || master.target_address || "-")],
                ["设备基地址", String(readProfile.true_start_register ?? "-")],
                ["设备地址跨度", String(readProfile.device_address_stride ?? "-")],
                ["读取区块数量", String(Array.isArray(readProfile.read_blocks) ? readProfile.read_blocks.length : "-")]
            ];
            return items.map(function (item) {
                return '<div class="device-detail-summary-item"><span>' + escapeHtml(item[0]) + '</span><strong>' + escapeHtml(item[1]) + "</strong></div>";
            }).join("");
        }

        // 渲染设备当前健康与通讯状态。
        function renderDeviceCondition(detail) {
            const status = detail.status || {};
            const stateClass = status.has_status ? (status.online ? "status-ok" : "status-warn") : "status-neutral";
            const suggestion = diagnosisSuggestion(status.diagnosis);
            const diagnosis = status.diagnosis || {};
            const diagnosisMessage = diagnosis.error_code && String(diagnosis.error_code).toUpperCase() !== "NONE"
                ? (diagnosis.message || status.status_summary || "设备状态异常")
                : (status.status_summary || "暂无采集状态");
            return [
                '<div class="device-condition-line">',
                '<span class="' + stateClass + '">' + escapeHtml(status.has_status ? (status.online ? "在线" : "离线") : "状态未知") + "</span>",
                '<strong>' + escapeHtml(diagnosisMessage) + "</strong>",
                "</div>",
                suggestion ? '<p><b>建议</b><span>' + escapeHtml(suggestion) + "</span></p>" : ""
            ].join("");
        }

        // 渲染设备详情中的实时点位。
        function renderDeviceDetailPoints(points) {
            if (!Array.isArray(points) || !points.length) {
                return '<div class="empty-block">当前设备暂无运行快照。</div>';
            }
            const sorted = points.slice().sort(function (left, right) {
                const leftOrder = Number(left.display_order || 0);
                const rightOrder = Number(right.display_order || 0);
                if (leftOrder !== rightOrder) {
                    return leftOrder - rightOrder;
                }
                return String(left.name || left.key || "").localeCompare(String(right.name || right.key || ""));
            });
            return [
                '<div class="device-detail-table-scroll"><table class="data-table device-detail-point-table">',
                "<thead><tr><th>数据项</th><th>当前值</th><th>质量</th><th>说明</th></tr></thead><tbody>",
                sorted.map(function (point) {
                    const valueText = pointDisplayValue(point);
                    return "<tr>" +
                        "<td>" + escapeHtml(dataItemDisplayName(point.name, point.key)) + "</td>" +
                        '<td class="device-detail-point-value" data-detail-point-key="' + escapeHtml(point.key || point.name || "") +
                        '" data-detail-point-value="' + escapeHtml(valueText) + '" data-detail-point-valid="' +
                        (point.valid === false ? "false" : "true") + '">' + escapeHtml(valueText) + "</td>" +
                        "<td>" + escapeHtml(qualityDisplayText(point.quality)) + "</td>" +
                        "<td>" + escapeHtml(point.message || (point.valid === false ? "数据无效或设备故障" : "-")) + "</td>" +
                        "</tr>";
                }).join(""),
                "</tbody></table></div>"
            ].join("");
        }

        // 详情点位以后端展示值为准，仅对同一设备同一点位的真实变化给出一次性反馈。
        function animateDeviceDetailPointChanges(detail, container) {
            const device = detail.device || {};
            const deviceKey = String(device.device_id || device.id || detail.device_id || "device");
            const nextValues = {};
            let remainingMotion = 8;
            container.querySelectorAll("[data-detail-point-key]").forEach(function (cell) {
                const key = deviceKey + "::" + String(cell.dataset.detailPointKey || "");
                const next = String(cell.dataset.detailPointValue || "");
                const hadPrevious = Object.prototype.hasOwnProperty.call(deviceDetailPointValues, key);
                const previous = hadPrevious ? deviceDetailPointValues[key] : next;
                nextValues[key] = next;
                if (remainingMotion > 0 && hadPrevious && previous !== next &&
                    cell.dataset.detailPointValid !== "false" && window.EdgeMotion) {
                    window.EdgeMotion.markValue(cell, previous, next);
                    remainingMotion -= 1;
                }
            });
            deviceDetailPointValues = nextValues;
        }

        // 格式化点位数值、单位和质量状态。
        function pointDisplayValue(point) {
            if (point.valid === false) {
                return "数据无效";
            }
            const key = point.key || "";
            if ((key === "current_status" || key === "motor_stop_time" || key === "last_test_time") && point.message) {
                return point.message || "-";
            }
            if (key === "running_status_flags") {
                const raw = Number(point.raw_value ?? point.value ?? 0);
                return "0x" + raw.toString(16).toUpperCase().padStart(4, "0");
            }
            const displayText = typeof point.display_text === "string" ? point.display_text.trim() : "";
            if (displayText) {
                const raw = Number(point.raw_value ?? point.value);
                if (Number.isFinite(raw)) {
                    return displayText + "（" + String(Math.trunc(raw)) + "）";
                }
            }
            const value = Number(point.value);
            if (!Number.isFinite(value)) {
                return "-";
            }
            const precision = Number(point.precision || 0);
            const text = value.toFixed(Math.max(0, Math.min(6, precision)));
            return point.unit ? (text + " " + point.unit) : text;
        }

        // 渲染设备支持的操作命令。
        function renderDeviceCommands(detail) {
            if (!commands) {
                return;
            }
            const commandList = detail.write_commands || [];
            if (!commandList.length) {
                commands.innerHTML = "";
                if (tabs) tabs.classList.add("hidden");
                if (commandsSection) {
                    commandsSection.classList.add("hidden");
                }
                selectDeviceDetailTab("basic");
                return;
            }
            if (tabs) tabs.classList.remove("hidden");
            if (commandsSection) {
                commandsSection.classList.remove("hidden");
            }
            commands.innerHTML = renderDeviceCommandGroups(detail, commandList);
            commands.querySelectorAll("[data-device-command-form]").forEach(function (form) {
                pageScope.listen(form, "submit", function (event) {
                    event.preventDefault();
                    executeDeviceCommand(form);
                });
            });
            commands.querySelectorAll("[data-em100-record-read]").forEach(function (button) {
                pageScope.listen(button, "click", function () {
                    readEM100Record(button.dataset.em100RecordRead || "event");
                });
            });
            updateDeviceCommandAvailability();
        }

        // 按命令分组渲染设备操作区。
        function renderDeviceCommandGroups(detail, commandList) {
            const groups = [];
            commandList.forEach(function (command) {
                const groupName = command.group || "设备设置";
                let group = groups.find(function (item) {
                    return item.name === groupName;
                });
                if (!group) {
                    group = { name: groupName, commands: [] };
                    groups.push(group);
                }
                group.commands.push(command);
            });
            if (String(detail.template_id || "").toUpperCase() === "EM100") {
                groups.push({ name: "记录读取", commands: [], recordRead: true });
            }
            const groupOrder = ["系统设置", "遥控命令", "记录维护", "记录读取"];
            groups.sort(function (left, right) {
                const leftIndex = groupOrder.indexOf(left.name);
                const rightIndex = groupOrder.indexOf(right.name);
                const normalizedLeft = leftIndex >= 0 ? leftIndex : groupOrder.length;
                const normalizedRight = rightIndex >= 0 ? rightIndex : groupOrder.length;
                if (normalizedLeft !== normalizedRight) {
                    return normalizedLeft - normalizedRight;
                }
                return String(left.name).localeCompare(String(right.name));
            });
            return groups.map(function (group) {
                const body = group.recordRead
                    ? renderEM100RecordReadGroup()
                    : group.commands.map(renderDeviceCommandCard).join("");
                return '<section class="device-command-group"><h4>' + escapeHtml(group.name) + "</h4>" + body + "</section>";
            }).join("");
        }


        // 构造 EM100 记录读取操作组。
        function renderEM100RecordReadGroup() {
            return [
                '<div class="device-command-card device-record-read-card">',
                '<header><div><h4>手动读取记录</h4><p>读取会消耗设备内一条未读记录，不加入周期轮询。</p></div><span class="toolbar-chip">' + escapeHtml(functionCodeText(3)) + "</span></header>",
                '<div class="modal-action-group">',
                '<button type="button" class="btn btn-secondary btn-small" data-device-command-submit data-em100-record-read="event">读取事件记录</button>',
                '<button type="button" class="btn btn-secondary btn-small" data-device-command-submit data-em100-record-read="test">读取测试记录</button>',
                "</div>",
                '<div class="device-command-result hidden" data-em100-record-result></div>',
                "</div>"
            ].join("");
        }

        // 后端在共享通讯资源边界协调轮询，前端无需改变用户可见轮询状态。
        function canExecuteDeviceCommand() {
            return Boolean(currentDetail);
        }

        // 根据设备状态更新命令控件的可用性。
        function updateDeviceCommandAvailability() {
            if (!commands) {
                return;
            }
            const canExecute = canExecuteDeviceCommand();
            commands.querySelectorAll("[data-device-command-submit]").forEach(function (button) {
                button.disabled = !canExecute;
            });
        }

        // 渲染单条设备命令卡片。
        function renderDeviceCommandCard(command) {
            const fields = command.value_fields || [];
            const buttonText = fields.length ? "保存设置" : "执行";
            const description = String(command.description || "").trim();
            const warnings = (Array.isArray(command.warnings) ? command.warnings : [])
                .map(function (warning) { return String(warning || "").trim(); })
                .filter(Boolean);
            const fieldsHtml = fields.length
                ? '<div class="channel-form-grid">' + fields.map(renderDeviceCommandField).join("") + "</div>"
                : "";
            const warningsHtml = warnings.length
                ? '<ul class="device-command-warnings" aria-label="操作注意事项">' + warnings.map(function (warning) {
                    return "<li>" + escapeHtml(warning) + "</li>";
                }).join("") + "</ul>"
                : "";
            return [
                '<form class="device-command-card" data-device-command-form data-command-key="' + escapeHtml(command.key || "") + '">',
                "<header><div><h4>" + escapeHtml(command.name || "设备设置") + "</h4>",
                description ? '<p class="device-command-description">' + escapeHtml(description) + "</p>" : "",
                "</div>",
                '<button type="submit" class="btn btn-primary btn-small" data-device-command-submit data-default-text="' + escapeHtml(buttonText) + '">' + escapeHtml(buttonText) + "</button></header>",
                warningsHtml,
                fieldsHtml,
                '<div class="device-command-result hidden" data-device-command-result></div>',
                "</form>"
            ].join("");
        }

        // 渲染设备命令的单个输入字段。
        function renderDeviceCommandField(field) {
            const key = escapeHtml(field.key || "");
            const label = escapeHtml(dataItemDisplayName(field.label, field.key));
            if (field.type === "enum") {
                const options = (field.options || []).map(function (option) {
                    return '<option value="' + escapeHtml(option.value) + '">' + escapeHtml(option.label || "未命名选项") + "</option>";
                }).join("");
                return '<label class="form-field"><span class="field-label">' + label + '</span><select name="' + key + '">' + options + "</select></label>";
            }
            return [
                '<label class="form-field"><span class="field-label">' + label + "</span>",
                '<input type="number" name="' + key + '" min="' + escapeHtml(field.min ?? 0) + '" max="' + escapeHtml(field.max ?? 65535) + '" step="1">',
                field.unit ? '<span class="field-hint">单位：' + escapeHtml(field.unit) + "</span>" : "",
                "</label>"
            ].join("");
        }

        // 校验并执行设备写命令，随后展示寄存器写入结果。
        async function executeDeviceCommand(form) {
            if (!currentDetail || !currentDetail.device || !currentDetail.device.device_id) {
                return;
            }
            const commandKey = form.dataset.commandKey || "";
            const command = (currentDetail.write_commands || []).find(function (item) {
                return item.key === commandKey;
            });
            if (!command) {
                renderCommandResult(form, false, "未找到命令定义", null);
                return;
            }
            if (!canExecuteDeviceCommand()) {
                renderCommandResult(form, false, commandPollingMessage(currentDetail), null);
                return;
            }
            if (command.require_confirm) {
                const deviceName = currentDetail.device.display_name || currentDetail.device.name || currentDetail.device.device_id;
                const operationName = command.name || "设备操作";
                const warnings = (Array.isArray(command.warnings) ? command.warnings : [])
                    .map(function (warning) { return String(warning || "").trim(); })
                    .filter(Boolean);
                let confirmMessage = String(command.confirm_text || "").trim() || "确定要执行该操作吗？";
                if (warnings.length) {
                    confirmMessage += "\n\n注意事项：\n" + warnings.map(function (warning) {
                        return "• " + warning;
                    }).join("\n");
                }
                if (!await confirmAction({
                    title: "确认执行操作",
                    message: confirmMessage,
                    confirmText: "确认执行",
                    context: [
                        { label: "设备", value: deviceName },
                        { label: "操作", value: operationName }
                    ]
                })) {
                    return;
                }
            }
            const values = {};
            for (const field of (command.value_fields || [])) {
                const input = form.elements.namedItem(field.key);
                const raw = input ? String(input.value || "").trim() : "";
                if (raw === "") {
                    renderCommandResult(form, false, "请填写 " + dataItemDisplayName(field.label, field.key), null);
                    return;
                }
                const value = Number(raw);
                if (!Number.isInteger(value) || value < 0 || value > 65535) {
                    renderCommandResult(form, false, dataItemDisplayName(field.label, field.key) + " 必须是 0-65535 范围内的整数", null);
                    return;
                }
                values[field.key] = value;
            }

            const button = form.querySelector('button[type="submit"]');
            if (button && button.disabled) return;
            if (button) {
                button.disabled = true;
                button.textContent = "执行中...";
            }
            renderCommandResult(form, true, "命令执行中...", null);
            try {
                const response = await csrfFetch(
                    "/api/devices/" + encodeURIComponent(currentDetail.device.device_id) +
                        "/commands/" + encodeURIComponent(commandKey) + "/execute",
                    {
                        method: "POST",
                        headers: { "Accept": "application/json", "Content-Type": "application/json" },
                        body: JSON.stringify({ values: values })
                    }
                );
                const payload = await response.json();
                if (!response.ok || !payload.success) {
                    const message = payload && payload.error ? payload.error.message : "设备设置保存失败";
                    throw new Error(friendlyApiMessage(message, "设备设置保存失败"));
                }
                const result = payload.data || {};
                const write = result.write_result || {};
                const succeeded = Boolean(result.success && write.success);
                renderCommandResult(form, succeeded, "", result);
                const failureMessage = deviceCommandFailureMessage(write.error_message);
                showToast(
                    succeeded ? "success" : "error",
                    succeeded
                        ? (result.success_hint || "操作执行成功")
                        : failureMessage
                );
            } catch (error) {
                if (error && error.edgeStale) return;
                const message = friendlyApiMessage(error.message, "操作执行失败");
                renderCommandResult(form, false, message, null);
                showToast("error", message);
            } finally {
                if (button) {
                    button.disabled = !canExecuteDeviceCommand();
                    button.textContent = button.dataset.defaultText || "保存设置";
                }
            }
        }

        // 保留后端已经确认的现场错误原因，并统一补充操作失败前缀。
        function deviceCommandFailureMessage(errorMessage) {
            const detail = friendlyApiMessage(errorMessage || "", "未返回具体错误原因");
            return detail.startsWith("操作执行失败：") ? detail : ("操作执行失败：" + detail);
        }

        // 主动读取一条 EM100 事件或测试记录。
        async function readEM100Record(recordType) {
            // EM100 记录读取是一次性设备操作，不加入周期轮询，避免误消费未读记录。
            if (!currentDetail || !currentDetail.device || !currentDetail.device.device_id) {
                return;
            }
            if (!canExecuteDeviceCommand()) {
                renderRecordReadResult(false, commandPollingMessage(currentDetail), null);
                return;
            }
            const endpoint = recordType === "test"
                ? "/api/devices/" + encodeURIComponent(currentDetail.device.device_id) + "/em100/read-test-record"
                : "/api/devices/" + encodeURIComponent(currentDetail.device.device_id) + "/em100/read-event-record";
            commands.querySelectorAll("[data-em100-record-read]").forEach(function (button) {
                button.disabled = true;
            });
            renderRecordReadResult(true, "正在读取记录...", null);
            try {
                const response = await csrfFetch(endpoint, {
                    method: "POST",
                    headers: { "Accept": "application/json" }
                });
                const payload = await response.json();
                if (!response.ok || !payload.success) {
                    const message = payload && payload.error ? payload.error.message : "记录读取失败";
                    throw new Error(friendlyApiMessage(message, "记录读取失败"));
                }
                const result = payload.data || {};
                renderRecordReadResult(Boolean(result.success), "", result);
            } catch (error) {
                if (error && error.edgeStale) return;
                renderRecordReadResult(false, friendlyApiMessage(error.message, "记录读取失败"), null);
            } finally {
                updateDeviceCommandAvailability();
            }
        }

        // 渲染设备记录读取结果。
        function renderRecordReadResult(ok, message, result) {
            const box = commands ? commands.querySelector("[data-em100-record-result]") : null;
            if (!box) {
                return;
            }
            box.className = "device-command-result " + (ok ? "device-command-result-ok" : "device-command-result-bad");
            if (!result) {
                box.innerHTML = escapeHtml(message || "");
                return;
            }
            const read = result.read_result || {};
            const rows = [
                ["读取结果", read.success ? "成功" : "失败"],
                ["记录类型", result.title || recordTypeText(result.record_type)],
                ["未读记录数", String(result.unread_count ?? "-")],
                ["记录有效", result.valid_record ? "有效" : "无未读/无效"],
                ["记录内容", result.content || "-"],
                ["记录数据", result.data_text || "-"],
                ["吸收比/极化值", result.ratio_type ? (result.ratio_type + " " + (result.ratio_value_text || "-")) : "-"],
                ["记录时间", result.record_time || "-"],
                ["原始寄存器", Array.isArray(result.raw_registers) ? result.raw_registers.map(function (value) {
                    return "0x" + Number(value || 0).toString(16).toUpperCase().padStart(4, "0");
                }).join(", ") : "-"],
                ["功能码", functionCodeText(read.function_code || 3)],
                ["起始寄存器", String(read.start_register ?? "-")],
                ["寄存器数量", String(read.register_count ?? "-")],
                ["状态", operationStatusText(read.status)],
                ["错误信息", friendlyApiMessage(read.error_message || "", "-")],
                ["请求帧", read.request_hex || "-"],
                ["响应帧", read.response_hex || "-"]
            ];
            box.innerHTML = rows.map(function (row) {
                const mono = row[0].includes("帧") || row[0].includes("寄存器") ? " trace-hex" : "";
                return '<div><span>' + escapeHtml(row[0]) + '</span><code class="' + mono + '">' + escapeHtml(row[1]) + "</code></div>";
            }).join("");
        }

        // 渲染设备命令执行结果。
        function renderCommandResult(form, ok, message, result) {
            const box = form.querySelector("[data-device-command-result]");
            if (!box) {
                return;
            }
            box.className = "device-command-result " + (ok ? "device-command-result-ok" : "device-command-result-bad");
            if (!result) {
                box.innerHTML = escapeHtml(message || "");
                return;
            }
            const write = result.write_result || {};
            const rows = [
                ["执行结果", write.success ? "执行成功" : deviceCommandFailureMessage(write.error_message)]
            ];
            box.innerHTML = rows.map(function (row) {
                const mono = row[0].includes("帧") ? " trace-hex" : "";
                return '<div><span>' + escapeHtml(row[0]) + '</span><code class="' + mono + '">' + escapeHtml(row[1]) + "</code></div>";
            }).join("");
        }

        // 更新设备详情区域的操作反馈。
        function setDeviceDetailFeedback(kind, message) {
            if (!feedback) {
                return;
            }
            if (!message) {
                feedback.className = "device-detail-feedback hidden";
                feedback.textContent = "";
                return;
            }
            feedback.className = "device-detail-feedback flash flash-" + kind;
            feedback.textContent = message;
            showToast(kind, message);
        }
    }


    // ---------- 通讯报文分页查看与手动刷新 ----------
    // 通讯报文按通道分页刷新；清空动作只影响诊断缓存，不影响历史采集数据。
    function initCommunicationTracePage() {
        const scope = pageScope;
        const root = document.querySelector("[data-communication-traces]");
        if (!root) {
            return;
        }

        const queryUrl = root.dataset.queryUrl || "";
        const clearUrl = root.dataset.clearUrl || "";
        const body = root.querySelector("[data-trace-body]");
        const empty = root.querySelector("[data-trace-empty]");
        const tableShell = root.querySelector("[data-trace-table-shell]");
        const feedback = root.querySelector("[data-trace-feedback]");
        const count = root.querySelector("[data-trace-count]");
        const refreshButton = root.querySelector("[data-trace-refresh]");
        const clearButton = root.querySelector("[data-trace-clear]");

        if (!queryUrl || !body) {
            return;
        }

        let loading = false;

        // 加载当前通道的通讯报文并刷新表格。
        async function loadTraces() {
            if (loading) {
                return;
            }
            loading = true;
            setTraceFeedback(feedback, "", "");
            if (refreshButton) {
                refreshButton.disabled = true;
                refreshButton.textContent = "刷新中...";
            }

            try {
                const response = await scope.fetch(queryUrl, {
                    method: "GET",
                    headers: { "Accept": "application/json" }
                });
                const payload = await response.json();
                if (!response.ok || !payload.success) {
                    const message = payload && payload.error ? payload.error.message : "读取通讯报文失败";
                    throw new Error(friendlyApiMessage(message, "读取通讯报文失败"));
                }

                const data = payload.data || {};
                const records = Array.isArray(data.records) ? data.records : [];
                renderTraceRows(records);
                if (count) {
                    count.textContent = String(records.length);
                }
                setTraceFeedback(feedback, "success", "通讯报文已刷新。");
            } catch (error) {
                if (!scope.isActive() || EdgeApp.isStalePageError(error)) return;
                renderTraceRows([]);
                if (count) {
                    count.textContent = "0";
                }
                setTraceFeedback(feedback, "error", "读取通讯报文失败：" + friendlyApiMessage(error.message, "后端服务不可达"));
            } finally {
                loading = false;
                if (refreshButton) {
                    refreshButton.disabled = false;
                    refreshButton.textContent = "刷新";
                }
            }
        }

        // 渲染通讯报文表格行。
        function renderTraceRows(records) {
            body.replaceChildren();
            const hasRecords = records.length > 0;
            if (empty) {
                empty.classList.toggle("hidden", hasRecords);
            }
            if (tableShell) {
                tableShell.classList.toggle("hidden", !hasRecords);
            }
            if (!hasRecords) {
                return;
            }

            const fragment = document.createDocumentFragment();
            records.forEach(function (record) {
                fragment.appendChild(buildTraceRow(record));
            });
            body.appendChild(fragment);
        }

        // 构造单条通讯报文表格行。
        function buildTraceRow(record) {
            const row = document.createElement("tr");
            const result = String(record.result || "").trim();

            appendTextCell(row, formatTimestamp(record.timestamp_ms), "trace-col-time");
            appendResultCell(row, result);
            appendHexCell(row, record.request_hex || "", "trace-col-request");
            appendHexCell(row, record.response_hex || "", "trace-col-response");
            return row;
        }

        // 向表格行追加纯文本单元格。
        function appendTextCell(row, value, className) {
            const cell = document.createElement("td");
            if (className) {
                cell.className = className;
            }
            cell.textContent = String(value || "-");
            row.appendChild(cell);
            return cell;
        }

        // 向表格行追加十六进制报文单元格。
        function appendHexCell(row, value, className) {
            const cell = document.createElement("td");
            const content = document.createElement("span");
            cell.className = "trace-hex " + className;
            content.className = "trace-hex-value";
            content.textContent = value || "-";
            content.title = value || "-";
            cell.appendChild(content);
            row.appendChild(cell);
        }

        // 向表格行追加通讯结果单元格。
        function appendResultCell(row, result) {
            const cell = document.createElement("td");
            const badge = document.createElement("span");
            const view = traceResultView(result);
            cell.className = "trace-col-result";
            badge.className = "trace-result " + view.className;
            badge.textContent = view.text;
            cell.appendChild(badge);
            row.appendChild(cell);
        }

        // 经用户确认后清空当前通道的内存报文缓存。
        async function clearTraces() {
            if (!clearUrl) {
                return;
            }
            const confirmed = await confirmAction({
                title: "确认清空通讯报文",
                message: "此操作只清空当前通道的内存报文缓存，不影响历史数据、事件和诊断状态。",
                confirmText: "确认清空"
            });
            if (!confirmed) {
                return;
            }

            if (clearButton) {
                clearButton.disabled = true;
                clearButton.textContent = "清空中...";
            }
            setTraceFeedback(feedback, "info", "正在清空当前通道通讯报文。");

            try {
                const response = await csrfFetch(clearUrl, {
                    method: "POST",
                    headers: { "Accept": "application/json" }
                });
                const payload = await response.json();
                if (!response.ok || !payload.success) {
                    const message = payload && payload.error ? payload.error.message : "清空通讯报文失败";
                    throw new Error(friendlyApiMessage(message, "清空通讯报文失败"));
                }
                setTraceFeedback(feedback, "success", "当前通道通讯报文已清空。");
                await loadTraces();
            } catch (error) {
                if (error && error.edgeStale) return;
                setTraceFeedback(feedback, "error", "清空通讯报文失败：" + friendlyApiMessage(error.message, "后端服务不可达"));
            } finally {
                if (clearButton) {
                    clearButton.disabled = false;
                    clearButton.textContent = "清空当前通道报文";
                }
            }
        }

        // 将通讯结果转换为显示文本和样式。
        function traceResultView(result) {
            switch (result) {
            case "success":
                return { text: "成功", className: "status-ok" };
            case "timeout":
                return { text: "超时", className: "status-warn" };
            case "io_error":
                return { text: "通道读写错误", className: "status-bad" };
            case "protocol_error":
                return { text: "协议解析错误", className: "status-bad" };
            case "unknown_error":
                return { text: "未知错误", className: "status-bad" };
            default:
                return { text: result || "未知", className: "status-neutral" };
            }
        }

        if (refreshButton) {
            pageScope.listen(refreshButton, "click", loadTraces);
        }
        if (clearButton) {
            pageScope.listen(clearButton, "click", clearTraces);
        }

        loadTraces();
    }

    // 更新通讯报文区域的操作反馈。
    function setTraceFeedback(container, kind, message) {
        if (!container) {
            return;
        }
        if (!message) {
            container.className = "trace-feedback hidden";
            container.textContent = "";
            return;
        }
        container.className = "trace-feedback flash flash-" + kind;
        container.textContent = message;
        showToast(kind, message);
    }


    // ---------- 通道、主站配置保存与删除 ----------
    // 保存接口会由后端自动应用配置；前端只负责校验、禁止重复提交和展示结果。
    // 通道表单在提交前完成基础格式校验，设备存在性和配置冲突由后端做最终判定。
    function initChannelConfigForms() {
        // 初始化通道类型联动；没有编辑表单时仍需绑定删除操作。
        const forms = document.querySelectorAll(".channel-config-form");
        if (!forms.length) {
            bindChannelDeleteButtons();
            return;
        }

        forms.forEach(function (form) {
            const select = form.querySelector(".serial-candidate-select");
            const typeSelect = form.querySelector("[data-channel-type-select]");
            const submitButton = form.querySelector('button[type="submit"]');

            updateChannelTypeSections(form);
            if (typeSelect) {
                pageScope.listen(typeSelect, "change", function () {
                    updateChannelTypeSections(form);
                });
            }

            // 不可用串口候选项需要用户再次确认后才能保留。
            if (select) {
                let lastValue = select.value;
                pageScope.listen(select, "change", async function () {
                    if (!select.value) {
                        lastValue = select.value;
                        return;
                    }
                    const option = select.options[select.selectedIndex];
                    if (option && option.dataset.available === "false") {
                        const confirmed = await confirmAction({
                            title: "确认使用不可用串口",
                            message: "该串口只是扫描候选项，当前可能不可用。是否仍作为绑定串口保存？",
                            confirmText: "仍然使用"
                        });
                        if (!confirmed) {
                            select.value = lastValue;
                            return;
                        }
                    }
                    lastValue = select.value;
                });
            }

            // 校验表单并提交 JSON，提交期间禁止重复操作。
            pageScope.listen(form, "submit", async function (event) {
                event.preventDefault();

                const payload = buildChannelPayload(form);
                const saveUrl = form.dataset.saveUrl;
                const method = form.dataset.method || "POST";
                const isCreate = method === "POST";
                const validationMessage = validateChannelPayload(payload, isCreate);
                if (validationMessage) {
                    showToast("error", validationMessage, { position: "top-left", autohide: false });
                    return;
                }

                if (submitButton) {
                    submitButton.disabled = true;
                    submitButton.textContent = "应用中...";
                }
                try {
                    const response = await csrfFetch(saveUrl, {
                        method: method,
                        headers: {
                            "Content-Type": "application/json",
                            "Accept": "application/json"
                        },
                        body: JSON.stringify(payload)
                    });

                    const result = await response.json();
                    if (!response.ok || !result.success) {
                        const message = result && result.error ? result.error.message : "保存失败";
                        throw new Error(friendlyApiMessage(message, "保存失败"));
                    }

                    const data = result.data || {};
                    const successMessage = data.warning_message
                        ? (data.message || "保存成功") + "；" + data.warning_message
                        : (data.message || "保存成功");
                    redirectWithFlash("success", successMessage);
                } catch (error) {
                    if (error && error.edgeStale) return;
                    showToast("error", friendlyApiMessage(error.message, "保存失败"), { position: "top-left", autohide: false });
                } finally {
                    if (submitButton) {
                        submitButton.disabled = false;
                        submitButton.textContent = form.dataset.submitLabel || "保存通道";
                    }
                }
            });
        });

        bindChannelDeleteButtons();
    }

    // 绑定通道删除按钮和确认流程。
    function bindChannelDeleteButtons() {
        document.querySelectorAll("[data-channel-delete]").forEach(function (button) {
            pageScope.listen(button, "click", async function () {
                const name = button.dataset.channelName || "该通道";
                if (!await confirmAction({
                    title: "确认删除通道",
                    message: "删除通道“" + name + "”后无法恢复，确定继续吗？",
                    confirmText: "确认删除",
                    danger: true
                })) {
                    return;
                }

                try {
                    const response = await csrfFetch(button.dataset.deleteUrl, {
                        method: "DELETE",
                        headers: { "Accept": "application/json" }
                    });
                    const result = await response.json();
                    if (!response.ok || !result.success) {
                        const message = result && result.error ? result.error.message : "删除失败";
                        throw new Error(friendlyApiMessage(message, "删除失败"));
                    }
                    const data = result.data || {};
                    redirectWithFlash("success", data.message || "通道已删除");
                } catch (error) {
                    if (error && error.edgeStale) return;
                    showToast("error", friendlyApiMessage(error.message, "删除失败"));
                }
            });
        });
    }

    // 主站编辑器根据协议和模板联动字段，但不复制后端的完整业务校验规则。
    function initMasterConfigForms() {
        const modal = document.getElementById("master-editor-modal");
        bindMasterDeleteButtons();
        if (!modal) {
            return;
        }

        const form = modal.querySelector(".master-config-form");
        const submitButton = form.querySelector("[data-master-submit-button]");
        const title = modal.querySelector("[data-master-modal-title]");
        const protocolField = form.querySelector('[name="protocol"]');
        const channelSelect = form.querySelector('[name="channel_id"]');
        const templateInput = form.querySelector('[name="device_template"]');
        const templateSummary = form.querySelector("[data-master-template-summary]");
        const addressPreview = form.querySelector("[data-master-address-preview]");
        const deviceCountInput = form.elements.namedItem("device_count");
        const startRegisterInput = form.elements.namedItem("block_start_register");
        const channelWarning = form.querySelector("[data-master-channel-warning]");
        const baseChannelOptions = Array.from(channelSelect.options).map(function (option) {
            return {
                value: option.value,
                text: option.textContent,
                disabled: option.disabled,
                channelDisabled: option.dataset.channelDisabled || "false",
                channelFault: option.dataset.channelFault || "false",
                channelType: option.dataset.channelType || ""
            };
        });
        const templatePicker = initMasterTemplatePicker(form, function (applyDefaults) {
            if (applyDefaults) applyMasterTemplateDefaults(form);
            updateMasterTemplateSummaryAndPreview(form, templateSummary, addressPreview);
        });

        initMasterModalCloseButtons(modal);
        toggleMasterProtocolFields(form);

        document.querySelectorAll("[data-master-editor-open]").forEach(function (button) {
            pageScope.listen(button, "click", function () {
                openMasterEditor(button);
            });
        });

        if (channelSelect) {
            pageScope.listen(channelSelect, "change", function () {
                updateMasterChannelWarning(channelWarning, channelSelect);
            });
        }
        [deviceCountInput, startRegisterInput].forEach(function (field) {
            if (field) {
                pageScope.listen(field, "input", function () {
                    updateMasterTemplateSummaryAndPreview(form, templateSummary, addressPreview);
                });
            }
        });

        pageScope.listen(form, "submit", async function (event) {
            event.preventDefault();

            if (form.dataset.submitting === "true") {
                return;
            }

            const payload = buildMasterPayload(form);
            const saveUrl = form.dataset.saveUrl;
            const method = form.dataset.method || "POST";
            const validationMessage = validateMasterPayload(payload, form);
            if (validationMessage) {
                showToast("error", validationMessage, { position: "top-left", autohide: false });
                return;
            }

            form.dataset.submitting = "true";
            if (submitButton) {
                submitButton.disabled = true;
                submitButton.textContent = "应用中...";
            }
            try {
                const response = await csrfFetch(saveUrl, {
                    method: method,
                    headers: {
                        "Content-Type": "application/json",
                        "Accept": "application/json"
                    },
                    body: JSON.stringify(payload)
                });

                const result = await response.json();
                if (!response.ok || !result.success) {
                    const message = result && result.error ? result.error.message : "保存失败";
                    throw new Error(friendlyApiMessage(message, "保存失败"));
                }

                const data = result.data || {};
                const successMessage = data.warning_message
                    ? (data.message || "保存成功") + "；" + data.warning_message
                    : (data.message || "保存成功");
                completeMasterSave(successMessage);
                return;
            } catch (error) {
                if (error && error.edgeStale) return;
                showToast("error", friendlyApiMessage(error.message, "保存失败"), { position: "top-left", autohide: false });
            } finally {
                form.dataset.submitting = "false";
                if (submitButton) {
                    submitButton.disabled = false;
                    submitButton.textContent = form.dataset.submitLabel || "保存主站";
                }
            }
        });

        // 完成主站保存后的页面更新与反馈。
        function completeMasterSave(message) {
            closeModal(modal);
            form.reset();
            form.dataset.preservedRemark = "";
            form.dataset.saveUrl = "/api/masters";
            form.dataset.method = "POST";
            showToast("success", message, { position: "top-left" });
            const url = new URL("/collection", window.location.origin);
            url.searchParams.set("flash_type", "success");
            url.searchParams.set("flash_message", message);
            url.hash = "masters";
            window.location.assign(url.toString());
        }

        // 打开主站编辑器并填充现有配置。
        function openMasterEditor(button) {
            form.reset();
            restoreChannelOptions("");
            const mode = button.dataset.mode || "create";
            const masterIdField = form.elements.namedItem("master_id");
            const masterNameField = form.elements.namedItem("master_name");
            const enabledField = form.elements.namedItem("enabled");
            const targetAddressField = form.elements.namedItem("target_address");
            const pollIntervalField = form.elements.namedItem("poll_interval_ms");
            const timeoutField = form.elements.namedItem("response_timeout_ms");
            const retryField = form.elements.namedItem("retry_count");
            const deviceTemplateField = form.elements.namedItem("device_template");
            const blockStartField = form.elements.namedItem("block_start_register");
            const deviceCountField = form.elements.namedItem("device_count");

            form.dataset.saveUrl = button.dataset.saveUrl || "/api/masters";
            form.dataset.method = button.dataset.method || (mode === "edit" ? "PUT" : "POST");
            form.dataset.submitLabel = mode === "edit" ? "保存主站" : "创建主站";

            if (mode === "edit") {
                title.textContent = "编辑主站";
                masterIdField.value = button.dataset.masterId || "";
                masterIdField.readOnly = true;
                masterNameField.value = button.dataset.masterName || "";
                enabledField.checked = button.dataset.enabled !== "false";
                protocolField.value = button.dataset.protocol || "modbus_rtu";
                targetAddressField.value = button.dataset.targetAddress || "1";
                pollIntervalField.value = button.dataset.pollIntervalMs || "1000";
                timeoutField.value = button.dataset.responseTimeoutMs || "1000";
                retryField.value = button.dataset.retryCount || "2";
                form.dataset.preservedRemark = button.dataset.remark || "";
                if (templatePicker) templatePicker.setValue(button.dataset.deviceTemplate || deviceTemplateField.value, false);
                else deviceTemplateField.value = button.dataset.deviceTemplate || deviceTemplateField.value;
                blockStartField.value = button.dataset.blockStartRegister || "0";
                deviceCountField.value = button.dataset.deviceCount || "1";
                restoreChannelOptions(button.dataset.channelId || "");
            } else {
                title.textContent = "新增主站";
                masterIdField.value = "";
                masterIdField.readOnly = false;
                masterNameField.value = "";
                enabledField.checked = true;
                protocolField.value = "modbus_rtu";
                targetAddressField.value = "1";
                pollIntervalField.value = "1000";
                timeoutField.value = "1000";
                retryField.value = "2";
                form.dataset.preservedRemark = "";
                deviceCountField.value = button.dataset.deviceCount || "1";
                if (templatePicker) templatePicker.setValue("", true);
                else applyMasterTemplateDefaults(form);
                restoreChannelOptions("");
            }

            toggleMasterProtocolFields(form);
            updateMasterChannelWarning(channelWarning, channelSelect);
            updateMasterTemplateSummaryAndPreview(form, templateSummary, addressPreview);
            openModal(modal);
        }

        // 恢复主站编辑器中的通道候选项。
        function restoreChannelOptions(selectedValue) {
            channelSelect.innerHTML = "";
            baseChannelOptions.forEach(function (optionData) {
                const option = document.createElement("option");
                option.value = optionData.value;
                option.textContent = optionData.text;
                option.disabled = optionData.disabled;
                option.dataset.channelDisabled = optionData.channelDisabled;
                option.dataset.channelFault = optionData.channelFault;
                option.dataset.channelType = optionData.channelType;
                channelSelect.appendChild(option);
            });

            if (!selectedValue) {
                channelSelect.value = "";
                return;
            }

            const exists = Array.from(channelSelect.options).some(function (option) {
                return option.value === selectedValue;
            });
            if (!exists) {
                const option = document.createElement("option");
                option.value = selectedValue;
                option.textContent = selectedValue + "（当前绑定已失效）";
                option.dataset.invalidBinding = "true";
                channelSelect.appendChild(option);
            }
            channelSelect.value = selectedValue;
        }
    }

    // 初始化主站设备类型选择器。
    function initMasterTemplatePicker(form, onSelection) {
        const root = form.querySelector("[data-master-template-picker]");
        if (!root) return null;
        const input = root.querySelector('[name="device_template"]');
        const trigger = root.querySelector("[data-master-template-trigger]");
        const panel = root.querySelector("[data-master-template-panel]");
        const search = panel.querySelector("[data-master-template-search]");
        const optionsContainer = panel.querySelector("[data-master-template-options]");
        const options = Array.from(panel.querySelectorAll("[data-template-id]"));
        const count = panel.querySelector("[data-master-template-count]");
        const empty = panel.querySelector("[data-master-template-empty]");
        const selectedName = trigger.querySelector("[data-master-template-selected-name]");
        const selectedID = trigger.querySelector("[data-master-template-selected-id]");
        let activeFilter = "all";
        let touchStartX = 0;
        let touchStartY = 0;
        let touchScrolled = false;
        let suppressOptionClickUntil = 0;

        document.body.appendChild(panel);

        // 返回当前选中的设备类型选项。
        function selectedOption() {
            return options.find(function (option) { return option.dataset.templateId === input.value; }) || null;
        }

        // 渲染设备类型当前选择结果。
        function renderSelection() {
            const selected = selectedOption();
            options.forEach(function (option) {
                const active = option === selected;
                option.classList.toggle("is-selected", active);
                option.setAttribute("aria-selected", active ? "true" : "false");
            });
            if (selected) {
                ["templateName", "templateKind", "defaultStartRegister", "readBlockCount", "readFunctionCodes", "deviceAddressStride"].forEach(function (key) {
                    input.dataset[key] = selected.dataset[key] || "";
                });
                selectedName.textContent = selected.dataset.templateName || selected.dataset.templateId;
                selectedID.textContent = selected.dataset.templateId + " · " + (selected.dataset.templateKind === "builtin" ? "内置设备类型" : "自定义设备类型");
            } else if (input.value) {
                input.dataset.templateName = "";
                selectedName.textContent = input.value;
                selectedID.textContent = "当前绑定的设备类型不可用，请重新选择";
            } else {
                selectedName.textContent = "请选择设备类型";
                selectedID.textContent = "支持名称或模板 ID 搜索";
            }
        }

        // 按搜索关键字筛选设备类型选项。
        function filterOptions(resetScroll) {
            const keyword = String(search.value || "").trim().toLocaleLowerCase();
            const kindFiltered = options.filter(function (option) {
                return activeFilter === "all" || option.dataset.templateKind === activeFilter;
            });
            const visibleOptions = kindFiltered.filter(function (option) {
                const haystack = ((option.dataset.templateName || "") + " " + (option.dataset.templateId || "")).toLocaleLowerCase();
                return !keyword || haystack.includes(keyword);
            });
            const visibleSet = new Set(visibleOptions);
            options.forEach(function (option) {
                option.hidden = !visibleSet.has(option);
            });
            count.textContent = "显示 " + visibleOptions.length + " / " + options.length + " 个设备类型";
            empty.hidden = visibleOptions.length > 0;
            if (resetScroll === true) optionsContainer.scrollTop = 0;
        }

        // 更新设备类型筛选状态提示。
        function renderActiveFilter() {
            panel.querySelectorAll("[data-master-template-filter]").forEach(function (button) {
                const active = button.dataset.masterTemplateFilter === activeFilter;
                button.classList.toggle("is-active", active);
                button.setAttribute("aria-pressed", active ? "true" : "false");
            });
        }

        // 将选择面板定位到触发控件附近。
        function positionPanel() {
            const margin = 8;
            const rect = trigger.getBoundingClientRect();
            const keyboardHeight = document.body.classList.contains("virtual-keyboard-open")
                ? Math.max(0, parseFloat(getComputedStyle(document.documentElement).getPropertyValue("--virtual-keyboard-height")) || 0)
                : 0;
            const visibleBottom = Math.max(margin + 220, window.innerHeight - keyboardHeight - margin);
            const width = Math.min(Math.max(rect.width, 420), window.innerWidth - margin * 2);
            const left = Math.min(Math.max(margin, rect.left), window.innerWidth - width - margin);
            const below = visibleBottom - rect.bottom;
            const above = Math.min(rect.top, visibleBottom) - margin;
            const openAbove = below < 330 && above > below;
            const available = Math.max(220, Math.min(520, openAbove ? above : below, visibleBottom - margin));
            panel.style.width = width + "px";
            panel.style.left = left + "px";
            panel.style.maxHeight = available + "px";
            optionsContainer.style.maxHeight = Math.max(100, available - 150) + "px";
            const preferredTop = openAbove ? rect.top - available : rect.bottom + 6;
            panel.style.top = Math.max(margin, Math.min(preferredTop, visibleBottom - available)) + "px";
            panel.classList.toggle("opens-up", openAbove);
        }

        // 打开当前选择面板。
        function open() {
            search.value = "";
            activeFilter = "all";
            renderActiveFilter();
            panel.hidden = false;
            trigger.setAttribute("aria-expanded", "true");
            filterOptions(true);
            positionPanel();
            search.focus({ preventScroll: true });
            const selected = selectedOption();
            if (selected && !selected.hidden) selected.scrollIntoView({ block: "nearest" });
        }

        // 关闭当前选择面板。
        function close() {
            panel.hidden = true;
            trigger.setAttribute("aria-expanded", "false");
        }

        // 设置当前组件的选中值。
        function setValue(value, applyDefaults) {
            let requested = String(value || "");
            if (!requested && options.length) requested = options[0].dataset.templateId || "";
            input.value = requested;
            renderSelection();
            if (typeof onSelection === "function") onSelection(applyDefaults === true);
        }

        pageScope.listen(trigger, "click", function () { if (panel.hidden) open(); else close(); });
        pageScope.listen(search, "input", function () { filterOptions(true); });
        panel.querySelectorAll("[data-master-template-filter]").forEach(function (button) {
            pageScope.listen(button, "click", function () {
                activeFilter = button.dataset.masterTemplateFilter || "all";
                renderActiveFilter();
                filterOptions(true);
            });
        });
        options.forEach(function (option) {
            pageScope.listen(option, "click", function () {
                if (Date.now() < suppressOptionClickUntil) return;
                setValue(option.dataset.templateId || "", true);
                close();
                trigger.focus();
            });
        });
        pageScope.listen(optionsContainer, "touchstart", function (event) {
            if (!event.touches || !event.touches[0]) return;
            touchStartX = event.touches[0].clientX;
            touchStartY = event.touches[0].clientY;
            touchScrolled = false;
        }, { passive: true });
        pageScope.listen(optionsContainer, "touchmove", function (event) {
            if (!event.touches || !event.touches[0]) return;
            if (Math.abs(event.touches[0].clientX - touchStartX) > 8 ||
                Math.abs(event.touches[0].clientY - touchStartY) > 8) {
                touchScrolled = true;
            }
        }, { passive: true });
        pageScope.listen(optionsContainer, "touchend", function () {
            if (touchScrolled) suppressOptionClickUntil = Date.now() + 450;
            touchScrolled = false;
        }, { passive: true });
        pageScope.listen(document, "pointerdown", function (event) {
            if (event.target.closest && event.target.closest(".virtual-keyboard")) return;
            if (!panel.hidden && !panel.contains(event.target) && !root.contains(event.target)) close();
        });
        pageScope.listen(document, "keydown", function (event) {
            if (event.key === "Escape" && !panel.hidden) {
                event.preventDefault();
                event.stopImmediatePropagation();
                close();
                trigger.focus();
            }
        }, true);
        pageScope.listen(window, "resize", function () { if (!panel.hidden) positionPanel(); });
        pageScope.listen(window, "scroll", function () { if (!panel.hidden) positionPanel(); }, true);
        pageScope.listen(window, "edge:virtual-keyboard-layout", function () {
            if (!panel.hidden) window.requestAnimationFrame(positionPanel);
        });

        renderSelection();
        return { setValue: setValue, selectedOption: selectedOption, close: close };
    }

    // 返回主站表单当前选中的设备类型。
    function masterTemplateSelection(form) {
        const input = form.querySelector('[name="device_template"]');
        if (!input || !input.value || !input.dataset.templateName) return null;
        return input;
    }

    // 应用主站设备类型默认值。
    function applyMasterTemplateDefaults(form) {
        const blockStartField = form.elements.namedItem("block_start_register");
        const selected = masterTemplateSelection(form);
        if (!selected) {
            return;
        }
        if (blockStartField) {
            blockStartField.value = selected.dataset.defaultStartRegister || "0";
        }
    }

    // 绑定主站删除按钮。
    function bindMasterDeleteButtons() {
        document.querySelectorAll("[data-master-delete]").forEach(function (button) {
            pageScope.listen(button, "click", async function () {
                const name = button.dataset.masterName || "该主站";
                if (!await confirmAction({
                    title: "确认删除主站",
                    message: "删除主站“" + name + "”后无法恢复，确定继续吗？",
                    confirmText: "确认删除",
                    danger: true
                })) {
                    return;
                }

                try {
                    const response = await csrfFetch(button.dataset.deleteUrl, {
                        method: "DELETE",
                        headers: { "Accept": "application/json" }
                    });
                    const result = await response.json();
                    if (!response.ok || !result.success) {
                        const message = result && result.error ? result.error.message : "删除失败";
                        throw new Error(friendlyApiMessage(message, "删除失败"));
                    }
                    const data = result.data || {};
                    redirectWithFlash("success", data.message || "主站已删除");
                } catch (error) {
                    if (error && error.edgeStale) return;
                    showToast("error", friendlyApiMessage(error.message, "删除失败"));
                }
            });
        });
    }



    // 绑定主站编辑弹窗的关闭按钮。
    function initMasterModalCloseButtons(modal) {
        modal.querySelectorAll("[data-master-modal-close]").forEach(function (button) {
            pageScope.listen(button, "click", function () {
                closeModal(modal);
            });
        });
    }



    // ---------- 表单序列化与前端快速校验 ----------
    // 后端仍执行最终校验，不能把这里的检查视为安全边界。
    function buildChannelPayload(form) {
        return {
            channel_id: readFieldValue(form, "channel_id"),
            channel_name: readFieldValue(form, "channel_name"),
            enabled: readCheckboxValue(form, "enabled"),
            channel_type: readFieldValue(form, "channel_type") || "modbus_rtu_serial",
            port_name: readFieldValue(form, "port_name"),
            tcp_host: readFieldValue(form, "tcp_host"),
            tcp_port: readNumberValue(form, "tcp_port") || 502,
            connect_timeout_ms: readNumberValue(form, "connect_timeout_ms") || 3000,
            baud_rate: readNumberValue(form, "baud_rate"),
            data_bits: readNumberValue(form, "data_bits"),
            parity: readFieldValue(form, "parity"),
            stop_bits: readNumberValue(form, "stop_bits"),
            response_timeout_ms: readNumberValue(form, "response_timeout_ms"),
            retry_count: readNumberValue(form, "retry_count")
        };
    }

    // 构建主站请求数据。
    function buildMasterPayload(form) {
        return {
            master_id: readFieldValue(form, "master_id"),
            master_name: readFieldValue(form, "master_name"),
            enabled: readCheckboxValue(form, "enabled"),
            protocol: readFieldValue(form, "protocol") || "modbus_rtu",
            channel_id: readFieldValue(form, "channel_id"),
            target_address: readNumberValue(form, "target_address"),
            poll_interval_ms: readNumberValue(form, "poll_interval_ms"),
            response_timeout_ms: readNumberValue(form, "response_timeout_ms"),
            retry_count: readNumberValue(form, "retry_count"),
            remark: form.dataset.preservedRemark || "",
            device_template: readFieldValue(form, "device_template"),
            block_start_register: readNumberValue(form, "block_start_register"),
            device_count: readNumberValue(form, "device_count")
        };
    }

    // 校验通道请求数据。
    function validateChannelPayload(payload, isCreate) {
        if (isCreate && !payload.channel_id) {
            return "通道 ID 不能为空";
        }
        if (/\s/.test(payload.channel_id || "")) {
            return "通道 ID 不能包含空白字符";
        }
        if (!payload.channel_name) {
            return "通道名称不能为空";
        }
        if (payload.channel_type === "modbus_rtu_serial") {
            if (!payload.port_name) {
                return "串口路径不能为空";
            }
            if (!String(payload.port_name).startsWith("/dev/")) {
                return "串口路径必须以 /dev/ 开头";
            }
        } else if (payload.channel_type === "modbus_tcp") {
            if (!payload.tcp_host) {
                return "TCP 远端地址不能为空";
            }
            if (/\s/.test(payload.tcp_host)) {
                return "TCP 远端地址不能包含空白字符";
            }
            if (payload.tcp_port < 1 || payload.tcp_port > 65535) {
                return "TCP 远端端口必须在 1-65535 之间";
            }
            if (payload.connect_timeout_ms < 1 || payload.connect_timeout_ms > 60000) {
                return "TCP 建连超时时间必须在 1-60000 ms 之间";
            }
        } else {
            return "请选择通道类型";
        }
        if (payload.response_timeout_ms < 1 || payload.response_timeout_ms > 60000) {
            return "超时时间必须在 1-60000 ms 之间";
        }
        if (payload.retry_count < 0 || payload.retry_count > 10) {
            return "重试次数必须在 0-10 之间";
        }
        return "";
    }

    // 更新通道类型区域。
    function updateChannelTypeSections(form) {
        if (!form) {
            return;
        }
        const channelType = readFieldValue(form, "channel_type") || "modbus_rtu_serial";
        form.querySelectorAll("[data-channel-type-section]").forEach(function (section) {
            const expectedType = section.dataset.channelTypeSection || "";
            const visible = expectedType === channelType;
            section.classList.toggle("hidden", !visible);
            section.hidden = !visible;
            section.setAttribute("aria-hidden", visible ? "false" : "true");
        });
    }

    // 校验主站请求数据。
    function validateMasterPayload(payload, form) {
        if (!payload.master_id) {
            return "主站 ID 不能为空";
        }
        if (!payload.master_name) {
            return "主站名称不能为空";
        }
        if (!payload.protocol) {
            return "请选择协议类型";
        }
        if (payload.protocol !== "modbus_rtu" && payload.protocol !== "modbus_tcp") {
            return "当前采集链路仅接受 Modbus RTU 或 Modbus TCP 主站";
        }
        if (!payload.device_template) {
            return "请选择设备类型";
        }
        if (!payload.channel_id) {
            return payload.protocol === "modbus_tcp" ? "Modbus TCP 主站必须选择关联通道" : "Modbus RTU 主站必须选择关联通道";
        }
        if (payload.protocol === "modbus_rtu" || payload.protocol === "modbus_tcp") {
            const channelSelect = form ? form.querySelector('[name="channel_id"]') : null;
            const selectedChannel = channelSelect ? channelSelect.options[channelSelect.selectedIndex] : null;
            if (selectedChannel && selectedChannel.dataset.invalidBinding === "true") {
                return "当前绑定通道已失效，请重新选择可用通道";
            }
            if (selectedChannel && selectedChannel.dataset.channelDisabled === "true") {
                return "所选通道已禁用，请先在采集管理的通道配置中启用该通道";
            }
            const selectedChannelType = selectedChannel ? selectedChannel.dataset.channelType : "";
            if (payload.protocol === "modbus_rtu" && selectedChannelType && selectedChannelType !== "modbus_rtu_serial") {
                return "Modbus RTU 主站必须绑定 RTU 串口通道";
            }
            if (payload.protocol === "modbus_tcp" && selectedChannelType && selectedChannelType !== "modbus_tcp") {
                return "Modbus TCP 主站必须绑定 TCP 通道";
            }
        }
        if (payload.target_address < 1 || payload.target_address > 247) {
            return "目标地址必须在 1-247 范围内";
        }
        if (payload.poll_interval_ms < 1 || payload.poll_interval_ms > 3600000) {
            return "轮询周期必须在 1-3600000 ms 之间";
        }
        if (payload.response_timeout_ms < 1 || payload.response_timeout_ms > 60000) {
            return "超时时间必须在 1-60000 ms 之间";
        }
        if (payload.retry_count < 0 || payload.retry_count > 10) {
            return "重试次数必须在 0-10 之间";
        }
        if (!Number.isInteger(payload.device_count) || payload.device_count < 1 || payload.device_count > 256) {
            return "设备数量必须为 1-256 的整数";
        }
        return "";
    }

    // 切换主站协议字段。
    function toggleMasterProtocolFields(form) {
        const channelField = form.querySelector("[data-master-channel-field]");
        const channelSelect = form.querySelector('[name="channel_id"]');
        const protocolInput = form.querySelector('[name="protocol"]');

        if (channelField) {
            channelField.classList.remove("hidden");
        }
        if (channelSelect) {
            channelSelect.required = true;
            channelSelect.disabled = false;
        }
    }

    // 更新主站通道警告。
    function updateMasterChannelWarning(container, channelSelect) {
        if (!container || !channelSelect || channelSelect.disabled) {
            if (container) {
                container.classList.add("hidden");
                container.textContent = "";
            }
            return;
        }

        const selected = channelSelect.options[channelSelect.selectedIndex];
        if (selected && selected.dataset.invalidBinding === "true") {
            container.className = "channel-form-notes status-warn";
            container.textContent = "当前主站绑定的通道已不在有效配置中，请重新选择可用通道后保存。";
            return;
        }
        if (selected && selected.dataset.channelDisabled === "true") {
            container.className = "channel-form-notes status-warn";
            container.textContent = "所选通道已禁用，请先在采集管理的通道配置中启用该通道，或改选其他可用通道。";
            return;
        }

        const hasAvailableChannel = Array.from(channelSelect.options).some(function (option) {
            return option.value && option.dataset.channelDisabled !== "true";
        });
        if (!hasAvailableChannel) {
            container.className = "channel-form-notes status-warn";
            container.textContent = "当前没有可用通道，请先在采集管理的通道配置中新增或启用通道。";
            return;
        }

        container.classList.add("hidden");
        container.textContent = "";
    }

    // 更新主站设备类型摘要和寄存器预览。
    function updateMasterTemplateSummaryAndPreview(form, summaryContainer, previewContainer) {
        if (!form) {
            return;
        }
        const selected = masterTemplateSelection(form);
        const deviceCountRaw = readFieldValue(form, "device_count");
        const deviceCount = Number(deviceCountRaw);
        const startRegisterRaw = readFieldValue(form, "block_start_register");
        const startRegister = Number(startRegisterRaw);
        if (!selected) {
            if (summaryContainer) {
                summaryContainer.textContent = "设备类型不可用，请重新选择。";
            }
            if (previewContainer) {
                previewContainer.className = "channel-form-notes status-warn";
                previewContainer.textContent = "无法预览设备基地址。";
            }
            return;
        }
        const blockCount = Number(selected.dataset.readBlockCount || 0);
        const stride = Number(selected.dataset.deviceAddressStride || 0);
        const functionCodes = Array.from(new Set((selected.dataset.readFunctionCodes || "")
            .split(",")
            .map(function (value) { return Number(value); })
            .filter(function (value) { return value === 3 || value === 4; })))
            .map(function (value) { return "FC" + String(value).padStart(2, "0"); });
        if (summaryContainer) {
            summaryContainer.textContent = "读取区块：" + blockCount + " 个；功能码：" +
                (functionCodes.length ? functionCodes.join(" + ") : "未配置") +
                "；设备地址跨度：" + stride;
        }
        if (!previewContainer) {
            return;
        }
        if (deviceCountRaw === "" || !Number.isInteger(deviceCount) || deviceCount <= 0 || deviceCount > 256 ||
                startRegisterRaw === "" || !Number.isInteger(startRegister) || startRegister < 0 ||
                !Number.isInteger(stride) || stride <= 0) {
            previewContainer.className = "channel-form-notes status-warn";
            previewContainer.textContent = "填写有效的设备数量和首台设备基地址后可预览地址。";
            return;
        }
        const preview = ["设备 1 基地址：" + startRegister];
        if (deviceCount >= 2) {
            const secondAddress = startRegister + stride;
            preview.push(secondAddress <= 65535
                ? "设备 2 基地址：" + secondAddress
                : "设备 2 基地址超出 Modbus 地址空间");
        }
        previewContainer.className = "channel-form-notes" +
            (preview.some(function (text) { return text.includes("超出"); }) ? " status-warn" : "");
        previewContainer.textContent = preview.join("；");
    }



    // 读取数值值。
    function readNumberValue(form, name) {
        const rawValue = readFieldValue(form, name);
        return Number(rawValue || 0);
    }

    // 读取复选框值。
    function readCheckboxValue(form, name) {
        const field = form.elements.namedItem(name);
        return Boolean(field && field.checked);
    }



    // 将操作结果带回当前页面，由服务端统一渲染提示。
    function redirectWithFlash(type, message) {
        const url = new URL(window.location.href);
        url.searchParams.set("flash_type", type);
        url.searchParams.set("flash_message", message);
        window.location.assign(url.toString());
    }



    // 格式化诊断时间戳。
    function formatDiagnosisTimestamp(value) {
        if (!value) {
            return "暂无记录";
        }
        return formatTimestamp(value);
    }



    // 返回 Modbus 功能码的中文说明。
    function functionCodeText(code) {
        const value = Number(code);
        if (!Number.isFinite(value) || value <= 0) {
            return "-";
        }
        return "FC" + Math.trunc(value).toString(16).toUpperCase().padStart(2, "0");
    }



    // 返回采集质量的中文说明。
    function qualityDisplayText(value) {
        const text = String(value || "").trim();
        switch (text.toLowerCase()) {
        case "good":
            return "良好";
        case "bad":
            return "异常";
        case "stale":
            return "过期";
        case "":
        case "-":
        case "unknown":
            return "暂无数据";
        default:
            return /[^\x00-\x7F]/.test(text) ? text : "未知";
        }
    }

    // 返回通信操作状态的中文说明。
    function operationStatusText(value) {
        const text = String(value || "").trim();
        switch (text.toLowerCase()) {
        case "success":
        case "ok":
        case "completed":
            return "成功";
        case "failed":
        case "failure":
        case "error":
            return "失败";
        case "timeout":
            return "超时";
        case "running":
            return "运行中";
        case "stopped":
            return "已停止";
        case "pending":
            return "等待中";
        case "":
        case "-":
        case "unknown":
            return "-";
        default:
            return /[^\x00-\x7F]/.test(text) ? text : "未知";
        }
    }

    // 返回通讯记录类型的中文说明。
    function recordTypeText(value) {
        const text = String(value || "").trim();
        switch (text.toLowerCase()) {
        case "event":
            return "事件记录";
        case "test":
            return "测试记录";
        case "":
            return "记录";
        default:
            return /[^\x00-\x7F]/.test(text) ? text : "记录";
        }
    }

    EdgeApp.registerPageController("collection", ["collection"], { mount: mount });
})();
