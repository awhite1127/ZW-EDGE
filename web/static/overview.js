// 系统概览页轻量刷新器：更新进程、存储、轮询、MQTT 和综合健康状态。
// 单项指标不可用时仅降级对应卡片，不清空其他仍然有效的运行信息。
(function () {
    "use strict";

    const EdgeApp = window.EdgeApp;
    if (!EdgeApp) return;

    const friendlyApiMessage = EdgeApp.friendlyApiMessage;
    const formatTimestamp = EdgeApp.formatTimestamp;
    const defaultDisplayText = EdgeApp.defaultDisplayText;
    const isPageHidden = EdgeApp.isPageHidden || function () { return false; };
    const onPageVisibilityChange = EdgeApp.onPageVisibilityChange || function () { return false; };
    const EdgeMotion = window.EdgeMotion;
    let pageScope = null;

    function mount(scope) {
        pageScope = scope;
        initOverviewDiagnosisRefresh();
    }


    // ---------- 系统概览局部刷新；各子状态独立降级 ----------
    // 首屏优先使用服务端注入快照，随后由单一定时任务增量刷新。
    function initOverviewDiagnosisRefresh() {
        const scope = pageScope;
        const root = document.querySelector("[data-overview-diagnosis]");
        if (!root) {
            return;
        }
        const refreshUrl = root.getAttribute("data-refresh-url");
        if (!refreshUrl) {
            return;
        }

        const elements = collectOverviewSnapshotElements();
        // 首屏快照已经由服务端写入 DOM；这里只把它作为“已有数据”标记，
        // 不再解析后把相同内容完整写入第二次。
        const initialSnapshot = root.getAttribute("data-initial-snapshot");
        const initialSnapshotRendered = Boolean(initialSnapshot);

        let inFlight = false;
        let refreshTimer = null;
        const refresh = function () {
            if (isPageHidden() || inFlight) {
                return;
            }
            inFlight = true;
            scope.fetch(refreshUrl, {
                headers: {
                    "Accept": "application/json"
                },
                credentials: "same-origin"
            })
                .then(function (response) {
                    return response.json().then(function (payload) {
                        if (!response.ok) {
                            throw new Error("overview diagnosis refresh failed: " + response.status);
                        }
                        return payload;
                    });
                })
                .then(function (payload) {
                    if (!scope.isActive()) throw new Error("stale page task");
                    if (!payload || payload.success === false || !payload.data) {
                        throw new Error("overview diagnosis response invalid");
                    }
                    updateOverviewDiagnosis(elements, payload.data, true);
                })
                .catch(function (error) {
                    if (!scope.isActive() || EdgeApp.isStalePageError(error)) return;
                    updateOverviewDiagnosis(elements, null, false);
                })
                // 不依赖 Promise.prototype.finally，兼容仍支持 async/fetch 的较旧 Chromium。
                .then(function () {
                    inFlight = false;
                });
        };

        const stopTimer = function () {
            if (refreshTimer !== null) {
                window.clearInterval(refreshTimer);
                refreshTimer = null;
            }
        };
        const startTimer = function () {
            stopTimer();
            if (!isPageHidden()) {
                refreshTimer = window.setInterval(refresh, 10000);
            }
        };

        if (!initialSnapshotRendered) refresh();
        startTimer();
        scope.onVisibilityChange(function () {
            if (isPageHidden()) {
                stopTimer();
            } else {
                refresh();
                startTimer();
            }
        });
        scope.listen(window, "pagehide", stopTimer);
        scope.listen(window, "pageshow", function (event) {
            if (event.persisted) {
                refresh();
                startTimer();
            }
        });
        scope.onDispose(stopTimer);
    }

    // 集中缓存快照更新所需节点，避免每轮刷新重复查询 DOM。
    function collectOverviewSnapshotElements() {
        return {
            badge: document.getElementById("overview-diagnosis-badge"),
            stateCard: document.getElementById("overview-diagnosis-state-card"),
            state: document.getElementById("overview-diagnosis-state"),
            note: document.getElementById("overview-diagnosis-note"),
            pollingState: document.getElementById("overview-polling-state"),
            pollingCycle: document.getElementById("overview-polling-cycle"),
            pollingQuality: document.getElementById("overview-polling-quality"),
            currentError: document.getElementById("overview-current-error"),
            mqttSection: document.querySelector("[data-overview-mqtt-summary]"),
            mqttState: document.getElementById("overview-mqtt-state"),
            mqttBroker: document.getElementById("overview-mqtt-broker"),
            mqttLastError: document.getElementById("overview-mqtt-last-error"),
            modbusSection: document.querySelector("[data-overview-modbus-summary]"),
            modbusState: document.getElementById("overview-modbus-state"),
            modbusPort: document.getElementById("overview-modbus-port"),
            modbusConnections: document.getElementById("overview-modbus-connections"),
            modbusError: document.getElementById("overview-modbus-error")
        };
    }

    // 所有卡片从同一快照更新，保持健康结论与底层指标一致。
    function updateOverviewDiagnosis(elements, snapshot, backendReachable) {
        if (!backendReachable || !snapshot) {
            renderOverviewSnapshotUnavailable(elements);
            return;
        }

        const health = snapshot.health || {};
        const healthInfo = overviewHealthInfo(health.level);
        const noteText = health.message || healthInfo.message;

        if (elements.badge) {
            elements.badge.textContent = healthInfo.text;
            elements.badge.className = healthInfo.className;
        }
        if (elements.stateCard) {
            elements.stateCard.classList.toggle("is-issue", healthInfo.issue);
            elements.stateCard.classList.toggle("is-normal", !healthInfo.issue);
        }
        setText(elements.state, healthInfo.text);
        setText(elements.note, noteText);

        renderOverviewPollingMetrics(elements, snapshot.polling || {}, snapshot.current_error || {});
        renderOverviewMqttRuntime(elements, snapshot.mqtt_runtime || {});
        renderOverviewModbusRuntime(elements, snapshot.modbus_server_runtime || {});
    }

    // 渲染概览快照不可用状态。
    function renderOverviewSnapshotUnavailable(elements) {
        if (elements.badge) {
            elements.badge.textContent = "后台暂不可达";
            elements.badge.className = "status-bad";
        }
        if (elements.stateCard) {
            elements.stateCard.classList.toggle("is-issue", true);
            elements.stateCard.classList.toggle("is-normal", false);
        }
        setText(elements.state, "后台暂不可达");
        setText(elements.note, "无法读取系统运行快照，请检查后端服务状态");

        renderOverviewPollingUnavailable(elements);
        renderOverviewMqttUnavailable(elements);
        renderOverviewModbusUnavailable(elements);
    }

    // 渲染概览轮询指标。
    function renderOverviewPollingMetrics(elements, polling, currentError) {
        const running = polling && polling.polling_running === true;
        const hasCycleError = polling && polling.last_cycle_has_error === true;
        if (elements.pollingState) {
            elements.pollingState.textContent = running ? "运行中" : "未运行";
            elements.pollingState.className = running ? (hasCycleError ? "status-warn" : "status-ok") : "status-neutral";
        }

        const finishedAt = Number(polling && polling.last_cycle_finished_at_ms || 0);
        setText(elements.pollingCycle, finishedAt ? formatOverviewTimestamp(finishedAt) : "暂无完成记录");

        const successDeviceCount = Number(polling && polling.last_cycle_success_device_count || 0);
        const failedDeviceCount = Number(polling && polling.last_cycle_failed_device_count || 0);
        const successDevices = formatMetricCount(successDeviceCount);
        const failedDevices = formatMetricCount(failedDeviceCount);
        const hasCycleData = finishedAt > 0 || successDeviceCount > 0 || failedDeviceCount > 0;
        const qualityText = hasCycleData
            ? ("成功设备 " + successDevices + " / 失败设备 " + failedDevices)
            : "暂无周期数据";
        const qualityClassName = failedDeviceCount > 0 ? "status-warn" : "status-neutral";
        // 成功/失败数量是复合状态，不按首个数字推断升降方向；文本和状态类原子更新，
        // 避免后续 className 赋值清掉刚添加的状态反馈类。
        setStatus(elements.pollingQuality, qualityText, qualityClassName);

        const currentErrorMessage = String(currentError && (currentError.message || (currentError.diagnosis || {}).message) || "").trim();
        const hasCurrentError = currentError && (currentError.has_error === true || currentErrorMessage !== "");
        setText(elements.currentError, hasCurrentError ? friendlyApiMessage(currentErrorMessage, "存在当前错误") : "暂无当前错误");
        if (elements.currentError) {
            elements.currentError.className = hasCurrentError ? "status-bad" : "status-neutral";
        }
    }

    // 渲染概览轮询不可用状态。
    function renderOverviewPollingUnavailable(elements) {
        if (elements.pollingState) {
            elements.pollingState.textContent = "状态不可用";
            elements.pollingState.className = "status-bad";
        }
        setText(elements.pollingCycle, "--");
        setStatus(elements.pollingQuality, "--", "status-neutral");
        setText(elements.currentError, "运行快照读取失败");
        if (elements.currentError) elements.currentError.className = "status-bad";
    }

    // 渲染概览MQTT运行态。
    function renderOverviewMqttRuntime(elements, runtime) {
        setOverviewSectionEnabled(elements.mqttSection, runtime && runtime.enabled === true);
        const state = overviewMqttStateInfo(runtime || {});
        setText(elements.mqttState, state.text);
        if (elements.mqttState) {
            elements.mqttState.className = state.className;
        }
        setText(elements.mqttBroker, defaultDisplayText(runtime && runtime.broker_endpoint));
        setText(elements.mqttLastError, defaultDisplayText(runtime && (runtime.last_error_message || runtime.last_publish_error_message)));
    }

    // 渲染概览MQTT不可用状态。
    function renderOverviewMqttUnavailable(elements) {
        setText(elements.mqttState, "状态暂不可用");
        if (elements.mqttState) {
            elements.mqttState.className = "status-neutral";
        }
        setText(elements.mqttBroker, "--");
        setText(elements.mqttLastError, "--");
    }

    // 后端快照不可用时同步降级 Modbus 运行态，避免继续显示上一次的监听状态和连接数。
    function renderOverviewModbusUnavailable(elements) {
        setStatus(elements.modbusState, "状态暂不可用", "status-neutral");
        setText(elements.modbusPort, "--");
        setText(elements.modbusConnections, "--");
        setText(elements.modbusError, "--");
    }

    // 渲染概览页的 Modbus 运行状态。
    function renderOverviewModbusRuntime(elements, runtime) {
        setOverviewSectionEnabled(elements.modbusSection, runtime && runtime.configured_enabled === true);
        let text = "已关闭", className = "status-neutral";
        if (runtime && runtime.configured_enabled) {
            if (runtime.listening) { text = "正在监听"; className = "status-ok"; }
            else if (runtime.state === "error") { text = "运行异常"; className = "status-bad"; }
            else { text = "已启用，未监听"; className = "status-warn"; }
        }
        setText(elements.modbusState, text);
        if (elements.modbusState) elements.modbusState.className = className;
        setText(elements.modbusPort, runtime && runtime.listen_port ? runtime.listen_port : "--");
        setText(elements.modbusConnections, runtime && runtime.current_connections || 0, "value");
        setText(elements.modbusError, defaultDisplayText(runtime && runtime.last_error_message));
    }

    // 设置概览区域启用。
    function setOverviewSectionEnabled(section, enabled) {
        if (section) {
            section.hidden = !enabled;
        }
    }

    // 后端只返回稳定级别码，颜色和短文案属于前端展示映射。
    function overviewHealthInfo(level) {
        switch (String(level || "").trim().toLowerCase()) {
        case "normal":
            return { text: "运行正常", message: "系统运行状态正常", className: "status-ok", issue: false };
        case "warning":
            return { text: "需要关注", message: "系统存在需要关注的运行状态", className: "status-warn", issue: true };
        case "error":
            return { text: "运行异常", message: "系统存在异常，请及时处理", className: "status-bad", issue: true };
        default:
            return { text: "状态未知", message: "系统健康状态暂不可用", className: "status-neutral", issue: false };
        }
    }

    // 汇总 MQTT 连接状态及最近发布结果。
    function overviewMqttStateInfo(runtime) {
        if (!runtime || Object.keys(runtime).length === 0) {
            return { text: "状态暂不可用", className: "status-neutral" };
        }
        if (!runtime.enabled) {
            return { text: "未启用", className: "status-neutral" };
        }
        if (runtime.connected || String(runtime.state || "").toLowerCase() === "connected") {
            return { text: "已连接", className: "status-ok" };
        }
        const errorText = String(runtime.last_error_message || runtime.last_publish_error_message || "").trim();
        if (errorText) {
            return { text: friendlyApiMessage(errorText, "未连接"), className: "status-warn" };
        }
        return { text: "未连接", className: "status-warn" };
    }

    // 将指标数量格式化为非负整数文本。
    function formatMetricCount(value) {
        const number = Number(value);
        if (!Number.isFinite(number) || number < 0) {
            return "--";
        }
        return String(Math.trunc(number));
    }

    // 格式化概览时间戳。
    function formatOverviewTimestamp(value) {
        if (!value) {
            return "--";
        }
        return formatTimestamp(value);
    }

    function setText(element, value, kind) {
        if (typeof element === "string") {
            const target = document.querySelector(element);
            if (target) {
                if (EdgeMotion && kind) EdgeMotion.updateText(target, value, kind);
                else target.textContent = value;
            }
            return;
        }
        if (element) {
            if (EdgeMotion && kind) EdgeMotion.updateText(element, value, kind);
            else element.textContent = value;
        }
    }

    // 状态文本与样式必须作为一次语义更新提交，不能在数值动效之后覆盖 motion 类。
    function setStatus(element, text, className) {
        if (!element) return;
        if (EdgeMotion) EdgeMotion.updateStatus(element, text, className);
        else {
            element.textContent = text;
            element.className = className;
        }
    }

    if (window.EdgeOverviewTestHooksEnabled === true) {
        window.EdgeOverviewTestHooks = {
            collectOverviewSnapshotElements: collectOverviewSnapshotElements,
            updateOverviewDiagnosis: updateOverviewDiagnosis,
            renderOverviewSnapshotUnavailable: renderOverviewSnapshotUnavailable,
            renderOverviewPollingMetrics: renderOverviewPollingMetrics,
            formatOverviewTimestamp: formatOverviewTimestamp
        };
    }

    EdgeApp.registerPageController("overview", ["overview"], { mount: mount });
})();
