// 事件与告警中心控制器：负责历史事件清理、活动告警刷新、确认以及设备点位规则编辑。
// 告警状态和规则始终以后端为权威来源，前端筛选只影响当前页面展示。
(function () {
    "use strict";

    const EdgeApp = window.EdgeApp;
    if (!EdgeApp) return;

    const showToast = EdgeApp.showToast;
    const confirmAction = EdgeApp.confirmAction;
    const isPageHidden = EdgeApp.isPageHidden || function () { return false; };
    const onPageVisibilityChange = EdgeApp.onPageVisibilityChange || function () { return false; };
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
        initEventActions();
        initAlarmCenterPage();
    }

    // 清空历史事件需要显式确认，且不改变当前活动告警运行态。
    function initEventActions() {
        document.querySelectorAll("[data-clear-events-form]").forEach(function (form) {
            form.addEventListener("submit", async function (event) {
                event.preventDefault();
                const confirmed = await confirmAction({
                    title: "确认清除历史事件",
                    message: "清除后相关历史事件将无法恢复；当前告警、告警规则、采集配置和实时数据不受影响。确定继续吗？",
                    confirmText: "确认清除",
                    danger: true
                });
                if (confirmed) form.submit();
            });
        });
    }


    // 告警中心把活动告警与规则表单放在同一控制器中，共享设备和点位选择上下文。
    function initAlarmCenterPage() {
        const scope = pageScope;
        const root = document.querySelector("[data-alarm-center]");
        if (!root) {
            return;
        }
        const modal = root.querySelector("[data-alarm-modal]");
        const form = root.querySelector("[data-alarm-rule-form]");
        const masterSelect = root.querySelector("[data-alarm-master-select]");
        const pointSelect = root.querySelector("[data-alarm-point-select]");
        const formError = root.querySelector("[data-alarm-form-error]");
        const modalTitle = root.querySelector("[data-alarm-modal-title]");
        const liveStats = root.querySelector("[data-alarm-live-stats]");
        const liveCurrent = root.querySelector("[data-alarm-live-current]");
        let alarmRefreshPromise = null;
        let alarmRefreshTimer = null;
        let modalReturnFocus = null;

        function alarmItemKey(item) {
            return String(item && item.dataset.alarmKey || "");
        }

        // 稳定告警集合只更新对应值和确认区；新增或恢复时才替换列表结构。
        function reconcileCurrentAlarms(nextCurrent) {
            const currentItems = Array.from(liveCurrent.querySelectorAll("[data-alarm-key]"));
            const nextItems = Array.from(nextCurrent.querySelectorAll("[data-alarm-key]"));
            const currentEmpty = liveCurrent.querySelector(".alarm-compact-empty");
            const nextEmpty = nextCurrent.querySelector(".alarm-compact-empty");
            const sameEmptyState = (!currentEmpty && !nextEmpty) ||
                (currentEmpty && nextEmpty && currentEmpty.outerHTML === nextEmpty.outerHTML);
            const sameKeys = currentItems.length === nextItems.length &&
                sameEmptyState &&
                currentItems.every(function (item, index) {
                    return alarmItemKey(item) === alarmItemKey(nextItems[index]);
                });

            liveCurrent.className = nextCurrent.className;
            if (!sameKeys) {
                const previousKeys = {};
                currentItems.forEach(function (item) { previousKeys[alarmItemKey(item)] = true; });
                if (window.EdgeMotion) window.EdgeMotion.cancelWithin(liveCurrent);
                liveCurrent.innerHTML = nextCurrent.innerHTML;
                if (window.EdgeMotion) {
                    liveCurrent.querySelectorAll("[data-alarm-key]").forEach(function (item) {
                        if (!previousKeys[alarmItemKey(item)]) window.EdgeMotion.reveal(item);
                    });
                    const empty = liveCurrent.querySelector(".alarm-compact-empty");
                    if (empty) window.EdgeMotion.reveal(empty);
                }
                return true;
            }

            currentItems.forEach(function (item, index) {
                const nextItem = nextItems[index];
                [".alarm-compact-device", ".alarm-compact-point"].forEach(function (selector) {
                    const currentLabel = item.querySelector(selector);
                    const nextLabel = nextItem.querySelector(selector);
                    if (!currentLabel || !nextLabel) return;
                    if (currentLabel.textContent !== nextLabel.textContent) {
                        currentLabel.textContent = nextLabel.textContent;
                    }
                    const nextTitle = nextLabel.getAttribute("title") || "";
                    if ((currentLabel.getAttribute("title") || "") !== nextTitle) {
                        currentLabel.setAttribute("title", nextTitle);
                    }
                });
                const currentValue = item.querySelector("[data-alarm-current-value]");
                const nextValue = nextItem.querySelector("[data-alarm-current-value]");
                if (currentValue && nextValue && currentValue.textContent !== nextValue.textContent) {
                    const previous = currentValue.textContent;
                    currentValue.textContent = nextValue.textContent;
                    if (window.EdgeMotion) {
                        window.EdgeMotion.markStatus(currentValue, previous, nextValue.textContent, {
                            critical: true
                        });
                    }
                }
                [".event-level-badge", ".alarm-compact-threshold", ".alarm-compact-side"].forEach(function (selector) {
                    const current = item.querySelector(selector);
                    const next = nextItem.querySelector(selector);
                    if (!current || !next || current.outerHTML === next.outerHTML) return;
                    if (window.EdgeMotion) window.EdgeMotion.cancelWithin(current);
                    current.replaceWith(document.importNode(next, true));
                    if (selector === ".alarm-compact-side" && window.EdgeMotion) {
                        const updated = item.querySelector(selector);
                        if (updated) window.EdgeMotion.reveal(updated);
                    }
                });
            });

            const currentRemaining = liveCurrent.querySelector(".alarm-remaining-count");
            const nextRemaining = nextCurrent.querySelector(".alarm-remaining-count");
            if (currentRemaining && nextRemaining) {
                if (currentRemaining.textContent !== nextRemaining.textContent) {
                    currentRemaining.textContent = nextRemaining.textContent;
                }
            } else if (currentRemaining || nextRemaining) {
                if (currentRemaining) currentRemaining.remove();
                if (nextRemaining) liveCurrent.appendChild(document.importNode(nextRemaining, true));
            }
            return false;
        }

        // 更新报警列表的自动刷新状态提示。
        function setAlarmRefreshStatus(message, failed) {
            const status = root.querySelector("[data-alarm-auto-refresh-status]");
            if (!status) return;
            status.textContent = message;
            status.classList.toggle("is-error", !!failed);
            status.classList.toggle("hidden", !failed);
        }

        // 自动刷新只替换发生变化的活动告警区域；规则统计和规则 DOM 保持首屏快照。
        function refreshAlarmManagement(forceAfterCurrent) {
            if (!liveStats || !liveCurrent || isPageHidden()) return Promise.resolve();
            if (alarmRefreshPromise) {
                return forceAfterCurrent
                    ? alarmRefreshPromise.then(function () { return refreshAlarmManagement(false); })
                    : alarmRefreshPromise;
            }
            alarmRefreshPromise = (async function () {
                try {
                    const response = await scope.fetch("/events?tab=alarms", {
                        method: "GET",
                        cache: "no-store",
                        headers: { "Accept": "text/html", "X-Requested-With": "XMLHttpRequest" }
                    });
                    const responseHTML = await response.text();
                    if (!response.ok) throw new Error("当前告警刷新失败");
                    if (!scope.isActive()) return;
                    const documentSnapshot = new DOMParser().parseFromString(responseHTML, "text/html");
                    const nextStats = documentSnapshot.querySelector("[data-alarm-live-stats]");
                    const nextCurrent = documentSnapshot.querySelector("[data-alarm-live-current]");
                    if (!nextStats || !nextCurrent) throw new Error("当前告警刷新数据不完整");

                    ["active", "unacknowledged"].forEach(function (name) {
                        const current = liveStats.querySelector('[data-alarm-live-count="' + name + '"]');
                        const next = nextStats.querySelector('[data-alarm-live-count="' + name + '"]');
                        if (current && next && current.textContent !== next.textContent) {
                            if (window.EdgeMotion) window.EdgeMotion.updateText(current, next.textContent, "value-neutral");
                            else current.textContent = next.textContent;
                        }
                    });
                    reconcileCurrentAlarms(nextCurrent);
                    setAlarmRefreshStatus("每 10 秒自动刷新", false);
                } catch (error) {
                    if (!scope.isActive() || EdgeApp.isStalePageError(error)) return;
                    setAlarmRefreshStatus("自动刷新暂不可用", true);
                } finally {
                    alarmRefreshPromise = null;
                }
            })();
            return alarmRefreshPromise;
        }

        if (liveStats && liveCurrent) {
            const stopAlarmRefreshTimer = function () {
                if (alarmRefreshTimer !== null) {
                    window.clearInterval(alarmRefreshTimer);
                    alarmRefreshTimer = null;
                }
            };
            const startAlarmRefreshTimer = function () {
                stopAlarmRefreshTimer();
                if (!isPageHidden()) {
                    alarmRefreshTimer = window.setInterval(refreshAlarmManagement, 10000);
                }
            };
            startAlarmRefreshTimer();
            scope.onVisibilityChange(function () {
                if (isPageHidden()) {
                    stopAlarmRefreshTimer();
                } else {
                    refreshAlarmManagement(false);
                    startAlarmRefreshTimer();
                }
            });
            scope.listen(window, "pagehide", stopAlarmRefreshTimer);
            scope.listen(window, "pageshow", function (event) {
                if (event.persisted) {
                    refreshAlarmManagement(false);
                    startAlarmRefreshTimer();
                }
            });
            scope.onDispose(stopAlarmRefreshTimer);
        }

        root.addEventListener("click", async function (event) {
            const button = event.target.closest("[data-alarm-acknowledge]");
            if (!button || !root.contains(button)) return;
            button.disabled = true;
            const originalText = button.textContent;
            button.textContent = "确认中...";
            const body = new URLSearchParams();
            body.set("device_id", button.dataset.deviceId || "");
            body.set("point_key", button.dataset.pointKey || "");
            body.set("active_since_ms", button.dataset.activeSinceMs || "");
            try {
                const response = await csrfFetch("/actions/alarms/acknowledge", {
                    method: "POST",
                    body: body,
                    headers: {
                        "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8",
                        "Accept": "application/json",
                        "X-Requested-With": "XMLHttpRequest"
                    }
                });
                const payload = await response.json().catch(function () { return null; });
                if (!response.ok || !payload || !payload.success) {
                    const message = payload && payload.error && payload.error.message ? payload.error.message : "告警确认失败";
                    throw new Error(message);
                }
                showToast("success", payload.data && payload.data.message ? payload.data.message : "告警已确认");
                await refreshAlarmManagement(true);
                if (button.isConnected) {
                    button.disabled = true;
                    button.textContent = "已确认";
                }
            } catch (error) {
                if (error && error.edgeStale) return;
                showToast("error", error && error.message ? error.message : "告警确认失败");
                button.disabled = false;
                button.textContent = originalText;
            }
        });

        // 返回告警表单当前选中的阈值类型。
        function selectedLimitType() {
            const selected = form && form.querySelector("input[name='limit_type']:checked");
            return selected ? selected.value : "off";
        }

        // 应用阈值类型。
        function applyLimitType() {
            if (!form) return;
            const type = selectedLimitType();
            const highEnabled = type === "high" || type === "both";
            const lowEnabled = type === "low" || type === "both";
            const enabled = type !== "off";
            const enabledInput = form.querySelector("[data-alarm-enabled]");
            const highInput = form.querySelector("[data-alarm-high-enabled]");
            const lowInput = form.querySelector("[data-alarm-low-enabled]");
            if (enabledInput) enabledInput.value = enabled ? "on" : "";
            if (highInput) highInput.value = highEnabled ? "on" : "";
            if (lowInput) lowInput.value = lowEnabled ? "on" : "";
            const highThreshold = form.querySelector("[data-alarm-high-threshold]");
            const lowThreshold = form.querySelector("[data-alarm-low-threshold]");
            if (highThreshold) highThreshold.disabled = !highEnabled;
            if (lowThreshold) lowThreshold.disabled = !lowEnabled;
            form.querySelectorAll("[data-alarm-limit-type] label").forEach(function (label) {
                const input = label.querySelector("input[name='limit_type']");
                label.classList.toggle("is-selected", Boolean(input && input.checked));
            });
        }

        // 设置告警表单错误。
        function setAlarmFormError(message) {
            if (formError) {
                formError.textContent = message || "";
            }
        }

        // 更新点位选项。
        function updatePointOptions(selectedPoint) {
            if (!masterSelect || !pointSelect) {
                return;
            }
            const masterID = masterSelect.value;
            let firstVisible = null;
            let matchedVisible = null;
            Array.from(pointSelect.options).forEach(function (option) {
                const visible = option.dataset.masterId === masterID;
                option.hidden = !visible;
                option.disabled = !visible;
                option.selected = false;
                if (visible && !firstVisible) {
                    firstVisible = option;
                }
                if (visible && selectedPoint && option.value === selectedPoint && !matchedVisible) {
                    matchedVisible = option;
                }
            });
            const nextSelected = matchedVisible || firstVisible;
            if (nextSelected) {
                nextSelected.selected = true;
            }
        }

        // 筛选并更新当前选择状态。
        function selectMasterOption(selectedMasterID) {
            if (!masterSelect) {
                return false;
            }
            let matched = null;
            Array.from(masterSelect.options).forEach(function (option) {
                option.selected = false;
                if (selectedMasterID && option.value === selectedMasterID && !matched) {
                    matched = option;
                }
            });
            if (!matched) {
                masterSelect.selectedIndex = -1;
                return false;
            }
            matched.selected = true;
            return true;
        }

        // 编辑时从触发元素恢复规则快照；新建时则清空旧表单，防止跨设备复用点位。
        function openAlarmModal(source) {
            if (!modal || !form || !masterSelect || !pointSelect || !source) {
                return;
            }
            form.reset();
            if (modalTitle) {
                modalTitle.textContent = "编辑告警规则";
            }
            if (!selectMasterOption(source.dataset.masterId || "")) {
                showToast("error", "当前配置项缺少有效主站 ID，请刷新页面后重试。");
                return;
            }
            updatePointOptions(source.dataset.pointKey || "");
            const enabled = source.dataset.enabled === "true";
            const highEnabled = source.dataset.highEnabled === "true";
            const lowEnabled = source.dataset.lowEnabled === "true";
            const type = !enabled ? "off" : (highEnabled && lowEnabled ? "both" : (highEnabled ? "high" : (lowEnabled ? "low" : "off")));
            const limitType = form.querySelector("input[name='limit_type'][value='" + type + "']");
            if (limitType) limitType.checked = true;
            setInputValue(form.querySelector("[data-alarm-high-threshold]"), source.dataset.highThreshold);
            setInputValue(form.querySelector("[data-alarm-low-threshold]"), source.dataset.lowThreshold);
            setInputValue(form.querySelector("[data-alarm-level]"), source.dataset.level || "warning");
            setInputValue(form.querySelector("[data-alarm-hysteresis]"), source.dataset.hysteresis || "0");
            setInputValue(form.querySelector("[data-alarm-trigger-count]"), source.dataset.triggerCount || "1");
            setInputValue(form.querySelector("[data-alarm-recovery-count]"), source.dataset.recoveryCount || "1");
            const targetValues = {
                "[data-alarm-target-master]": source.dataset.masterName || "—",
                "[data-alarm-target-point]": source.dataset.pointName || "—",
                "[data-alarm-target-unit]": source.dataset.unit || "无单位",
                "[data-alarm-target-coverage]": source.dataset.coverage || "暂无可作用设备"
            };
            Object.keys(targetValues).forEach(function (selector) {
                const target = form.querySelector(selector);
                if (target) target.textContent = targetValues[selector];
            });
            form.querySelectorAll("[data-alarm-unit]").forEach(function (item) { item.textContent = source.dataset.unit || "无单位"; });
            applyLimitType();
            setAlarmFormError("");
            modalReturnFocus = document.activeElement instanceof HTMLElement ? document.activeElement : source;
            if (window.EdgeMotion) window.EdgeMotion.cancelExit(modal);
            modal.hidden = false;
            modal.setAttribute("aria-hidden", "false");
            document.body.classList.add("modal-open");
            const focusTarget = form.querySelector("input:not([type='hidden']):not([disabled]), select:not([disabled]), button:not([disabled])");
            if (focusTarget) scope.setTimeout(function () { focusTarget.focus(); }, 0);
        }

        // 关闭告警弹窗。
        function closeAlarmModal() {
            if (!modal) {
                return;
            }
            modal.setAttribute("aria-hidden", "true");
            if (window.EdgeMotion) {
                window.EdgeMotion.hideAfterExit(modal, function () {
                    modal.hidden = true;
                });
            } else {
                modal.hidden = true;
            }
            document.body.classList.remove("modal-open");
            setAlarmFormError("");
            if (modalReturnFocus && document.contains(modalReturnFocus)) modalReturnFocus.focus();
            modalReturnFocus = null;
        }

        if (masterSelect) {
            masterSelect.addEventListener("change", function () {
                updatePointOptions("");
            });
            if (!masterSelect.value && masterSelect.options.length > 0) {
                masterSelect.options[0].selected = true;
            }
            updatePointOptions(pointSelect ? pointSelect.value : "");
        }

        root.querySelectorAll("[data-alarm-edit]").forEach(function (button) {
            button.addEventListener("click", function () {
                openAlarmModal(button);
            });
        });

        root.querySelectorAll("[data-alarm-modal-close]").forEach(function (button) {
            button.addEventListener("click", closeAlarmModal);
        });
        if (modal) {
            modal.addEventListener("click", function (event) {
                if (event.target === modal) {
                    closeAlarmModal();
                }
            });
            scope.listen(document, "keydown", function (event) {
                if (modal.hidden) return;
                if (event.key === "Escape") {
                    event.preventDefault();
                    closeAlarmModal();
                    return;
                }
                if (event.key !== "Tab") return;
                const focusable = Array.from(modal.querySelectorAll("button:not([disabled]), input:not([disabled]):not([type='hidden']), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex='-1'])"))
                    .filter(function (node) { return node.getClientRects().length > 0; });
                if (!focusable.length) return;
                const first = focusable[0];
                const last = focusable[focusable.length - 1];
                if (event.shiftKey && document.activeElement === first) {
                    event.preventDefault();
                    last.focus();
                } else if (!event.shiftKey && document.activeElement === last) {
                    event.preventDefault();
                    first.focus();
                }
            });
        }

        if (form) {
            form.querySelectorAll("input[name='limit_type']").forEach(function (input) {
                input.addEventListener("change", applyLimitType);
            });
            form.addEventListener("submit", async function (event) {
                applyLimitType();
                const message = validateAlarmRuleForm(form);
                if (message) {
                    event.preventDefault();
                    setAlarmFormError(message);
                    return;
                }
                event.preventDefault();
                setAlarmFormError("");
                const submitButton = form.querySelector("button[type='submit']");
                if (submitButton) {
                    submitButton.disabled = true;
                    submitButton.textContent = "保存中...";
                }
                try {
                    const response = await csrfFetch(form.action, {
                        method: "POST",
                        body: new FormData(form),
                        headers: {
                            "Accept": "application/json",
                            "X-Requested-With": "XMLHttpRequest"
                        }
                    });
                    const payload = await response.json();
                    if (!response.ok || !payload.success) {
                        const errorMessage = payload && payload.error ? payload.error.message : "告警规则保存失败";
                        throw new Error(errorMessage);
                    }
                    const message = payload && payload.data && payload.data.message ? payload.data.message : "告警规则已保存";
                    const url = new URL(window.location.href);
                    url.searchParams.set("tab", "alarms");
                    url.searchParams.set("flash_type", "success");
                    url.searchParams.set("flash_message", message);
                    window.location.assign(url.toString());
                } catch (error) {
                    if (error && error.edgeStale) return;
                    setAlarmFormError(error && error.message ? error.message : "告警规则保存失败");
                } finally {
                    if (submitButton) {
                        submitButton.disabled = false;
                        submitButton.textContent = "保存并应用规则";
                    }
                }
            });
        }

    }



    // 设置输入值。
    function setInputValue(input, value) {
        if (input) {
            input.value = value == null ? "" : value;
        }
    }

    // 前端校验用于即时反馈，阈值、计数和点位合法性仍由后端再次校验。
    function validateAlarmRuleForm(form) {
        const masterSelect = form.querySelector("[data-alarm-master-select]");
        const pointSelect = form.querySelector("[data-alarm-point-select]");
        const limitTypeInput = form.querySelector("input[name='limit_type']:checked");
        const limitType = limitTypeInput ? limitTypeInput.value : "off";
        const highEnabled = limitType === "high" || limitType === "both";
        const lowEnabled = limitType === "low" || limitType === "both";
        const high = parseFormNumber((form.querySelector("[data-alarm-high-threshold]") || {}).value, false);
        const low = parseFormNumber((form.querySelector("[data-alarm-low-threshold]") || {}).value, false);
        const hysteresis = parseFormNumber((form.querySelector("[data-alarm-hysteresis]") || {}).value, true);
        const triggerCount = parseInt((form.querySelector("[data-alarm-trigger-count]") || {}).value || "1", 10);
        const recoveryCount = parseInt((form.querySelector("[data-alarm-recovery-count]") || {}).value || "1", 10);
        if (!masterSelect || String(masterSelect.value || "").trim() === "") {
            return "请选择主站。";
        }
        if (!pointSelect || String(pointSelect.value || "").trim() === "" || (pointSelect.selectedOptions[0] || {}).disabled) {
            return "请选择数据项。";
        }
        if (highEnabled && !Number.isFinite(high)) {
            return "请填写有效的上限值。";
        }
        if (lowEnabled && !Number.isFinite(low)) {
            return "请填写有效的下限值。";
        }
        if (highEnabled && lowEnabled && !(low < high)) {
            return "同时启用上下限时，下限必须小于上限。";
        }
        if (!Number.isFinite(hysteresis) || hysteresis < 0) {
            return "回差不能为负数。";
        }
        if (highEnabled && lowEnabled && hysteresis >= high - low) {
            return "同时启用上下限时，回差必须小于上下限差值。";
        }
        if (!Number.isInteger(triggerCount) || triggerCount < 1 || triggerCount > 100) {
            return "连续触发次数必须在 1～100 之间。";
        }
        if (!Number.isInteger(recoveryCount) || recoveryCount < 1 || recoveryCount > 100) {
            return "连续恢复次数必须在 1～100 之间。";
        }
        return "";
    }

    // 解析表单数值。
    function parseFormNumber(value, blankAsZero) {
        if (value == null || String(value).trim() === "") {
            return blankAsZero ? 0 : NaN;
        }
        return Number(value);
    }

    EdgeApp.registerPageController("events", ["events"], { mount: mount });
})();
