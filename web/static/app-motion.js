// 公共微动效控制器：页面空间位移只发生在主内容层，
// 高频数据反馈覆盖上一轮且不执行逐节点同步布局。
(function () {
    "use strict";

    var root = document.documentElement;
    function motionDuration(variableName, fallback) {
        if (!window.getComputedStyle) return fallback;
        var value = String(window.getComputedStyle(root).getPropertyValue(variableName) || "").trim();
        if (/^\d+(?:\.\d+)?ms$/.test(value)) return Number(value.slice(0, -2));
        if (/^\d+(?:\.\d+)?s$/.test(value)) return Number(value.slice(0, -1)) * 1000;
        return fallback;
    }
    var EXIT_DURATION_MS = motionDuration("--motion-exit", 110);
    var VALUE_DURATION_MS = motionDuration("--motion-data", 560);
    var STATUS_DURATION_MS = motionDuration("--motion-status", 480);
    var CONTENT_DURATION_MS = motionDuration("--motion-content", 160);
    var DATA_REGION_DURATION_MS = motionDuration("--motion-data-region", 190);
    var PAGE_DURATION_MS = motionDuration("--motion-page", 145);
    var pressedElement = null;
    var statusObserver = null;
    var valueFeedbackCount = 0;
    var statusFeedbackCount = 0;
    var criticalFeedbackCount = 0;
    var feedbackBudgetTimer = null;
    var initialPageTimer = null;
    var initialPageElement = null;
    var FEEDBACK_BURST_LIMIT = 10;
    var CRITICAL_BURST_LIMIT = 6;
    var statusSelector = [
        ".pill",
        ".status-ok",
        ".status-warn",
        ".status-bad",
        ".status-neutral",
        ".realtime-row-status",
        ".realtime-layer-state",
        ".template-data-item-state",
        "[data-motion-status]"
    ].join(",");
    var contentSelector = [
        ".empty-block",
        ".events-empty-state",
        ".history-empty-state",
        ".realtime-empty-panel",
        ".flash:not(.hidden)",
        ".channel-save-feedback:not(.hidden)",
        "[data-motion-reveal]"
    ].join(",");

    function prefersReducedMotion() {
        if (root.classList.contains("motion-reduced")) return true;
        return Boolean(window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches);
    }

    function animationTimers(element) {
        if (!element.__edgeMotionTimers) element.__edgeMotionTimers = {};
        return element.__edgeMotionTimers;
    }

    function clearClassTimer(element, className) {
        if (!element || !element.__edgeMotionTimers) return;
        var timers = element.__edgeMotionTimers;
        var frame = timers[className + ":frame"];
        if (frame) {
            window.cancelAnimationFrame(frame);
            delete timers[className + ":frame"];
        }
        var timer = timers[className];
        if (timer) {
            window.clearTimeout(timer);
            delete timers[className];
        }
    }

    function clearAuxiliaryClasses(element, className) {
        if (!element || !element.__edgeMotionAuxiliary) return;
        var auxiliary = element.__edgeMotionAuxiliary[className] || [];
        auxiliary.forEach(function (item) {
            element.classList.remove(item);
        });
        delete element.__edgeMotionAuxiliary[className];
    }

    function setAuxiliaryClasses(element, className, classNames) {
        if (!element) return;
        if (!element.__edgeMotionAuxiliary) element.__edgeMotionAuxiliary = {};
        element.__edgeMotionAuxiliary[className] = classNames.slice();
    }

    // 同一节点重复变化时替换上一轮反馈。首次反馈立即写入；仍在播放的
    // 节点在下一帧重启，避免为一批数值逐个读取 offsetWidth 触发同步布局。
    function restartClass(element, className, duration) {
        if (!element) return;
        var wasActive = element.classList.contains(className);
        clearClassTimer(element, className);
        element.classList.remove(className);
        if (prefersReducedMotion()) {
            clearAuxiliaryClasses(element, className);
            return;
        }
        var timers = animationTimers(element);
        var start = function () {
            delete timers[className + ":frame"];
            if (!element || !element.isConnected || prefersReducedMotion()) return;
            element.classList.add(className);
            timers[className] = window.setTimeout(function () {
                if (element) {
                    element.classList.remove(className);
                    clearAuxiliaryClasses(element, className);
                }
                delete timers[className];
            }, duration);
        };
        if (wasActive && window.requestAnimationFrame) {
            timers[className + ":frame"] = window.requestAnimationFrame(start);
        } else {
            start();
        }
    }

    function numericValue(value) {
        var text = String(value == null ? "" : value).trim();
        var match = text.match(/^([-+]?(?:(?:\d{1,3}(?:,\d{3})+)|\d+)(?:\.\d+)?)(?:\s*[^\d+\-.,()（）]*)?$/);
        return match ? Number(match[1].replace(/,/g, "")) : NaN;
    }

    // 软件渲染环境下限制同一批次的重绘节点数量；200ms 后自动恢复预算。
    function claimFeedbackBudget(kind) {
        if (kind === "critical") {
            if (criticalFeedbackCount >= CRITICAL_BURST_LIMIT) return false;
            criticalFeedbackCount += 1;
        } else if (kind === "status") {
            if (statusFeedbackCount >= FEEDBACK_BURST_LIMIT) return false;
            statusFeedbackCount += 1;
        } else {
            if (valueFeedbackCount >= FEEDBACK_BURST_LIMIT) return false;
            valueFeedbackCount += 1;
        }
        if (feedbackBudgetTimer === null) {
            feedbackBudgetTimer = window.setTimeout(function () {
                valueFeedbackCount = 0;
                statusFeedbackCount = 0;
                criticalFeedbackCount = 0;
                feedbackBudgetTimer = null;
            }, 200);
        }
        return true;
    }

    // 数值反馈只在前后展示值确实不同时触发；方向只影响短时底色，不覆盖告警/质量正文颜色。
    function markValue(element, previous, next, options) {
        var before = String(previous == null ? "" : previous);
        var after = String(next == null ? "" : next);
        if (!element || before === after) return false;
        var beforeNumber = numericValue(before);
        var afterNumber = numericValue(after);
        if (!Number.isFinite(beforeNumber) || !Number.isFinite(afterNumber) || beforeNumber === afterNumber) {
            return false;
        }
        if (!claimFeedbackBudget("value")) return false;
        clearAuxiliaryClasses(element, "motion-value-change");
        element.classList.remove("motion-value-up", "motion-value-down");
        var auxiliary = [];
        if ((!options || options.direction !== false) &&
                afterNumber !== beforeNumber) {
            element.classList.add(afterNumber > beforeNumber ? "motion-value-up" : "motion-value-down");
            auxiliary.push(afterNumber > beforeNumber ? "motion-value-up" : "motion-value-down");
        }
        setAuxiliaryClasses(element, "motion-value-change", auxiliary);
        restartClass(element, "motion-value-change", VALUE_DURATION_MS);
        return true;
    }

    function normalizedStatusClass(element) {
        return String(element && element.className || "")
            .replace(/\bmotion-(?:status-change|status-critical)\b/g, "")
            .replace(/\s+/g, " ")
            .trim();
    }

    function statusSignature(element) {
        if (!element) return "";
        return String(element.textContent || "").trim() + "|" + normalizedStatusClass(element);
    }

    function isCriticalStatus(element) {
        var signature = statusSignature(element).toLowerCase();
        return /status-bad|danger|alarm|error|异常|离线|不可达|失败|告警|报警|越限|无效|未连接/.test(signature);
    }

    function markStatus(element, previous, next, options) {
        if (!element || String(previous || "") === String(next || "")) return false;
        var critical = options && options.critical !== undefined
            ? Boolean(options.critical) : isCriticalStatus(element);
        if (!claimFeedbackBudget(critical ? "critical" : "status")) return false;
        clearAuxiliaryClasses(element, "motion-status-change");
        element.classList.toggle("motion-status-critical", critical);
        setAuxiliaryClasses(element, "motion-status-change", critical ? ["motion-status-critical"] : []);
        restartClass(element, "motion-status-change", STATUS_DURATION_MS);
        return true;
    }

    function updateText(element, value, kind) {
        if (!element) return false;
        var previous = String(element.textContent || "");
        var next = value == null || value === "" ? "-" : String(value);
        if (previous === next) return false;
        element.textContent = next;
        if (kind === "value") markValue(element, previous, next);
        else if (kind === "value-neutral") markValue(element, previous, next, { direction: false });
        else if (kind === "status") markStatus(element, previous, next);
        return true;
    }

    function updateStatus(element, text, className, options) {
        if (!element) return false;
        var previous = statusSignature(element);
        element.textContent = text == null || text === "" ? "-" : String(text);
        if (className) element.className = className;
        var next = statusSignature(element);
        element.__edgeMotionStatusSignature = next;
        return markStatus(element, previous, next, options);
    }

    function reveal(element) {
        restartClass(element, "motion-content-enter", CONTENT_DURATION_MS);
    }

    function enterDataRegion(element) {
        restartClass(element, "motion-data-region-enter", DATA_REGION_DURATION_MS);
    }

    function clearInitialPageEntry() {
        if (initialPageTimer !== null) {
            window.clearTimeout(initialPageTimer);
            initialPageTimer = null;
        }
        if (initialPageElement) {
            initialPageElement.removeEventListener("animationend", finishInitialPageEntry);
            initialPageElement = null;
        }
        root.classList.remove("motion-enabled");
    }

    function finishInitialPageEntry(event) {
        if (event && (event.target !== initialPageElement || event.animationName !== "edge-page-enter")) return;
        clearInitialPageEntry();
    }

    // 首屏类只覆盖第一次绘制。软导航开始前也会主动清除，避免软导航类移除后
    // 持久首屏选择器重新激活 edge-page-enter，形成第二次落位。
    function initInitialPageEntry() {
        initialPageElement = document.querySelector(".content-stack");
        if (!initialPageElement || prefersReducedMotion()) {
            clearInitialPageEntry();
            return;
        }
        initialPageElement.addEventListener("animationend", finishInitialPageEntry);
        initialPageTimer = window.setTimeout(finishInitialPageEntry, PAGE_DURATION_MS + 80);
    }

    // 软导航替换内容后使用独立 keyframe 名称启动，无需对大页面强制同步重排。
    function enterPage(element) {
        if (!element) return;
        clearInitialPageEntry();
        clearClassTimer(element, "motion-page-enter");
        element.classList.remove("motion-page-enter");
        if (prefersReducedMotion()) return;
        element.classList.add("motion-page-enter");
        animationTimers(element)["motion-page-enter"] = window.setTimeout(function () {
            element.classList.remove("motion-page-enter");
            delete animationTimers(element)["motion-page-enter"];
        }, PAGE_DURATION_MS);
    }

    function beginNavigation(container) {
        clearInitialPageEntry();
        cancelWithin(container || document);
    }

    function cancel(element, className) {
        if (!element || !className) return;
        clearClassTimer(element, className);
        element.classList.remove(className);
    }

    function clearElementMotion(element) {
        if (!element) return;
        var timers = element.__edgeMotionTimers || {};
        Object.keys(timers).forEach(function (key) {
            window.clearTimeout(timers[key]);
            window.cancelAnimationFrame(timers[key]);
        });
        element.__edgeMotionTimers = {};
        element.__edgeMotionAuxiliary = {};
        element.classList.remove(
            "motion-content-enter",
            "motion-data-region-enter",
            "motion-page-enter",
            "motion-value-change",
            "motion-value-up",
            "motion-value-down",
            "motion-status-change",
            "motion-status-critical",
            "motion-closing"
        );
    }

    function cancelWithin(container) {
        if (!container) return;
        var selector = [
            ".motion-content-enter",
            ".motion-data-region-enter",
            ".motion-page-enter",
            ".motion-value-change",
            ".motion-value-up",
            ".motion-value-down",
            ".motion-status-change",
            ".motion-status-critical",
            ".motion-closing"
        ].join(",");
        if (typeof container.matches === "function" && container.matches(selector)) {
            clearElementMotion(container);
        }
        Array.prototype.forEach.call(container.querySelectorAll(selector), clearElementMotion);
    }

    function cancelExit(element) {
        if (!element) return;
        clearClassTimer(element, "motion-closing");
        element.classList.remove("motion-closing");
    }

    // 业务关闭状态立即生效；节点仅为完成 110ms 退出视觉而稍后设置 hidden。
    function hideAfterExit(element, callback) {
        if (!element) {
            if (callback) callback();
            return;
        }
        cancelExit(element);
        if (prefersReducedMotion()) {
            if (callback) callback();
            return;
        }
        element.classList.add("motion-closing");
        animationTimers(element)["motion-closing"] = window.setTimeout(function () {
            element.classList.remove("motion-closing");
            delete animationTimers(element)["motion-closing"];
            if (callback) callback();
        }, EXIT_DURATION_MS);
    }

    function setReduced(enabled) {
        var reduced = Boolean(enabled);
        root.classList.toggle("motion-reduced", reduced);
        if (reduced) {
            clearInitialPageEntry();
            clearPressed();
            cancelWithin(document);
        }
    }

    function closestInteractive(target) {
        if (!target || typeof target.closest !== "function") return null;
        var element = target.closest([
            "button",
            ".btn",
            "a.nav-item",
            "a.subpage-back-button",
            "a.history-period-tab",
            "a.alarm-tab",
            "a.modbus-view-tab",
            "a.tree-filter-node",
            "[role='button']"
        ].join(","));
        if (!element || element.disabled || element.classList.contains("is-disabled") ||
                element.getAttribute("aria-disabled") === "true") return null;
        return element;
    }

    function clearPressed() {
        if (pressedElement) pressedElement.classList.remove("motion-pressed");
        pressedElement = null;
    }

    function bindTouchFeedback() {
        document.addEventListener("touchstart", function (event) {
            clearPressed();
            pressedElement = closestInteractive(event.target);
            if (pressedElement) pressedElement.classList.add("motion-pressed");
        }, false);
        document.addEventListener("touchend", clearPressed, false);
        document.addEventListener("touchcancel", clearPressed, false);
        window.addEventListener("blur", clearPressed);
        window.addEventListener("pagehide", clearPressed);
    }

    function registerStatus(element) {
        if (!element || typeof element.matches !== "function" || !element.matches(statusSelector)) return;
        // 实时点位由 realtime.js 按质量/告警优先级显式调度。忽略其数字子节点
        // 引发的通用状态观察，避免数值变化把整个 metric-chip 当状态块闪烁。
        if (element.hasAttribute("data-realtime-point-key")) return;
        var next = statusSignature(element);
        var previous = element.__edgeMotionStatusSignature;
        element.__edgeMotionStatusSignature = next;
        if (previous !== undefined && previous !== next) markStatus(element, previous, next);
    }

    function scanStatus(rootNode, revealNewContent) {
        if (!rootNode || rootNode.nodeType !== 1) return;
        registerStatus(rootNode);
        if (rootNode.querySelectorAll) {
            Array.prototype.forEach.call(rootNode.querySelectorAll(statusSelector), registerStatus);
        }
        if (revealNewContent) {
            if (rootNode.matches && rootNode.matches(contentSelector)) reveal(rootNode);
            if (rootNode.querySelectorAll) {
                Array.prototype.forEach.call(rootNode.querySelectorAll(contentSelector), reveal);
            }
        }
    }

    function initStatusObserver() {
        if (!window.MutationObserver || !document.body) return;
        if (statusObserver) statusObserver.disconnect();
        Array.prototype.forEach.call(document.querySelectorAll(statusSelector), registerStatus);
        statusObserver = new MutationObserver(function (records) {
            records.forEach(function (record) {
                if (record.type === "childList") {
                    Array.prototype.forEach.call(record.addedNodes || [], function (node) {
                        scanStatus(node, true);
                    });
                    var parentStatus = record.target.nodeType === 1
                        ? (record.target.matches(statusSelector) ? record.target : record.target.closest(statusSelector))
                        : null;
                    registerStatus(parentStatus);
                } else if (record.type === "characterData") {
                    registerStatus(record.target.parentElement && record.target.parentElement.closest(statusSelector));
                } else if (record.type === "attributes") {
                    registerStatus(record.target);
                }
            });
        });
        statusObserver.observe(document.body, {
            subtree: true,
            childList: true,
            characterData: true,
            attributes: true,
            attributeFilter: ["class"]
        });
    }

    // 页面离开时清除短时 class 与计时器；对 bfcache 返回页同样保持静态终态。
    function cleanupMotion() {
        clearInitialPageEntry();
        clearPressed();
        if (feedbackBudgetTimer !== null) {
            window.clearTimeout(feedbackBudgetTimer);
            feedbackBudgetTimer = null;
        }
        valueFeedbackCount = 0;
        statusFeedbackCount = 0;
        criticalFeedbackCount = 0;
        if (statusObserver) statusObserver.disconnect();
        statusObserver = null;
        cancelWithin(document);
    }

    window.EdgeMotion = {
        cancel: cancel,
        cancelWithin: cancelWithin,
        cancelExit: cancelExit,
        beginNavigation: beginNavigation,
        enterDataRegion: enterDataRegion,
        enterPage: enterPage,
        hideAfterExit: hideAfterExit,
        markStatus: markStatus,
        markValue: markValue,
        prefersReducedMotion: prefersReducedMotion,
        reveal: reveal,
        setReduced: setReduced,
        updateStatus: updateStatus,
        updateText: updateText
    };

    initInitialPageEntry();
    bindTouchFeedback();
    initStatusObserver();
    window.addEventListener("pagehide", cleanupMotion);
    window.addEventListener("pageshow", function (event) {
        if (event.persisted) initStatusObserver();
    });
})();
