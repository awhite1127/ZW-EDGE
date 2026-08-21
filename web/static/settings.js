// 系统设置页控制器：管理时间、网络与 MQTT 配置。
// 保存值与实际运行态分开渲染，避免把“已持久化但尚未应用”误认为当前系统状态。
(function () {
    "use strict";

    const EdgeApp = window.EdgeApp;
    if (!EdgeApp) return;

    const showToast = EdgeApp.showToast;
    const friendlyApiMessage = EdgeApp.friendlyApiMessage;
    const formatTimestamp = EdgeApp.formatTimestamp;
    const readFieldValue = EdgeApp.readFieldValue;
    const setFeedback = EdgeApp.setFeedback;
    const isPageHidden = EdgeApp.isPageHidden || function () { return false; };
    const onPageVisibilityChange = EdgeApp.onPageVisibilityChange || function () { return false; };
    const confirmAction = EdgeApp.confirmAction;
    let settingsRuntimeRefreshPaused = false;
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
        settingsRuntimeRefreshPaused = false;
        initSettingsRuntimeRefresh();
        initTimeSettingsControls();
        initNetworkSaveApplyActions();
        initMqttTlsHint();
    }

    // 时间、网络和 MQTT 运行态共用一个刷新入口，保证页面摘要来自相近采样时刻。
    function initSettingsRuntimeRefresh() {
        const scope = pageScope;
        const root = document.querySelector("[data-settings-runtime-page]");
        if (!root) {
            return;
        }
        const endpoint = root.dataset.settingsRuntimeUrl || "/api/settings/runtime-status";
        let inFlight = false;
        let timer = null;

        const stopTimer = function () {
            if (timer !== null) {
                window.clearTimeout(timer);
                timer = null;
            }
        };
        const schedule = function () {
            stopTimer();
            if (!isPageHidden()) {
                timer = window.setTimeout(refresh, 5000);
            }
        };

        const refresh = async function () {
            timer = null;
            if (isPageHidden()) {
                return;
            }
            if (inFlight || settingsRuntimeRefreshPaused) {
                schedule();
                return;
            }
            inFlight = true;
            try {
                const response = await scope.fetch(endpoint, {
                    headers: {
                        "Accept": "application/json",
                        "X-Requested-With": "XMLHttpRequest"
                    }
                });
                const payload = await response.json();
                if (!scope.isActive()) return;
                if (!response.ok || !payload.success) {
                    throw new Error(payload && payload.error && payload.error.message ? payload.error.message : "状态刷新失败");
                }
                updateSettingsRuntimeStatus(payload.data || {});
            } catch (_error) {
                if (!scope.isActive() || EdgeApp.isStalePageError(_error)) return;
                markSettingsRuntimeUnavailable();
            } finally {
                inFlight = false;
                if (scope.isActive()) schedule();
            }
        };

        scope.onVisibilityChange(function () {
            if (isPageHidden()) {
                stopTimer();
            } else {
                refresh();
            }
        });
        scope.listen(window, "pagehide", stopTimer);
        scope.listen(window, "pageshow", function (event) {
            if (event.persisted) refresh();
        });
        scope.onDispose(stopTimer);
        schedule();
    }

    // 使用最新快照更新设置页运行状态。
    function updateSettingsRuntimeStatus(data) {
        updateNetworkRuntimeStatus(data.network_runtime || {}, data.network_runtime_available !== false, data.network_runtime_error || "");
        updateMqttRuntimeStatus(data.mqtt_runtime || {}, data.mqtt_runtime_available !== false, data.mqtt_runtime_error || "");
        updateTimeRuntimeStatus(data.time_runtime || {}, data.time_runtime_available !== false, data.time_runtime_error || "");
    }

    // 更新设置页的时间服务运行状态。
    function updateTimeRuntimeStatus(runtime, available, errorMessage) {
        const state = document.querySelector("[data-settings-time-state]");
        const summary = document.querySelector("[data-settings-time-summary]");
        const modeControl = document.querySelector("[data-time-mode-control]");
        if (modeControl) modeControl.dataset.runtimeAvailable = available ? "true" : "false";
        if (!available) {
            setStatusText(state, "状态不可用", "status-bad");
            if (summary) summary.textContent = friendlyApiMessage(errorMessage, "板端时间状态暂时无法获取");
            updateTimeModeFields();
            return;
        }
        const stateClass = runtime.settings_pending_apply ? "status-warn" : (runtime.sync_state === "synchronized" ? "status-ok" :
            (runtime.sync_state === "error" || runtime.sync_state === "takeover_failed" ? "status-bad" :
                (runtime.sync_state === "unsynchronized" || runtime.sync_state === "external" ? "status-warn" : "status-neutral")));
        setStatusText(state, runtime.settings_pending_apply ? "设置待应用" : (runtime.sync_state_text || "状态未知"), stateClass);
        if (summary) summary.textContent = [runtime.current_time_text, displayTimezone(runtime.timezone), runtime.utc_offset_text].filter(Boolean).join(" · ");
        setText("[data-time-runtime-current]", defaultDisplayText(runtime.current_time_text));
        setText("[data-time-runtime-zone]", [displayTimezone(runtime.timezone), runtime.utc_offset_text].filter(Boolean).join(" ") || "-");
        setText("[data-time-runtime-process]", runtime.ntp_process_state_text || (runtime.ntp_process_running ? "NTP 正在运行" : "NTP 未运行"));
        setText("[data-settings-time-process-summary]", runtime.ntp_process_state_text || (runtime.ntp_process_running ? "NTP 正在运行" : "NTP 未运行"));
        setText("[data-time-runtime-rtc]", runtime.rtc_available ? defaultDisplayText(runtime.rtc_time_text) : "RTC 不可用");
        setText("[data-time-runtime-last-sync]", runtime.last_sync_time_ms ? formatTimestamp(runtime.last_sync_time_ms) : "暂无");
        const processNotice = document.querySelector("[data-time-process-notice]");
        if (processNotice) {
            const processState = runtime.ntp_process_state || "stopped";
            const visible = processState === "external" || processState === "taking_over" || processState === "takeover_failed";
            processNotice.textContent = visible ? (runtime.ntp_process_state_text || "NTP 运行状态需要检查") : "";
            processNotice.classList.toggle("hidden", !visible);
            processNotice.classList.toggle("status-bad", processState === "takeover_failed");
            processNotice.classList.toggle("status-warn", processState !== "takeover_failed");
        }
        const pendingWarning = document.querySelector("[data-time-pending-warning]");
        if (pendingWarning) pendingWarning.classList.toggle("hidden", !runtime.settings_pending_apply);
        const runtimeError = document.querySelector("[data-time-runtime-error]");
        if (runtimeError) {
            runtimeError.textContent = runtime.last_error_message || "";
            runtimeError.classList.toggle("hidden", !runtime.last_error_message);
        }
        if (modeControl) modeControl.dataset.settingsPending = runtime.settings_pending_apply ? "true" : "false";
        updateTimeModeFields();
    }

    // 自动校时与手动校时互斥；界面禁用只用于引导，后端仍负责权限和状态校验。
    function initTimeSettingsControls() {
        // 跟踪设置表单是否有未保存修改，并联动时间模式控件。
        const timeForm = document.querySelector("[data-time-settings-form]");
        const syncForm = document.querySelector("[data-time-sync-form]");
        const modeInputs = document.querySelectorAll("[data-time-mode]");
        const manualForm = document.querySelector("[data-manual-time-form]");
        if (timeForm) {
            timeForm.dataset.dirty = "false";
            timeForm.addEventListener("input", function () {
                timeForm.dataset.dirty = "true";
                updateTimeModeFields();
            });
            timeForm.addEventListener("change", function () {
                timeForm.dataset.dirty = "true";
                updateTimeModeFields();
            });
        }
        modeInputs.forEach(function (input) { input.addEventListener("change", updateTimeModeFields); });
        updateTimeModeFields();
        // 绑定立即执行的 NTP 同步操作。
        if (syncForm) {
            const syncButton = syncForm.querySelector("[data-time-sync-button]");
            syncForm.addEventListener("submit", function (event) {
                if (syncForm.dataset.busy === "true") {
                    event.preventDefault();
                    return;
                }
                event.preventDefault();
                setTimeOperationBusy(syncForm, [syncButton], "正在同步...");
                submitTimeFormAfterFeedback(syncForm);
            });
        }
        if (!manualForm) return;
        // 浏览器时间和手动输入共用提交表单，通过来源字段区分。
        const epochInput = manualForm.querySelector("[data-manual-epoch]");
        const sourceInput = manualForm.querySelector("[data-manual-time-source]");
        const dateInput = manualForm.querySelector("[data-manual-datetime]");
        const browserButton = manualForm.querySelector("[data-browser-time-sync]");
        const manualButton = manualForm.querySelector("[data-manual-time-submit]");
        if (browserButton) {
            browserButton.addEventListener("click", function () {
                if (manualForm.dataset.busy === "true") return;
                epochInput.value = String(Date.now());
                if (sourceInput) sourceInput.value = "browser";
                if (typeof manualForm.requestSubmit === "function") {
                    manualForm.requestSubmit();
                } else {
                    setTimeOperationBusy(manualForm, [browserButton, manualButton], "正在同步...", browserButton);
                    submitTimeFormAfterFeedback(manualForm);
                }
            });
        }
        if (manualButton) {
            manualButton.addEventListener("click", function () {
                if (sourceInput) sourceInput.value = "manual";
                epochInput.value = "";
            });
        }
        // 手动输入需结合所选时区转换为时间戳后再提交。
        manualForm.addEventListener("submit", function (event) {
            if (manualForm.dataset.busy === "true") {
                event.preventDefault();
                return;
            }
            const browserSync = sourceInput && sourceInput.value === "browser" && epochInput.value;
            if (!browserSync) {
                const timezoneInput = document.querySelector('[data-time-settings-form] [name="timezone"]');
                const timezone = timezoneInput ? timezoneInput.value.trim() : manualForm.dataset.timezone;
                const epoch = zonedDateTimeToEpoch(dateInput ? dateInput.value : "", timezone);
                if (!Number.isFinite(epoch)) {
                    event.preventDefault();
                    showToast("error", "请选择有效的手动时间和时区", { position: "top-left" });
                    return;
                }
                epochInput.value = String(Math.round(epoch));
            }
            setTimeOperationBusy(
                manualForm,
                [browserButton, manualButton],
                browserSync ? "正在同步..." : "正在校准时间...",
                browserSync ? browserButton : manualButton
            );
            event.preventDefault();
            submitTimeFormAfterFeedback(manualForm);
        });
    }

    // 显示反馈后提交时间设置表单。
    function submitTimeFormAfterFeedback(form) {
        window.setTimeout(function () {
            HTMLFormElement.prototype.submit.call(form);
        }, 0);
    }

    // 切换时间操作按钮的忙碌状态。
    function setTimeOperationBusy(form, buttons, busyText, activeButton) {
        form.dataset.busy = "true";
        form.setAttribute("aria-busy", "true");
        buttons.forEach(function (button) {
            if (!button) return;
            if (!button.dataset.idleLabel) button.dataset.idleLabel = button.textContent;
            button.disabled = true;
        });
        const target = activeButton || buttons.find(Boolean);
        if (target) target.textContent = busyText;
        updateTimeModeFields();
    }

    // 根据时间模式切换相关输入字段。
    function updateTimeModeFields() {
        const modeControl = document.querySelector("[data-time-mode-control]");
        const selected = document.querySelector("[data-time-mode]:checked");
        if (!modeControl || !selected) return;
        const ntpMode = selected.dataset.timeMode === "ntp";
        const pending = String(modeControl.dataset.settingsPending || "").toLowerCase() === "true";
        const available = String(modeControl.dataset.settingsAvailable || "").toLowerCase() === "true" &&
            String(modeControl.dataset.runtimeAvailable || "").toLowerCase() === "true";
        const timeForm = document.querySelector("[data-time-settings-form]");
        const dirty = Boolean(timeForm && timeForm.dataset.dirty === "true");
        const operationBusy = ["[data-time-sync-form]", "[data-manual-time-form]"].some(function (selector) {
            const form = document.querySelector(selector);
            return Boolean(form && form.dataset.busy === "true");
        });
        const blocked = !available || pending || dirty || operationBusy;
        const ntpFields = document.querySelector("[data-time-ntp-fields]");
        const ntpSection = document.querySelector("[data-time-ntp-section]");
        const manualSection = document.querySelector("[data-time-manual-section]");
        if (ntpFields) ntpFields.classList.toggle("hidden", !ntpMode);
        if (ntpSection) ntpSection.classList.toggle("hidden", !ntpMode);
        if (manualSection) {
            manualSection.classList.toggle("hidden", ntpMode);
            manualSection.querySelectorAll("input[type='datetime-local'],button").forEach(function (control) {
                control.disabled = blocked;
            });
        }
        const syncButton = document.querySelector("[data-time-sync-button]");
        if (syncButton) syncButton.disabled = !ntpMode || blocked;
        const actionHint = document.querySelector("[data-time-action-hint]");
        if (actionHint) {
            actionHint.textContent = !available
                ? "时间运行状态暂不可用，请恢复后端连接后再操作。"
                : (pending
                    ? "时间设置待应用，请先保存并应用当前设置。"
                    : (dirty
                        ? "设置已修改，请先保存并应用，再执行校时操作。"
                        : "时间操作正在执行，请稍候。"));
            actionHint.classList.toggle("hidden", !blocked);
        }
    }

    // 将指定时区的本地时间转换为毫秒时间戳。
    function zonedDateTimeToEpoch(value, timezone) {
        const match = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?$/.exec(String(value || ""));
        if (!match || !timezone) return NaN;
        const parts = match.slice(1).map(Number);
        const wallAsUTC = Date.UTC(parts[0], parts[1] - 1, parts[2], parts[3], parts[4], parts[5] || 0);
        let guess = wallAsUTC;
        try {
            const formatter = new Intl.DateTimeFormat("en-CA", {
                timeZone: timezone, year: "numeric", month: "2-digit", day: "2-digit",
                hour: "2-digit", minute: "2-digit", second: "2-digit", hourCycle: "h23"
            });
            for (let i = 0; i < 2; i += 1) {
                const formatted = {};
                formatter.formatToParts(new Date(guess)).forEach(function (part) { if (part.type !== "literal") formatted[part.type] = Number(part.value); });
                const represented = Date.UTC(formatted.year, formatted.month - 1, formatted.day, formatted.hour, formatted.minute, formatted.second);
                guess += wallAsUTC - represented;
            }
            return guess;
        } catch (_error) {
            return NaN;
        }
    }

    // 更新设置页的网络运行状态。
    function updateNetworkRuntimeStatus(runtime, available, errorMessage) {
        const state = document.querySelector("[data-settings-network-state]");
        const summary = document.querySelector("[data-settings-network-summary]");
        if (!available) {
            setStatusText(state, "状态不可用", "status-bad");
            if (summary) {
                summary.textContent = errorMessage ? friendlyApiMessage(errorMessage, "网口运行状态暂时无法获取") : "网口运行状态暂时无法获取";
            }
            return;
        }
        const linkText = runtime.link_state_text || runtime.link_state || "状态未知";
        setStatusText(state, linkText, networkStatusClass(runtime.link_state));
        setText("[data-network-runtime-ip]", defaultDisplayText(runtime.ip_address));
        setText("[data-network-runtime-gateway]", defaultDisplayText(runtime.default_gateway));
        if (summary) {
            summary.textContent = networkSummaryText(runtime);
        }
    }

    // 更新MQTT运行态状态。
    function updateMqttRuntimeStatus(runtime, available, errorMessage) {
        const stateText = available ? mqttStateText(runtime.state) : "状态不可用";
        const stateClass = available ? mqttStateClass(runtime.state) : "status-bad";
        setStatusText(document.querySelector("[data-settings-mqtt-state]"), stateText, stateClass);
        setStatusText(document.querySelector("[data-mqtt-runtime-state]"), stateText, stateClass);

        const summary = document.querySelector("[data-settings-mqtt-summary]");
        if (summary) {
            if (!available) {
                summary.textContent = errorMessage ? friendlyApiMessage(errorMessage, "MQTT 状态暂时无法获取") : "MQTT 状态暂时无法获取";
            } else if (runtime.last_error_message) {
                summary.textContent = runtime.last_error_message;
            } else if (runtime.enabled) {
                summary.textContent = "已启用 MQTT 北向发布";
            } else {
                summary.textContent = "未启用 MQTT 北向发布";
            }
        }

        setText("[data-mqtt-runtime-last-publish-result]", available ? mqttPublishResultText(runtime) : "状态暂不可用");
    }

    // TLS 提示只解释证书路径组合，不读取或上传任何证书与私钥内容。
    function initMqttTlsHint() {
        const checkbox = document.querySelector('.mqtt-settings-form input[name="tls_enabled"]');
        if (!checkbox) {
            return;
        }
        checkbox.addEventListener("change", function () {
            if (!checkbox.checked) {
                return;
            }
            showToast(
                "info",
                "启用 TLS 时 CA 证书路径必填；客户端证书与私钥仅在双向认证时填写。证书文件需预先放到板端。",
                { position: "top-left" }
            );
        });
    }

    // 标记设置运行态不可用状态。
    function markSettingsRuntimeUnavailable() {
        setStatusText(document.querySelector("[data-settings-network-state]"), "状态不可用", "status-bad");
        setStatusText(document.querySelector("[data-settings-mqtt-state]"), "状态不可用", "status-bad");
        setStatusText(document.querySelector("[data-settings-time-state]"), "状态不可用", "status-bad");
        setStatusText(document.querySelector("[data-mqtt-runtime-state]"), "状态不可用", "status-bad");
        setText("[data-mqtt-runtime-last-publish-result]", "状态暂不可用");
        const modeControl = document.querySelector("[data-time-mode-control]");
        if (modeControl) modeControl.dataset.runtimeAvailable = "false";
        updateTimeModeFields();
    }

    // 设置状态文本。
    function setStatusText(element, text, className) {
        if (!element) {
            return;
        }
        if (window.EdgeMotion) {
            const baseClasses = Array.from(element.classList).filter(function (name) {
                return !/^status-(?:ok|warn|bad|neutral)$/.test(name) &&
                    !/^motion-status-/.test(name);
            });
            baseClasses.push(className || "status-neutral");
            window.EdgeMotion.updateStatus(element, text, baseClasses.join(" "));
            return;
        }
        element.textContent = text;
        element.classList.remove("status-ok", "status-warn", "status-bad", "status-neutral");
        element.classList.add(className || "status-neutral");
    }

    function setText(selector, value) {
        const element = document.querySelector(selector);
        if (element) {
            element.textContent = value;
        }
    }

    // 将空值转换为统一的占位文本。
    function defaultDisplayText(value) {
        const text = String(value || "").trim();
        return text || "-";
    }

    // 用户界面只对常用时区补充易读名称，其他 IANA 标识保持原样。
    function displayTimezone(value) {
        const timezone = String(value || "").trim();
        if (timezone === "Asia/Shanghai") return "北京时间（Asia/Shanghai）";
        if (timezone === "Etc/UTC") return "UTC（Etc/UTC）";
        return timezone;
    }

    function networkStatusClass(state) {
        switch (state) {
        case "connected":
            return "status-ok";
        case "disconnected":
            return "status-warn";
        case "not_found":
            return "status-bad";
        default:
            return "status-neutral";
        }
    }

    // 根据网络运行状态生成概览说明。
    function networkSummaryText(runtime) {
        if (runtime.ip_address) {
            return "当前 IP：" + runtime.ip_address;
        }
        if (runtime.message) {
            return runtime.message;
        }
        if (runtime.link_state_text) {
            return runtime.link_state_text;
        }
        return "暂未获取到实际 IP";
    }

    // 返回 MQTT 连接状态的中文说明。
    function mqttStateText(state) {
        switch (state) {
        case "disabled":
            return "未启用";
        case "disconnected":
            return "未连接";
        case "connecting":
            return "连接中";
        case "connected":
            return "已连接";
        case "error":
            return "异常";
        default:
            return "未知";
        }
    }

    function mqttStateClass(state) {
        switch (state) {
        case "connected":
            return "status-ok";
        case "connecting":
            return "status-warn";
        case "error":
            return "status-bad";
        case "disabled":
        case "disconnected":
        default:
            return "status-neutral";
        }
    }

    // 返回最近一次 MQTT 发布结果的中文说明。
    function mqttPublishResultText(runtime) {
        if (!runtime) {
            return "状态暂不可用";
        }
        if (!runtime.enabled) {
            return "未启用";
        }
        if (runtime.last_publish_error_message) {
            return "发布失败：" + runtime.last_publish_error_message;
        }
        if (!runtime.last_publish_time_ms) {
            return "暂无发布记录";
        }
        let text = "发布成功，" + String(runtime.last_publish_payload_bytes || 0) + " 字节";
        if (runtime.last_publish_sequence) {
            text += "，序号 " + String(runtime.last_publish_sequence);
        }
        return text;
    }

    // 更新网络模式字段。
    function updateNetworkModeFields(form) {
        if (!form) {
            return;
        }
        const modeSelect = form.querySelector("[data-network-mode]");
        const dhcpMode = modeSelect && modeSelect.value === "dhcp";
        form.querySelectorAll("[data-network-static-field]").forEach(function (field) {
            field.classList.toggle("hidden", dhcpMode);
        });
        ["ip_address", "netmask", "gateway"].forEach(function (name) {
            const input = form.querySelector('[name="' + name + '"]');
            if (input) {
                input.required = !dhcpMode;
            }
        });
        const staticHint = form.querySelector("[data-network-static-hint]");
        const dhcpHint = form.querySelector("[data-network-dhcp-hint]");
        if (staticHint) {
            staticHint.classList.toggle("hidden", dhcpMode);
        }
        if (dhcpHint) {
            dhcpHint.classList.toggle("hidden", !dhcpMode);
        }
    }

    // 网络切换可能立即断开当前页面，因此成功提示同时给出新的候选访问地址。
    function initNetworkSaveApplyActions() {
        document.querySelectorAll("[data-network-save-apply-form]").forEach(function (form) {
            // 初始化模式联动和按钮原始状态。
            const feedback = form.querySelector("[data-network-save-apply-feedback]");
            const submitButton = form.querySelector("[data-network-save-apply-submit]");
            const submitLabel = submitButton ? submitButton.textContent : "保存网络设置并应用";
            const modeSelect = form.querySelector("[data-network-mode]");

            if (modeSelect) {
                modeSelect.addEventListener("change", function () {
                    updateNetworkModeFields(form);
                });
            }
            updateNetworkModeFields(form);

            form.addEventListener("submit", async function (event) {
                event.preventDefault();
                setFeedback(feedback, "", "");

                // 收集目标配置并生成包含断连风险的确认内容。
                const target = {
                    mode: readFieldValue(form, "mode") || "static",
                    interfaceName: readFieldValue(form, "interface_name"),
                    ipAddress: readFieldValue(form, "ip_address"),
                    netmask: readFieldValue(form, "netmask"),
                    gateway: readFieldValue(form, "gateway")
                };
                const confirmLines = [
                    "保存网络设置后会立即应用到系统网口，当前 Web 连接可能短暂中断。",
                    "",
                    "即将保存并应用的目标配置：",
                    "配置模式：" + (target.mode === "dhcp" ? "DHCP 自动获取" : "静态 IP"),
                    "网口名称：" + (target.interfaceName || "-")
                ];
                if (target.mode === "static") {
                    confirmLines.push(
                        "IP 地址：" + (target.ipAddress || "-"),
                        "子网掩码：" + (target.netmask || "-"),
                        "默认网关：" + (target.gateway || "-"),
                        "",
                        "请确认已记录新的 IP 地址，并在应用后使用新地址重新访问页面。"
                    );
                } else {
                    confirmLines.push("", "DHCP 获取的新地址可能与当前地址不同，请从路由器或运行状态中确认。");
                }
                const confirmMessage = confirmLines.join("\n");

                if (!await confirmAction({
                    title: "确认保存并应用网络设置",
                    message: confirmMessage,
                    confirmText: "确认应用"
                })) {
                    return;
                }

                if (submitButton) {
                    submitButton.disabled = true;
                    submitButton.textContent = "保存并应用中...";
                }
                settingsRuntimeRefreshPaused = true;
                setFeedback(feedback, "info", "正在保存网络设置并应用到系统网口。");

                // 提交配置；成功后给出静态地址或 DHCP 的重新访问指引。
                try {
                    const body = new URLSearchParams(new FormData(form));
                    const response = await csrfFetch(form.action, {
                        method: "POST",
                        headers: {
                            "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8",
                            "Accept": "application/json",
                            "X-Requested-With": "XMLHttpRequest"
                        },
                        body: body
                    });

                    const result = await response.json();
                    if (!response.ok || !result.success) {
                        const message = result && result.error ? result.error.message : "保存并应用失败";
                        throw new Error(friendlyApiMessage(message, "保存并应用失败"));
                    }

                    const responseMessage = result && result.data ? result.data.message : "";
                    const accessUrl = buildNetworkAccessUrl(target.ipAddress);
                    const message = responseMessage || (target.mode === "dhcp"
                        ? "网络配置已切换为 DHCP，请确认当前实际地址后重新访问页面。"
                        : "网络设置已保存并应用到系统网口。请使用新地址 " + accessUrl + " 重新访问页面。当前页面不会自动跳转。");
                    setFeedback(feedback, "success", message, { toastOptions: { autohide: false } });
                } catch (error) {
                    if (error && error.edgeStale) return;
                    const rawMessage = error && error.message ? error.message : "";
                    const disconnected = rawMessage.toLowerCase().includes("failed to fetch") ||
                        rawMessage.toLowerCase().includes("network") ||
                        rawMessage.toLowerCase().includes("load failed");
                    const accessUrl = buildNetworkAccessUrl(target.ipAddress);
                    const message = disconnected
                        ? (target.mode === "dhcp"
                            ? "当前 Web 连接可能已中断。DHCP 地址可能已经变化，请从路由器或设备运行状态确认新地址。"
                            : "当前 Web 连接可能已中断。请尝试使用新地址 " + accessUrl + " 重新访问页面；如配置未生效，请回到原地址继续检查。")
                        : friendlyApiMessage(rawMessage, "保存并应用失败");
                    setFeedback(feedback, disconnected ? "info" : "error", message, {
                        toastOptions: { autohide: disconnected ? false : true }
                    });
                } finally {
                    // 恢复操作按钮，并延迟恢复运行状态刷新。
                    if (submitButton) {
                        submitButton.disabled = false;
                        submitButton.textContent = submitLabel;
                    }
                    const scope = pageScope;
                    if (scope && scope.isActive()) {
                        scope.setTimeout(function () {
                            settingsRuntimeRefreshPaused = false;
                        }, 1000);
                    }
                }
            });
        });
    }

    // 根据目标 IP 构造网络切换后的候选访问地址。
    function buildNetworkAccessUrl(ipAddress) {
        const normalizedIp = String(ipAddress || "").trim();
        const host = normalizedIp || window.location.hostname;
        const displayHost = host.includes(":") && !host.startsWith("[") ? "[" + host + "]" : host;
        const port = window.location.port ? ":" + window.location.port : "";
        return window.location.protocol + "//" + displayHost + port;
    }

    if (window.EdgeOverviewTestHooksEnabled === true) {
        window.EdgeOverviewTestHooks = {
            updateNetworkModeFields: updateNetworkModeFields,
            zonedDateTimeToEpoch: zonedDateTimeToEpoch,
            updateTimeRuntimeStatus: updateTimeRuntimeStatus,
            updateTimeModeFields: updateTimeModeFields
        };
    }

    EdgeApp.registerPageController("settings", ["settings"], { mount: mount });
})();
