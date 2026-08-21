// 控制台前端公共运行库：统一处理 CSRF、提示消息、导航、弹窗、格式化和安全文本转义。
// 页面脚本只通过 window.EdgeApp 使用这些能力，避免各页面重复实现并产生行为差异。
(function () {
    "use strict";

    // 兼容标准和旧 Chromium 前缀版 Page Visibility API；不支持时按可见处理。
    function isPageHidden() {
        if (typeof document.hidden === "boolean") {
            return document.hidden;
        }
        if (typeof document.webkitHidden === "boolean") {
            return document.webkitHidden;
        }
        return false;
    }

    // 绑定页面可见性变化；旧浏览器没有该能力时保留正常定时刷新回退。
    function onPageVisibilityChange(listener) {
        if (typeof listener !== "function") {
            return false;
        }
        if ("hidden" in document) {
            document.addEventListener("visibilitychange", listener);
            return true;
        }
        if ("webkitHidden" in document) {
            document.addEventListener("webkitvisibilitychange", listener);
            return true;
        }
        return false;
    }


    // 所有改变服务端状态的请求都从页面 meta 标签读取同一枚 CSRF token。
    function csrfToken() {
        const meta = document.querySelector('meta[name="csrf-token"]');
        return meta ? String(meta.content || "").trim() : "";
    }

    // 合并请求头并写入 CSRF 令牌。
    function withCsrfHeaders(headers) {
        const nextHeaders = Object.assign({}, headers || {});
        const token = csrfToken();
        if (token) {
            nextHeaders["X-CSRF-Token"] = token;
        }
        return nextHeaders;
    }

    // 发送自动携带 CSRF 令牌的请求。
    function csrfFetch(url, options, fetchImplementation) {
        const nextOptions = Object.assign({}, options || {});
        const method = String(nextOptions.method || "GET").toUpperCase();
        if (method !== "GET" && method !== "HEAD" && method !== "OPTIONS") {
            nextOptions.headers = withCsrfHeaders(nextOptions.headers);
        }
        const request = typeof fetchImplementation === "function" ? fetchImplementation : window.fetch;
        return request(url, nextOptions);
    }

    // 统一读取同源 JSON API 响应。服务端写出前已经完成用户可见错误清洗，
    // 因此业务错误应原样交给页面，不能再被前端通用英文过滤降级。
    async function readApiResponse(response, fallback) {
        const fallbackMessage = String(fallback || "操作失败").trim() || "操作失败";
        const status = Number(response && response.status) || 0;
        let rawBody = "";
        try {
            rawBody = await response.text();
        } catch (_error) {
            return {
                ok: false,
                status: status,
                errorCode: "response_read_failed",
                message: fallbackMessage + "：无法读取服务器响应"
            };
        }

        let payload = null;
        if (rawBody.trim()) {
            try {
                payload = JSON.parse(rawBody);
            } catch (_error) {
                return {
                    ok: false,
                    status: status,
                    errorCode: "invalid_response",
                    message: fallbackMessage + "：服务器响应格式不正确" +
                        (status ? "（HTTP " + status + "）" : "")
                };
            }
        }

        if (response && response.ok && payload && payload.success === true) {
            return { ok: true, status: status, data: payload.data, payload: payload };
        }

        const apiError = payload && payload.error && typeof payload.error === "object"
            ? payload.error : null;
        const errorCode = apiError ? String(apiError.code || "").trim() : "";
        const detail = apiError ? String(apiError.message || "").trim() : "";
        if (detail && detail !== "操作失败") {
            return { ok: false, status: status, errorCode: errorCode, message: detail, payload: payload };
        }
        const diagnostics = [];
        if (status) diagnostics.push("HTTP " + status);
        if (errorCode) diagnostics.push("错误码 " + errorCode);
        return {
            ok: false,
            status: status,
            errorCode: errorCode,
            message: fallbackMessage + (diagnostics.length ? "（" + diagnostics.join("，") + "）" : ""),
            payload: payload
        };
    }

    // 读取树形界面状态。
    function readTreeState(key) {
        try {
            const value = JSON.parse(window.localStorage.getItem(key) || "{}");
            return value && typeof value === "object" ? value : {};
        } catch (error) {
            return {};
        }
    }

    // 写入树形界面状态。
    function writeTreeState(key, value) {
        try {
            window.localStorage.setItem(key, JSON.stringify(value || {}));
        } catch (error) {
            // 存储不可用时仍保留当前页面内的折叠状态。
        }
    }


    // ---------- 全局轻量反馈 ----------
    // Toast 同时服务于服务端 flash 和前端异步请求；初始化后统一接管关闭与超时行为。
    function initToasts(rootNode) {
        const scope = rootNode && rootNode.querySelectorAll ? rootNode : document;
        scope.querySelectorAll("[data-toast]").forEach(function (toast) {
            bindToast(toast);
        });
        scope.querySelectorAll("[data-initial-toast]").forEach(function (source) {
            showToast(source.dataset.toastType || "info", source.dataset.toastMessage || "", {
                autohide: source.dataset.toastAutohide !== "false"
            });
            source.remove();
        });
    }

    // 显示提示。
    function showToast(type, message, options) {
        const normalizedMessage = String(message || "").trim();
        if (!normalizedMessage) {
            return null;
        }
        const container = toastContainer(options && options.position);
        if (!container) {
            return null;
        }
        const toast = document.createElement("div");
        toast.className = "toast toast-" + normalizeToastType(type);
        toast.setAttribute("data-toast", "");
        if (!options || options.autohide !== false) {
            toast.setAttribute("data-toast-autohide", "true");
        }

        const messageNode = document.createElement("div");
        messageNode.className = "toast-message";
        messageNode.textContent = normalizedMessage;

        const closeButton = document.createElement("button");
        closeButton.type = "button";
        closeButton.className = "toast-dismiss";
        closeButton.setAttribute("data-toast-dismiss", "");
        closeButton.setAttribute("aria-label", "关闭提示");
        closeButton.title = "关闭提示";
        closeButton.innerHTML = '<svg class="ui-icon" aria-hidden="true"><use href="#icon-close"></use></svg>';

        toast.appendChild(messageNode);
        toast.appendChild(closeButton);
        container.appendChild(toast);
        bindToast(toast);
        return toast;
    }

    // 获取或创建页面提示容器。
    function toastContainer(position) {
        if (position === "top-left") {
            let leftContainer = document.querySelector("[data-toast-container-left]");
            if (!leftContainer) {
                leftContainer = document.createElement("div");
                leftContainer.className = "toast-container toast-container-left";
                leftContainer.setAttribute("data-toast-container-left", "");
                document.body.appendChild(leftContainer);
            }
            return leftContainer;
        }
        return document.querySelector("[data-toast-container]");
    }

    // 绑定提示。
    function bindToast(toast) {
        if (toast.dataset.toastBound === "true") {
            return;
        }
        toast.dataset.toastBound = "true";
        const closeButton = toast.querySelector("[data-toast-dismiss]");
        if (closeButton) {
            closeButton.addEventListener("click", function () {
                dismissToast(toast);
            });
        }
        if (toast.dataset.toastAutohide === "true") {
            window.setTimeout(function () {
                dismissToast(toast);
            }, 3000);
        }
    }

    // 关闭提示。
    function dismissToast(toast) {
        if (!toast || toast.classList.contains("is-dismissing")) {
            return;
        }
        toast.classList.add("is-dismissing");
        window.setTimeout(function () {
            toast.remove();
        }, 180);
    }

    // 规范化提示类型。
    function normalizeToastType(type) {
        switch (String(type || "").toLowerCase()) {
        case "success":
            return "success";
        case "warning":
        case "warn":
            return "warning";
        case "error":
            return "error";
        default:
            return "info";
        }
    }

    // 显示默认密码提醒一次。
    function showDefaultPasswordReminderOnce() {
        if (!document.body || document.body.dataset.passwordChangeRecommended !== "true") {
            return;
        }
        const csrfMeta = document.querySelector('meta[name="csrf-token"]');
        const sessionMarker = csrfMeta ? String(csrfMeta.content || "").trim() : "";
        const storageKey = "edge.default-password-reminder.session";
        try {
            if (sessionMarker && window.sessionStorage.getItem(storageKey) === sessionMarker) {
                return;
            }
            if (sessionMarker) {
                window.sessionStorage.setItem(storageKey, sessionMarker);
            }
        } catch (error) {
            // 会话存储不可用时仍显示本页提醒，不阻塞系统进入。
        }
        showToast("warning", "当前仍在使用默认密码，建议尽快修改。", { position: "top-left" });
    }



    // 将空值转换为统一的占位文本。
    function defaultDisplayText(value) {
        const text = String(value || "").trim();
        return text || "-";
    }


    // 左上角按钮打开权限感知的全菜单；常驻侧栏继续只显示一级页面图标。
    function initGlobalNavigation() {
        const toggle = document.querySelector("[data-nav-toggle]");
        const overlay = document.querySelector("[data-global-nav]");
        const dialog = overlay ? overlay.querySelector(".global-nav-dialog") : null;
        const grid = overlay ? overlay.querySelector("[data-global-nav-grid]") : null;
        if (!toggle || !overlay || !dialog || !grid) {
            return;
        }

        document.querySelectorAll(".side-nav .nav-item").forEach(function (item) {
            const menuItem = item.cloneNode(true);
            menuItem.classList.add("global-nav-item");
            menuItem.removeAttribute("title");
            menuItem.addEventListener("click", function () {
                closeNavigation(false);
            });
            grid.appendChild(menuItem);
        });

        const openNavigation = function () {
            if (window.EdgeMotion) window.EdgeMotion.cancelExit(overlay);
            overlay.hidden = false;
            overlay.setAttribute("aria-hidden", "false");
            toggle.setAttribute("aria-expanded", "true");
            document.body.classList.add("global-nav-open");
            dialog.focus();
        };

        // 关闭导航。
        function closeNavigation(restoreFocus) {
            overlay.setAttribute("aria-hidden", "true");
            toggle.setAttribute("aria-expanded", "false");
            document.body.classList.remove("global-nav-open");
            if (window.EdgeMotion) {
                window.EdgeMotion.hideAfterExit(overlay, function () {
                    overlay.hidden = true;
                });
            } else {
                overlay.hidden = true;
            }
            if (restoreFocus !== false) toggle.focus();
        }

        toggle.addEventListener("click", openNavigation);
        overlay.querySelectorAll("[data-global-nav-close]").forEach(function (button) {
            button.addEventListener("click", function () {
                closeNavigation(true);
            });
        });
        document.addEventListener("keydown", function (event) {
            if (overlay.getAttribute("aria-hidden") !== "false") {
                return;
            }
            if (event.key === "Escape") {
                closeNavigation(true);
                return;
            }
            if (event.key !== "Tab") {
                return;
            }
            const focusable = Array.from(dialog.querySelectorAll(
                'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])'
            )).filter(function (element) {
                return element.getClientRects().length > 0;
            });
            if (!focusable.length) {
                event.preventDefault();
                dialog.focus();
                return;
            }
            const first = focusable[0];
            const last = focusable[focusable.length - 1];
            if (event.shiftKey && (document.activeElement === first || !dialog.contains(document.activeElement))) {
                event.preventDefault();
                last.focus();
            } else if (!event.shiftKey && (document.activeElement === last || !dialog.contains(document.activeElement))) {
                event.preventDefault();
                first.focus();
            }
        });
    }


    // 初始化控制台时钟。
    function initConsoleClock() {
        const clock = document.getElementById("console-clock");
        if (!clock) {
            return;
        }

        const renderClock = function () {
            const now = new Date();
            const pad = function (value) { return String(value).padStart(2, "0"); };
            clock.textContent = [
                now.getFullYear(),
                "-",
                pad(now.getMonth() + 1),
                "-",
                pad(now.getDate()),
                " ",
                pad(now.getHours()),
                ":",
                pad(now.getMinutes()),
                ":",
                pad(now.getSeconds())
            ].join("");
        };

        let clockTimer = null;
        const stopClock = function () {
            if (clockTimer !== null) {
                window.clearInterval(clockTimer);
                clockTimer = null;
            }
        };
        const startClock = function () {
            stopClock();
            if (isPageHidden()) {
                return;
            }
            renderClock();
            clockTimer = window.setInterval(renderClock, 1000);
        };

        startClock();
        onPageVisibilityChange(function () {
            if (isPageHidden()) stopClock();
            else startClock();
        });
        window.addEventListener("pagehide", stopClock);
        window.addEventListener("pageshow", startClock);
    }


    // 初始化通道弹窗。
    function initChannelModalDelegation() {
        document.addEventListener("click", function (event) {
            const openButton = event.target.closest("[data-channel-modal-open]");
            if (openButton) {
                const modal = document.getElementById(openButton.dataset.channelModalOpen || "");
                if (modal) openModal(modal);
                return;
            }
            const closeButton = event.target.closest("[data-channel-modal-close]");
            if (!closeButton) return;
            const modal = closeButton.closest(".channel-modal");
            if (modal) closeModal(modal);
        });
    }

    let pendingConfirmResolve = null;

    // 使用现有 channel-modal 视觉提供统一程序内确认，返回 true/false 供业务流程继续判断。
    function confirmAction(options) {
        const modal = document.querySelector("[data-app-confirm-modal]");
        if (!modal) {
            return Promise.resolve(false);
        }
        if (pendingConfirmResolve) {
            pendingConfirmResolve(false);
            pendingConfirmResolve = null;
        }
        const settings = options && typeof options === "object" ? options : {};
        modal.querySelector("[data-app-confirm-title]").textContent = String(settings.title || "确认操作");
        modal.querySelector("[data-app-confirm-message]").textContent = String(settings.message || "确定继续吗？");
        const accept = modal.querySelector("[data-app-confirm-accept]");
        accept.textContent = String(settings.confirmText || "确认执行");
        accept.className = "btn " + (settings.danger ? "btn-danger" : "btn-primary");
        accept.disabled = false;

        const context = modal.querySelector("[data-app-confirm-context]");
        context.replaceChildren();
        const rows = Array.isArray(settings.context) ? settings.context : [];
        rows.forEach(function (row) {
            const item = document.createElement("div");
            const label = document.createElement("span");
            const value = document.createElement("strong");
            label.textContent = String(row && row.label || "");
            value.textContent = String(row && row.value || "-");
            item.append(label, value);
            context.appendChild(item);
        });
        context.classList.toggle("hidden", rows.length === 0);
        openModal(modal);
        accept.focus();
        return new Promise(function (resolve) {
            pendingConfirmResolve = resolve;
        });
    }

    function settleConfirm(confirmed) {
        const modal = document.querySelector("[data-app-confirm-modal]");
        const resolve = pendingConfirmResolve;
        pendingConfirmResolve = null;
        if (modal) closeModal(modal);
        if (resolve) resolve(Boolean(confirmed));
    }

    function initAppConfirmDialog() {
        document.addEventListener("click", function (event) {
            if (event.target.closest("[data-app-confirm-accept]")) {
                const button = event.target.closest("[data-app-confirm-accept]");
                button.disabled = true;
                settleConfirm(true);
                return;
            }
            if (event.target.closest("[data-app-confirm-cancel]")) settleConfirm(false);
        });
    }

    // 绑定 Escape 键关闭当前顶层弹窗。
    function bindGlobalModalEscape() {
        document.addEventListener("keydown", function (event) {
            const openModals = document.querySelectorAll(".channel-modal.open");
            const activeModal = openModals.length ? openModals[openModals.length - 1] : null;
            if (event.key === "Tab" && activeModal) {
                const focusable = Array.from(activeModal.querySelectorAll("button:not([disabled]), a[href], input:not([disabled]):not([type='hidden']), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex='-1'])"))
                    .filter(function (node) { return node.getClientRects().length > 0; });
                if (focusable.length) {
                    const first = focusable[0];
                    const last = focusable[focusable.length - 1];
                    if (event.shiftKey && document.activeElement === first) {
                        event.preventDefault();
                        last.focus();
                    } else if (!event.shiftKey && document.activeElement === last) {
                        event.preventDefault();
                        first.focus();
                    }
                }
                return;
            }
            if (event.key !== "Escape") {
                return;
            }
            if (activeModal) {
                closeModal(activeModal);
            }
        });
    }

    // 弹窗开关集中维护 aria-hidden 与页面滚动状态，保证键盘和读屏行为一致。
    function openModal(modal) {
        if (!modal || modal.classList.contains("open")) {
            return;
        }
        if (window.EdgeVirtualKeyboard) {
            window.EdgeVirtualKeyboard.hide();
        }
        modal._returnFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null;
        modal.classList.add("open");
        modal.setAttribute("aria-hidden", "false");
        document.body.classList.add("modal-open");
        const focusTarget = modal.querySelector("input:not([type='hidden']), select, textarea, button");
        if (focusTarget) {
            focusTarget.focus();
        }
    }

    // 关闭弹窗。
    function closeModal(modal) {
        if (!modal || !modal.classList.contains("open")) {
            return;
        }
        if (modal.matches("[data-app-confirm-modal]") && pendingConfirmResolve) {
            const resolve = pendingConfirmResolve;
            pendingConfirmResolve = null;
            resolve(false);
        }
        if (window.EdgeVirtualKeyboard) {
            window.EdgeVirtualKeyboard.hide();
        }
        const returnFocus = modal._returnFocus;
        modal._returnFocus = null;
        modal.classList.remove("open");
        modal.setAttribute("aria-hidden", "true");
        if (modal.hasAttribute("data-clear-passwords-on-close")) {
            modal.querySelectorAll('input[type="password"]').forEach(function (input) {
                input.value = "";
            });
        }
        if (!document.querySelector(".channel-modal.open")) {
            document.body.classList.remove("modal-open");
        }
        if (returnFocus && returnFocus.isConnected) {
            returnFocus.focus();
        }
    }

    // 软导航会直接替换主内容，不能依赖旧页面弹窗的关闭按钮完成清理。
    // 这里同步复位所有跨页面瞬态状态，避免后退/前进后遗留滚动锁或遮罩层。
    function resetTransientUI() {
        if (window.EdgeVirtualKeyboard && typeof window.EdgeVirtualKeyboard.hide === "function") {
            window.EdgeVirtualKeyboard.hide();
        }

        if (pendingConfirmResolve) {
            const resolve = pendingConfirmResolve;
            pendingConfirmResolve = null;
            resolve(false);
        }

        document.querySelectorAll(".channel-modal.open").forEach(function (modal) {
            modal.classList.remove("open");
            modal.setAttribute("aria-hidden", "true");
            modal._returnFocus = null;
            if (modal.hasAttribute("data-clear-passwords-on-close")) {
                modal.querySelectorAll('input[type="password"]').forEach(function (input) {
                    input.value = "";
                });
            }
        });

        // 告警规则弹窗使用独立遮罩结构，没有 channel-modal 的 open 类。
        document.querySelectorAll("[data-alarm-modal]:not([hidden])").forEach(function (modal) {
            modal.hidden = true;
            modal.setAttribute("aria-hidden", "true");
        });

        const navigation = document.querySelector("[data-global-nav]");
        if (navigation) {
            navigation.hidden = true;
            navigation.setAttribute("aria-hidden", "true");
        }
        const navigationToggle = document.querySelector("[data-nav-toggle]");
        if (navigationToggle) navigationToggle.setAttribute("aria-expanded", "false");

        document.body.classList.remove("modal-open", "global-nav-open", "virtual-keyboard-open");
        document.documentElement.style.removeProperty("--virtual-keyboard-height");
    }


    // 后端错误可能包含内部英文诊断；这里仅转换已知模式，未知内容回退为安全中文提示。
    function friendlyApiMessage(rawMessage, fallback) {
        // 清理 API 错误码前缀，并判断原始消息是否已包含中文。
        let message = String(rawMessage || "").trim();
        if (!message || message === "-") {
            return fallback || "";
        }
        const prefixed = message.match(/^[a-z_]+:\s+(.+)$/i);
        if (prefixed) {
            message = prefixed[1];
        }

        const lower = message.toLowerCase();
        const hasChinese = /[\u4e00-\u9fff]/.test(message);

        // 按稳定的错误片段匹配用户可理解的中文描述。
        const mappings = [
            ["device not found:", "设备不存在："],
            ["请选择设备模板", "请选择设备类型"],
            ["设备模板不存在", "设备类型不存在"],
            ["所属主控未配置设备模板", "所属主站未配置设备类型"],
            ["所属主站未配置设备模板", "所属主站未配置设备类型"],
            ["设备由主控和设备模板自动推导", "设备由主站配置的设备数量、起始地址和设备类型读取区块自动生成，请在主站配置中调整。"],
            ["设备由主站和设备模板自动推导", "设备由主站配置的设备数量、起始地址和设备类型读取区块自动生成，请在主站配置中调整。"],
            ["当前采集链路仅接受 modbus rtu 主控", "当前采集链路仅接受 Modbus RTU 主站"],
            ["当前采集链路仅接受 modbus rtu 或 modbus tcp 主控", "当前采集链路仅接受 Modbus RTU 或 Modbus TCP 主站"],
            ["tcp 连接失败", "TCP 连接失败，请检查远端 IP / 主机名、端口和设备 TCP 服务状态。"],
            ["tcp 连接超时", "TCP 连接超时，请检查网络连通性和远端端口是否开放。"],
            ["tcp 发送失败", "TCP 发送失败，请检查网络链路、远端连接状态和 Modbus TCP 服务稳定性。"],
            ["tcp 响应超时", "TCP 响应超时，请检查 Unit ID、寄存器范围和响应超时配置。"],
            ["tcp 远端关闭连接", "TCP 远端关闭连接，请检查远端 Modbus TCP 服务和请求参数。"],
            ["mbap transaction id 不匹配", "MBAP 事务标识不匹配"],
            ["mbap protocol id 异常", "MBAP 协议标识异常"],
            ["mbap length 异常", "MBAP 长度字段异常"],
            ["unit id 不匹配", "Modbus TCP Unit ID 不匹配"],
            ["modbus tcp 异常码", "Modbus TCP 异常响应"],
            ["byte count 异常", "Modbus TCP 响应数据长度异常"],
            ["tcp 远端地址不能为空", "TCP 远端地址不能为空"],
            ["tcp 远端端口必须在", "TCP 远端端口必须在 1-65535 范围内"],
            ["tcp 建连超时时间必须在", "TCP 建连超时时间必须在 1-60000 ms 范围内"],
            ["主控寄存器数量必须为", "当前主站配置包含已停用的连续采集数量规则，请重新打开主站配置并直接填写设备数量。"],
            ["主控寄存器数量必须大于 0", "当前主站配置缺少有效设备数量，请重新打开主站配置并填写大于 0 的设备数量。"],
            ["单次读取寄存器数量超出范围", "单次读取寄存器数量不能超过 125。"],
            ["单次读取保持寄存器数量超出范围", "单次读取保持寄存器数量不能超过 125。"],
            ["该设备寄存器范围与同主控下已有设备重叠", "该设备寄存器范围与同主站下已有设备重叠，请调整块内偏移。"],
            ["该设备寄存器范围与同主站下已有设备重叠", "该设备寄存器范围与同主站下已有设备重叠，请调整块内偏移。"],
            ["channel is still referenced by master:", "仍有主站绑定该通道，无法删除："],
            ["bound channel does not exist:", "绑定的通道不存在："],
            ["master created:", "主站已创建："],
            ["master saved:", "主站已保存："],
            ["master deleted:", "主站已删除："],
            ["master_id already exists:", "主站 ID 已存在："],
            ["channel_id already exists:", "通道 ID 已存在："],
            ["master not found:", "主站不存在："],
            ["channel not found:", "通道不存在："],
            ["channel is not ready", "通道尚未就绪"],
            ["failed to open target channel", "打开目标通道失败"],
            ["protocol is invalid", "协议类型无效"],
            ["channel_id is required for modbus_rtu", "Modbus RTU 主站必须选择关联通道"],
            ["master_name is required", "主站名称不能为空"],
            ["master_id is required", "主站 ID 不能为空"],
            ["channel_name is required", "通道名称不能为空"],
            ["channel_id is required", "通道 ID 不能为空"],
            ["port_name must be an absolute /dev/* path", "串口路径必须是 /dev/* 格式的绝对路径"],
            ["unsupported baud_rate", "当前仅支持常用串口波特率"],
            ["data_bits must be between 5 and 8", "数据位必须在 5 到 8 之间"],
            ["stop_bits must be 1 or 2", "停止位必须为 1 或 2"],
            ["response_timeout_ms must be between 1 and 60000", "超时时间必须在 1 到 60000 ms 之间"],
            ["retry_count must be between 0 and 10", "重试次数必须在 0 到 10 之间"],
            ["target_address must be between 1 and 247", "目标地址必须在 1 到 247 之间"],
            ["poll_interval_ms must be between 1 and 3600000", "轮询周期必须在 1 到 3600000 ms 之间"],
            ["device_count must be between 1 and 256", "设备数量必须在 1 到 256 之间"],
            ["failed to reload master config", "主站配置已写入，但重新加载失败"],
            ["failed to reload channel config", "通道配置已写入，但重新加载失败"],
            ["failed to read serial attributes", "串口设备无法打开或初始化"],
            ["failed to open serial port", "串口设备无法打开或初始化"],
            ["open serial port failed", "串口设备无法打开或初始化"],
            ["failed to configure serial port", "串口设备无法打开或初始化"],
            ["failed to write to serial port", "写入串口失败"],
            ["failed to read from serial port", "读取串口数据失败"],
            ["serial port not found", "未找到串口设备"],
            ["串口设备无法打开或初始化", "串口设备无法打开或初始化"],
            ["读取串口属性失败", "串口设备无法打开或初始化"],
            ["打开串口失败", "串口设备无法打开或初始化"],
            ["配置串口参数失败", "串口设备无法打开或初始化"],
            ["invalid baud rate", "波特率无效"],
            ["failed to fetch", "请求失败"],
            ["network error", "网络请求失败"],
            ["internal server error", "服务器内部错误"],
            ["bad request", "请求参数无效"],
            ["unauthorized", "未授权访问"],
            ["forbidden", "无权执行当前操作"],
            ["not found", "目标不存在"],
            ["conflict", "资源状态冲突"],
            ["context deadline exceeded", "操作超时"],
            ["timed out", "操作超时"],
            ["timeout", "操作超时"],
            ["cancelled", "操作已取消"],
            ["canceled", "操作已取消"],
            ["unavailable", "服务暂不可用"],
            ["connection refused", "连接被拒绝"],
            ["connection reset by peer", "连接已重置"],
            ["broken pipe", "连接已中断"],
            ["unexpected eof", "连接意外中断"],
            ["eof", "连接已断开"],
            ["no route to host", "无法到达目标主机"],
            ["host is down", "目标主机不可用"],
            ["permission denied", "通道打开失败：当前用户没有串口访问权限，请检查 dialout 权限或设备权限配置。"],
            ["read-only file system", "文件系统为只读"],
            ["file exists", "目标已存在"],
            ["device or resource busy", "通道打开失败：串口设备已被其他程序占用，请关闭占用程序后重试。"],
            ["device busy", "设备被占用"],
            ["port busy", "设备被占用"],
            ["input/output error", "通道打开失败：串口设备不可访问，请检查串口是否真实存在、虚拟机/系统是否正确映射串口、设备是否异常。"],
            ["no such file or directory", "通道打开失败：串口设备不存在，请检查串口路径是否正确。"],
            ["invalid argument", "请求参数无效"],
            ["invalid parameter", "请求参数无效"],
            ["invalid character", "JSON 内容格式错误"],
            ["unexpected token", "响应格式不正确"],
            ["unexpected end of json input", "JSON 内容不完整"],
            ["is not valid json", "响应格式不正确"],
            ["malformed json", "JSON 格式错误"],
            ["decode failed", "数据解析失败"],
            ["encode failed", "数据编码失败"],
            ["parse failed", "解析失败"],
            ["create failed", "创建失败"],
            ["update failed", "更新失败"],
            ["delete failed", "删除失败"],
            ["save failed", "保存失败"],
            ["load failed", "加载失败"],
            ["login failed", "登录失败"],
            ["start polling failed", "轮询服务操作失败"],
            ["stop polling failed", "轮询服务操作失败"],
            ["unknown error", "操作失败"]
        ];

        for (let index = 0; index < mappings.length; index += 1) {
            const source = mappings[index][0];
            const target = mappings[index][1];
            const position = lower.indexOf(source);
            if (position >= 0) {
                const tail = message.slice(position + source.length).trim();
                if (source.endsWith(":") && tail) {
                    return target + tail;
                }
                return target;
            }
        }

        // 安全的中文消息原样保留，其余英文诊断按类别生成兜底提示。
        if (hasChinese && !hasUnsafeEnglishFragment(message)) {
            return message;
        }
        return fallbackApiMessageForEnglish(lower, fallback);
    }

    // 判断是否包含不安全英文片段。
    function hasUnsafeEnglishFragment(text) {
        const safeAcronyms = /\b(IP|TCP|RTU|Modbus|MBAP|SQLite|JSON|FC\d+|Unit|ID|ms)\b/gi;
        const cleaned = String(text || "")
            // 设备类型、字段和读取区块的内部标识由页面及后端限制为
            // [a-z0-9_-]。它们是用户可操作信息，不应让一条中文校验错误
            // 被降级成笼统的“保存失败”。
            .replace(/(?:模板\s*ID|设备类型\s*ID|字段\s*(?:key|ID|标识)|采集点\s*(?:key|ID|标识)|读取区块\s*(?:key|ID|标识)|区块\s*(?:key|ID|标识))(?:\s*(?:已存在|不存在|重复|无效|不可编辑))?[：:]\s*[a-z0-9_-]+/gi, "")
            .replace(safeAcronyms, "")
            .replace(/0x[0-9a-f]+/gi, "")
            .replace(/\/dev\/\*/g, "")
            .replace(/\/dev\/[A-Za-z0-9/_-]+/g, "")
            .replace(/[0-9]+/g, "");
        return /[A-Za-z_]{2,}/.test(cleaned);
    }

    // 为未本地化的英文错误生成中文兜底提示。
    function fallbackApiMessageForEnglish(lower, fallback) {
        if (lower.includes("serial") || lower.includes("/dev/") || lower.includes("tty")) {
            return "串口通信异常";
        }
        if (lower.includes("json") || lower.includes("decode") || lower.includes("encode") || lower.includes("parse")) {
            return "数据解析失败";
        }
        if (lower.includes("connection") || lower.includes("network") || lower.includes("fetch") || lower.includes("host")) {
            return "网络连接异常";
        }
        return fallback || "操作失败";
    }

    // 根据诊断状态生成处理建议。
    function diagnosisSuggestion(diagnosis) {
        if (!diagnosis || !diagnosis.suggestion) {
            return "";
        }
        const code = String(diagnosis.error_code || "").trim();
        if (!code || code.toUpperCase() === "NONE") {
            return "";
        }
        const status = String(diagnosis.status || "").trim().toLowerCase();
        if (status === "normal") {
            return "";
        }
        return friendlyApiMessage(diagnosis.suggestion || "", "请检查相关配置和现场连接状态。");
    }


    // ---------- 无副作用的通用 DOM/格式化辅助函数 ----------
    function readFieldValue(form, name) {
        const field = form.elements.namedItem(name);
        return field ? String(field.value || "").trim() : "";
    }



    // 设置反馈。
    function setFeedback(container, kind, message, options) {
        if (!container) {
            return;
        }
        if (!message) {
            container.className = "channel-save-feedback hidden";
            container.textContent = "";
            return;
        }
        container.className = "channel-save-feedback flash flash-" + kind;
        container.textContent = message;
        if (window.EdgeMotion) {
            window.EdgeMotion.reveal(container);
        }
        showToast(kind, message, options ? options.toastOptions : undefined);
    }


    // 格式化时间戳。
    function formatTimestamp(value) {
        if (!value) {
            return "-";
        }
        if (value < 1000000000000) {
            return value + " ms";
        }
        const date = new Date(Number(value));
        const pad = function (n) { return String(n).padStart(2, "0"); };
        return [
            date.getFullYear(),
            "-",
            pad(date.getMonth() + 1),
            "-",
            pad(date.getDate()),
            " ",
            pad(date.getHours()),
            ":",
            pad(date.getMinutes()),
            ":",
            pad(date.getSeconds())
        ].join("");
    }


    // 动态拼接 HTML 前必须经过本函数，禁止直接插入来自设备或后端的原始文本。
    function escapeHtml(value) {
        return String(value)
            .replaceAll("&", "&amp;")
            .replaceAll("<", "&lt;")
            .replaceAll(">", "&gt;")
            .replaceAll('"', "&quot;")
            .replaceAll("'", "&#39;");
    }

    // 返回数据点位的中文显示名称。
    function dataItemDisplayName(name, key) {
        const normalizedName = String(name || "").trim();
        const normalizedKey = String(key || "").trim();
        if (normalizedName && !looksLikeInternalFieldName(normalizedName)) {
            return normalizedName;
        }
        const known = knownDataItemName(normalizedKey) || knownDataItemName(normalizedName);
        if (known) {
            return known;
        }
        return "数据项";
    }


    // 返回已知数据点位的固定中文名称。
    function knownDataItemName(value) {
        switch (String(value || "").trim().toLowerCase()) {
        case "resistance":
        case "ground_resistance":
            return "接地电阻";
        case "temperature":
            return "温度";
        case "battery_voltage":
            return "电池电压";
        case "signal_strength":
            return "信号强度";
        case "current_status":
            return "当前状态";
        case "running_status_flags":
            return "运行状态标志";
        case "motor_stop_time":
            return "电机停机时间";
        case "last_test_time":
            return "最近一次绝缘测试时间";
        case "leakage_current":
            return "剩余电流";
        case "insulation_resistance":
            return "绝缘电阻";
        case "voltage":
            return "电压";
        case "humidity":
            return "湿度";
        default:
            return "";
        }
    }

    // 判断名称是否形似内部字段标识。
    function looksLikeInternalFieldName(value) {
        return /^[A-Za-z0-9_]+$/.test(String(value || "").trim());
    }

    window.EdgeApp = Object.assign(window.EdgeApp || {}, {
        csrfFetch: csrfFetch,
        readApiResponse: readApiResponse,
        isPageHidden: isPageHidden,
        onPageVisibilityChange: onPageVisibilityChange,
        readTreeState: readTreeState,
        writeTreeState: writeTreeState,
        showToast: showToast,
        friendlyApiMessage: friendlyApiMessage,
        diagnosisSuggestion: diagnosisSuggestion,
        formatTimestamp: formatTimestamp,
        escapeHtml: escapeHtml,
        dataItemDisplayName: dataItemDisplayName,
        defaultDisplayText: defaultDisplayText,
        readFieldValue: readFieldValue,
        setFeedback: setFeedback,
        hydratePage: initToasts,
        openModal: openModal,
        closeModal: closeModal,
        confirmAction: confirmAction,
        resetTransientUI: resetTransientUI
    });

    initToasts(document);
    showDefaultPasswordReminderOnce();
    initGlobalNavigation();
    initConsoleClock();
    initChannelModalDelegation();
    initAppConfirmDialog();
    bindGlobalModalEscape();
})();
