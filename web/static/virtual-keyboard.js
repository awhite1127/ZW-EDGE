(function () {
    "use strict";

    var STORAGE_KEY = "edge.virtualKeyboard.override";
    var RECENT_WORDS_KEY = "edge.virtualKeyboard.recentWords";
    var RECENT_WORDS_LIMIT = 10;
    var keyboardRoot = null;
    var activeTarget = null;
    var activeConfig = null;
    var dirty = false;
    var uppercase = false;
    var chineseMode = false;
    var pinyinComposition = "";
    var candidatePage = 0;
    var temporaryNumberInput = false;
    var enabled = false;
    var lastTouchKeyTime = 0;
    var pressedKey = null;
    var touchKey = null;
    var touchStartX = 0;
    var touchStartY = 0;
    var touchMoved = false;
    var pendingPointerTarget = null;
    var pendingPointerTimer = 0;

    function queryValue(search, name) {
        var source = String(search || "").replace(/^\?/, "");
        if (!source) return "";
        var parts = source.split("&");
        for (var index = 0; index < parts.length; index += 1) {
            var pair = parts[index].split("=");
            var key = "";
            var value = "";
            try {
                key = decodeURIComponent((pair.shift() || "").replace(/\+/g, " "));
                value = decodeURIComponent(pair.join("=").replace(/\+/g, " "));
            } catch (_) {
                continue;
            }
            if (key === name) return value;
        }
        return "";
    }

    function currentOverride() {
        var direct = queryValue(window.location.search, "keyboard");
        var nested = "";
        if (direct !== "0" && direct !== "1") {
            var redirect = queryValue(window.location.search, "redirect");
            var question = redirect.indexOf("?");
            if (question >= 0) nested = queryValue(redirect.slice(question), "keyboard");
        }
        var value = direct === "0" || direct === "1" ? direct : nested;
        if (value === "0" || value === "1") {
            try {
                window.sessionStorage.setItem(STORAGE_KEY, value);
            } catch (_) {
                // 隐私模式或禁用存储时，本页查询参数仍然有效。
            }
            return value;
        }
        try {
            value = window.sessionStorage.getItem(STORAGE_KEY) || "";
        } catch (_) {
            value = "";
        }
        return value === "0" || value === "1" ? value : "";
    }

    function hasTouchCapability() {
        return (Number(window.navigator.maxTouchPoints) || 0) > 0 ||
            (Number(window.navigator.msMaxTouchPoints) || 0) > 0 ||
            "ontouchstart" in window;
    }

    function isCompactViewport() {
        var viewportWidth = window.innerWidth || document.documentElement.clientWidth || 0;
        var viewportHeight = window.innerHeight || document.documentElement.clientHeight || 0;
        return viewportWidth > 0 && viewportHeight > 0 && viewportWidth <= 1280 && viewportHeight <= 800;
    }

    function shouldEnable() {
        var override = currentOverride();
        if (override === "1") return true;
        if (override === "0") return false;
        return isCompactViewport() && hasTouchCapability();
    }

    function dispatchFieldEvent(target, name) {
        var event;
        try {
            event = new Event(name, { bubbles: true });
        } catch (_) {
            event = document.createEvent("Event");
            event.initEvent(name, true, false);
        }
        target.dispatchEvent(event);
    }

    function dispatchKeyboardLayoutEvent(open) {
        var event;
        try {
            event = new CustomEvent("edge:virtual-keyboard-layout", { detail: { open: open } });
        } catch (_) {
            event = document.createEvent("CustomEvent");
            event.initCustomEvent("edge:virtual-keyboard-layout", false, false, { open: open });
        }
        window.dispatchEvent(event);
    }

    function fractionalStep(target) {
        var raw = String(target.getAttribute("step") || "").trim().toLowerCase();
        if (raw === "any") return true;
        if (!raw) return false;
        var step = Number(raw);
        return Number.isFinite(step) && Math.floor(step) !== step;
    }

    function targetConfig(target) {
        if (!target || target.nodeType !== 1 || target.disabled || target.readOnly) return null;
        var tag = target.tagName.toLowerCase();
        if (tag !== "input" && tag !== "textarea") return null;

        var explicit = String(target.getAttribute("data-keyboard") || "").toLowerCase();
        if (explicit === "none") return null;
        if (explicit === "text") return { type: "text", decimal: false, negative: false, chinese: false };
        if (explicit === "text-cn") return { type: "text", decimal: false, negative: false, chinese: true };
        if (explicit === "integer") return {
            type: "number",
            decimal: false,
            negative: !isNonNegative(target)
        };
        if (explicit === "decimal") return {
            type: "number",
            decimal: true,
            negative: !isNonNegative(target)
        };
        if (explicit === "ip") return { type: "ip", decimal: false, negative: false };
        if (explicit === "password") return { type: "password", decimal: false, negative: false };

        var type = String(target.getAttribute("type") || "text").toLowerCase();
        var inputMode = String(target.getAttribute("inputmode") || "").toLowerCase();
        if (type === "password") return { type: "password", decimal: false, negative: false };
        if (type === "number") return {
            type: "number",
            decimal: inputMode === "decimal" || fractionalStep(target),
            negative: !isNonNegative(target)
        };
        if (inputMode === "numeric") return { type: "number", decimal: false, negative: !isNonNegative(target) };
        if (inputMode === "decimal") return { type: "number", decimal: true, negative: !isNonNegative(target) };
        if (type === "search") return null;
        if (["hidden", "checkbox", "radio", "date", "datetime-local", "time", "month", "week", "file", "color", "range", "button", "submit", "reset"].indexOf(type) >= 0) {
            return null;
        }
        return { type: "text", decimal: false, negative: false, chinese: false };
    }

    function isNonNegative(target) {
        var rawMin = target.getAttribute("min");
        if (rawMin === null || String(rawMin).trim() === "") return false;
        var min = Number(rawMin);
        return Number.isFinite(min) && min >= 0;
    }

    function selection(target) {
        var length = String(target.value || "").length;
        try {
            return {
                start: typeof target.selectionStart === "number" ? target.selectionStart : length,
                end: typeof target.selectionEnd === "number" ? target.selectionEnd : length
            };
        } catch (_) {
            return { start: length, end: length };
        }
    }

    function setSelection(target, start, end) {
        try {
            target.setSelectionRange(start, end);
        } catch (_) {
            // 不支持选区的输入类型会退化为末尾插入。
        }
    }

    function maxLength(target) {
        var raw = target.getAttribute("maxlength");
        if (raw === null || raw === "") return 0;
        var value = Number(raw);
        return Number.isFinite(value) && value > 0 ? Math.floor(value) : 0;
    }

    function updateValue(value, caret) {
        if (!activeTarget) return;
        if (value === activeTarget.value) {
            setSelection(activeTarget, caret, caret);
            return;
        }
        activeTarget.value = value;
        dirty = true;
        setSelection(activeTarget, caret, caret);
        dispatchFieldEvent(activeTarget, "input");
    }

    function insertText(text) {
        if (!activeTarget) return;
        var current = String(activeTarget.value || "");
        var range = selection(activeTarget);
        var allowed = maxLength(activeTarget);
        var insertion = String(text);
        if (allowed > 0) {
            var available = allowed - (current.length - (range.end - range.start));
            insertion = insertion.slice(0, Math.max(0, available));
        }
        if (!insertion) return;
        var candidate = current.slice(0, range.start) + insertion + current.slice(range.end);
        if (activeConfig.type === "number" && !validNumericDraft(candidate, activeConfig)) return;
        updateValue(candidate, range.start + insertion.length);
    }

    function validNumericDraft(value, config) {
        if (value === "") return true;
        var sign = config.negative ? "-?" : "";
        var pattern = config.decimal ?
            new RegExp("^" + sign + "(?:\\d+\\.?\\d*|\\.\\d*)$") :
            new RegExp("^" + sign + "\\d+$");
        return pattern.test(value);
    }

    function backspace() {
        if (!activeTarget) return;
        if (chineseMode && pinyinComposition) {
            pinyinComposition = pinyinComposition.slice(0, -1);
            candidatePage = 0;
            render();
            return;
        }
        var current = String(activeTarget.value || "");
        var range = selection(activeTarget);
        if (range.start !== range.end) {
            updateValue(current.slice(0, range.start) + current.slice(range.end), range.start);
            return;
        }
        if (range.start <= 0) return;
        updateValue(current.slice(0, range.start - 1) + current.slice(range.end), range.start - 1);
    }

    function clearValue() {
        if (!activeTarget) return;
        pinyinComposition = "";
        candidatePage = 0;
        updateValue("", 0);
        if (chineseMode) render();
    }

    function moveCaret(delta) {
        if (!activeTarget) return;
        var range = selection(activeTarget);
        var position = delta < 0 ? range.start : range.end;
        position = Math.max(0, Math.min(String(activeTarget.value || "").length, position + delta));
        setSelection(activeTarget, position, position);
        activeTarget.focus();
    }

    function toggleMinus() {
        if (!activeTarget || !activeConfig.negative) return;
        var current = String(activeTarget.value || "");
        if (current.charAt(0) === "-") {
            updateValue(current.slice(1), Math.max(0, selection(activeTarget).start - 1));
        } else {
            updateValue("-" + current, selection(activeTarget).start + 1);
        }
    }

    function restoreInputType() {
        if (!activeTarget || !temporaryNumberInput) return;
        var before = activeTarget.value;
        activeTarget.setAttribute("type", "number");
        temporaryNumberInput = false;
        if (activeTarget.value !== before) {
            dirty = true;
            dispatchFieldEvent(activeTarget, "input");
        }
    }

    // 组合中的拼音也是用户已经输入的内容；失焦、完成或提交前必须先落入字段，不能静默丢弃。
    function commitPinyinComposition() {
        if (!activeTarget || !chineseMode || !pinyinComposition) return false;
        var rawComposition = pinyinComposition;
        var committedText = rawComposition;
        if (window.EdgeOfflinePinyin) {
            var matches = window.EdgeOfflinePinyin.search(rawComposition, recentWords());
            if (matches.length && matches[0] && matches[0].text) {
                committedText = String(matches[0].text);
            }
        }
        insertText(committedText);
        if (committedText !== rawComposition) rememberWord(committedText);
        pinyinComposition = "";
        candidatePage = 0;
        return true;
    }

    function finishTarget(keepKeyboard) {
        if (!activeTarget) return;
        var target = activeTarget;
        commitPinyinComposition();
        restoreInputType();
        if (dirty) dispatchFieldEvent(target, "change");
        if (!keepKeyboard && document.activeElement === target) target.blur();
        clearState(keepKeyboard);
    }

    function clearState(keepKeyboard) {
        activeTarget = null;
        activeConfig = null;
        dirty = false;
        uppercase = false;
        chineseMode = false;
        pinyinComposition = "";
        candidatePage = 0;
        temporaryNumberInput = false;
        clearPressedKey();
        if (!keepKeyboard && keyboardRoot) {
            keyboardRoot.setAttribute("aria-hidden", "true");
            if (window.EdgeMotion) {
                window.EdgeMotion.hideAfterExit(keyboardRoot, function () {
                    keyboardRoot.hidden = true;
                });
            } else {
                keyboardRoot.hidden = true;
            }
        }
        if (!keepKeyboard) {
            document.body.classList.remove("virtual-keyboard-open");
            document.documentElement.style.removeProperty("--virtual-keyboard-height");
            dispatchKeyboardLayoutEvent(false);
        }
    }

    function hide() {
        if (!activeTarget) return;
        finishTarget(false);
    }

    function formForTarget(target) {
        if (!target) return null;
        if (target.form) return target.form;
        return target.closest ? target.closest("form") : null;
    }

    function defaultSubmitter(form) {
        if (!form || !form.elements) return null;
        for (var index = 0; index < form.elements.length; index += 1) {
            var control = form.elements[index];
            if (!control || control.disabled) continue;
            var type = String(control.type || "").toLowerCase();
            if (type === "submit" || type === "image") return control;
        }
        return null;
    }

    function completeTarget() {
        if (!activeTarget) return;
        var target = activeTarget;
        var form = formForTarget(target);
        var submitter = defaultSubmitter(form);

        // 先提交字段的 change/blur，再让原表单校验和 submit 监听器接管。
        finishTarget(false);
        if (!form) return;

        if (typeof form.requestSubmit === "function") {
            if (submitter) form.requestSubmit(submitter);
            else form.requestSubmit();
            return;
        }
        // 兼容旧版板端 Chromium，同时保持校验及 submitter 的点击语义。
        if (submitter && typeof submitter.click === "function") {
            submitter.click();
            return;
        }
        var fallbackSubmitter = document.createElement("button");
        fallbackSubmitter.type = "submit";
        fallbackSubmitter.hidden = true;
        form.appendChild(fallbackSubmitter);
        fallbackSubmitter.click();
        form.removeChild(fallbackSubmitter);
    }

    function key(label, value, className, action) {
        var disabled = String(className || "").split(/\s+/).indexOf("is-disabled") >= 0;
        return '<button type="button" class="virtual-keyboard-key ' + (className || "") +
            '" data-key-value="' + escapeAttribute(value || "") + '" data-key-action="' +
            escapeAttribute(action || "insert") + '"' + (disabled ? " disabled" : "") + ">" + label + "</button>";
    }

    function escapeAttribute(value) {
        return String(value).replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;");
    }

    function row(items, className) {
        return '<div class="virtual-keyboard-row ' + (className || "") + '">' + items.join("") + "</div>";
    }

    function completionKey() {
        return key(formForTarget(activeTarget) ? "提交" : "完成", "", "key-wide key-complete", "complete");
    }

    function englishLayout(passwordMode) {
        var letters = uppercase ? "QWERTYUIOPASDFGHJKLZXCVBNM" : "qwertyuiopasdfghjklzxcvbnm";
        function letterKeys(start, end) {
            return letters.slice(start, end).split("").map(function (letter) {
                return key(letter, letter);
            });
        }
        var title = passwordMode ? "密码键盘" : "英文键盘";
        var languageKey = activeConfig && activeConfig.chinese ?
            key("中文", "", "key-wide key-function", "language") : "";
        return '<div class="virtual-keyboard-heading"><span>' + title + '</span><small>内容直接输入到当前字段</small></div>' +
            row("1234567890".split("").map(function (digit) { return key(digit, digit); }).concat([
                key("清空", "", "key-wide key-function", "clear"),
                key("⌫", "", "key-wide key-function", "backspace")
            ])) +
            row(letterKeys(0, 10)) +
            row(letterKeys(10, 19).concat([key("@", "@")])) +
            row([
                key(uppercase ? "小写" : "大写", "", "key-wide" + (uppercase ? " is-active" : ""), "shift"),
                languageKey
            ].concat(letterKeys(19, 26)).concat([
                key(".", "."), key("-", "-"), key("_", "_")
            ])) +
            row([
                key("/", "/"), key(":", ":"), key(",", ","),
                key("空格", " ", "key-space"),
                key("←", "", "key-wide", "left"),
                key("→", "", "key-wide", "right"),
                completionKey()
            ]);
    }

    function commonWords() {
        var source = window.EdgeKeyboardCommonWords || {};
        var seen = Object.create(null);
        var recents = recentWords();
        function uniqueTexts(entries) {
            return (Array.isArray(entries) ? entries : []).map(function (entry) {
                return typeof entry === "string" ? entry : String(entry && entry.text || "");
            }).filter(function (word) {
                if (!word || seen[word]) return false;
                seen[word] = true;
                return true;
            });
        }
        recents.forEach(function (word) { seen[word] = true; });
        return {
            recents: recents,
            deviceTypes: uniqueTexts(source.deviceTypes),
            quantities: uniqueTexts(source.quantities)
        };
    }

    function recentWords() {
        var parsed;
        try {
            parsed = JSON.parse(window.localStorage.getItem(RECENT_WORDS_KEY) || "[]");
        } catch (_) {
            return [];
        }
        if (!Array.isArray(parsed)) return [];
        return parsed.filter(function (word, index) {
            return typeof word === "string" && word.length > 0 && parsed.indexOf(word) === index;
        }).slice(0, RECENT_WORDS_LIMIT);
    }

    function rememberWord(word) {
        var value = String(word || "");
        if (!value || !activeConfig || !activeConfig.chinese || activeConfig.type !== "text") return;
        var words = recentWords().filter(function (item) { return item !== value; });
        words.unshift(value);
        try {
            window.localStorage.setItem(RECENT_WORDS_KEY, JSON.stringify(words.slice(0, RECENT_WORDS_LIMIT)));
        } catch (_) {
            // 本地存储不可用时仅跳过最近使用，不影响当前输入。
        }
    }

    function candidateKey(word, source) {
        var sourceClass = String(source || "").replace(/[^a-z-]/g, "");
        return key(
            escapeText(word),
            word,
            "virtual-keyboard-candidate" + (sourceClass ? " candidate-source-" + sourceClass : ""),
            "candidate"
        );
    }

    function commonCandidateItems(source) {
        return source.recents.map(function (word) {
            return { text: word, source: "recent" };
        }).concat(source.deviceTypes.map(function (word) {
            return { text: word, source: "device" };
        }), source.quantities.map(function (word) {
            return { text: word, source: "quantity" };
        }));
    }

    function escapeText(value) {
        return String(value).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    function chineseLayout() {
        var source = commonWords();
        var composition = String(pinyinComposition || "");
        var candidates = commonCandidateItems(source);
        if (composition && window.EdgeOfflinePinyin) {
            candidates = window.EdgeOfflinePinyin.search(composition, recentWords());
        }

        var pageSize = 8;
        var pageCount = Math.max(1, Math.ceil(candidates.length / pageSize));
        candidatePage = Math.max(0, Math.min(candidatePage, pageCount - 1));
        var pageItems = candidates.slice(candidatePage * pageSize, (candidatePage + 1) * pageSize);
        var candidateStrip = '<div class="virtual-keyboard-candidate-strip">' +
            '<div class="virtual-keyboard-pinyin-composition"><strong>拼音</strong><span>' +
            (composition ? escapeText(composition) : "常用词") + "</span></div>" +
            '<div class="virtual-keyboard-pinyin-candidate-list">' +
            (pageItems.length ? pageItems.map(function (item) {
                return candidateKey(item.text, item.source);
            }).join("") : '<span class="virtual-keyboard-pinyin-empty">暂无匹配，请按 ABC 输入英文</span>') +
            "</div>" +
            '<span class="virtual-keyboard-page-state">' +
            (candidates.length ? (candidatePage + 1) + "/" + pageCount : "0/0") + "</span>" +
            key("‹", "-1", "key-function key-page" + (candidatePage <= 0 ? " is-disabled" : ""), "candidate-page") +
            key("›", "1", "key-function key-page" + (candidatePage + 1 >= pageCount ? " is-disabled" : ""), "candidate-page") +
            "</div>";

        function pinyinKeys(letters) {
            return letters.split("").map(function (letter) {
                return key(letter, letter, "", "pinyin");
            });
        }

        return candidateStrip +
            row("1234567890".split("").map(function (digit) {
                return key(digit, digit, "key-number");
            }), "virtual-keyboard-number-row") +
            row(pinyinKeys("qwertyuiop"), "virtual-keyboard-letter-row") +
            row(pinyinKeys("asdfghjkl"), "virtual-keyboard-letter-row virtual-keyboard-letter-row-home") +
            row([
                key("ABC", "", "key-wide key-function", "language")
            ].concat(pinyinKeys("zxcvbnm")).concat([
                key("⌫", "", "key-wide key-function key-backspace", "backspace")
            ]), "virtual-keyboard-letter-row virtual-keyboard-letter-row-bottom") +
            row([
                key("清空", "", "key-wide key-function", "clear"),
                key("@", "@"), key(".", "."), key(",", ","),
                key("-", "-"), key("_", "_"), key("/", "/"), key(":", ":"),
                key("空格", " ", "key-space", "space"),
                key("←", "", "key-wide key-function", "left"),
                key("→", "", "key-wide key-function", "right"),
                completionKey()
            ], "virtual-keyboard-function-row");
    }

    function numberLayout(config) {
        var finalRow = [key("0", "0", "key-double")];
        if (config.decimal) finalRow.push(key(".", "."));
        if (config.negative) finalRow.push(key("−", "", "", "minus"));
        finalRow.push(
            key("←", "", "key-wide key-function", "left"),
            key("→", "", "key-wide key-function", "right"),
            key("清空", "", "key-wide key-function", "clear"),
            key("⌫", "", "key-wide key-function", "backspace"),
            completionKey()
        );
        return '<div class="virtual-keyboard-heading"><span>' +
            (config.decimal ? "数字键盘" : "整数键盘") +
            '</span><small>保留原字段范围与步进校验</small></div><div class="virtual-keyboard-compact">' +
            row(["7", "8", "9"].map(function (digit) { return key(digit, digit); })) +
            row(["4", "5", "6"].map(function (digit) { return key(digit, digit); })) +
            row(["1", "2", "3"].map(function (digit) { return key(digit, digit); })) +
            row(finalRow) + "</div>";
    }

    function ipLayout() {
        return '<div class="virtual-keyboard-heading"><span>IP 地址键盘</span><small>格式由原表单继续校验</small></div><div class="virtual-keyboard-compact">' +
            row(["7", "8", "9"].map(function (digit) { return key(digit, digit); })) +
            row(["4", "5", "6"].map(function (digit) { return key(digit, digit); })) +
            row(["1", "2", "3"].map(function (digit) { return key(digit, digit); })) +
            row([
                key("0", "0", "key-double"),
                key(".", "."),
                key("←", "", "key-wide key-function", "left"),
                key("→", "", "key-wide key-function", "right"),
                key("清空", "", "key-wide key-function", "clear"),
                key("⌫", "", "key-wide key-function", "backspace"),
                completionKey()
            ]) + "</div>";
    }

    function render() {
        if (!keyboardRoot || !activeConfig) return;
        var content = keyboardRoot.querySelector("[data-virtual-keyboard-content]");
        keyboardRoot.classList.toggle("is-chinese-mode", chineseMode);
        if (activeConfig.type === "number") {
            content.innerHTML = numberLayout(activeConfig);
        } else if (activeConfig.type === "ip") {
            content.innerHTML = ipLayout();
        } else if (activeConfig.type === "text" && activeConfig.chinese && chineseMode) {
            content.innerHTML = chineseLayout();
        } else {
            content.innerHTML = englishLayout(activeConfig.type === "password");
        }
        syncKeyboardGeometry();
    }

    function syncKeyboardGeometry() {
        if (!keyboardRoot || keyboardRoot.hidden) return;
        window.requestAnimationFrame(function () {
            if (!keyboardRoot || keyboardRoot.hidden) return;
            document.documentElement.style.setProperty("--virtual-keyboard-height", keyboardRoot.offsetHeight + "px");
            dispatchKeyboardLayoutEvent(true);
            ensureVisible();
        });
    }

    function createKeyboard() {
        if (keyboardRoot) return keyboardRoot;
        keyboardRoot = document.createElement("section");
        keyboardRoot.className = "virtual-keyboard";
        keyboardRoot.setAttribute("aria-label", "屏幕虚拟键盘");
        keyboardRoot.setAttribute("aria-hidden", "true");
        keyboardRoot.hidden = true;
        keyboardRoot.innerHTML = '<div class="virtual-keyboard-panel" data-virtual-keyboard-content></div>';
        document.body.appendChild(keyboardRoot);

        keyboardRoot.addEventListener("mousedown", function (event) {
            var button = event.target.closest ? event.target.closest("[data-key-action]") : null;
            if (!button) return;
            event.preventDefault();
            setPressedKey(button);
        });
        keyboardRoot.addEventListener("mouseleave", clearPressedKey);
        keyboardRoot.addEventListener("touchstart", function (event) {
            var button = event.target.closest ? event.target.closest("[data-key-action]") : null;
            if (!button) return;
            lastTouchKeyTime = Date.now();
            touchKey = button;
            touchMoved = false;
            if (event.touches && event.touches[0]) {
                touchStartX = event.touches[0].clientX;
                touchStartY = event.touches[0].clientY;
            }
            setPressedKey(button);
        }, false);
        keyboardRoot.addEventListener("touchmove", function (event) {
            if (!touchKey || !event.touches || !event.touches[0]) return;
            var deltaX = Math.abs(event.touches[0].clientX - touchStartX);
            var deltaY = Math.abs(event.touches[0].clientY - touchStartY);
            if (deltaX > 8 || deltaY > 8) {
                touchMoved = true;
                clearPressedKey();
            }
        }, false);
        keyboardRoot.addEventListener("touchend", function (event) {
            var button = touchKey;
            var shouldActivate = Boolean(button) && !touchMoved;
            touchKey = null;
            touchMoved = false;
            clearPressedKey();
            lastTouchKeyTime = Date.now();
            if (!shouldActivate) return;
            event.preventDefault();
            handleKey(button);
        }, false);
        keyboardRoot.addEventListener("touchcancel", function () {
            touchKey = null;
            touchMoved = false;
            clearPressedKey();
        }, false);
        keyboardRoot.addEventListener("click", function (event) {
            var button = event.target.closest ? event.target.closest("[data-key-action]") : null;
            if (!button) return;
            event.preventDefault();
            // language/candidate 等动作会重绘按键；阻止已移除的按钮继续冒泡后被当作键盘外点击。
            event.stopPropagation();
            if (Date.now() - lastTouchKeyTime < 600) return;
            handleKey(button);
        });
        document.addEventListener("mouseup", clearPressedKey);
        return keyboardRoot;
    }

    function setPressedKey(button) {
        if (pressedKey === button) return;
        clearPressedKey();
        pressedKey = button;
        pressedKey.classList.add("is-pressed");
    }

    function clearPressedKey() {
        if (!pressedKey) return;
        pressedKey.classList.remove("is-pressed");
        pressedKey = null;
    }

    function handleKey(button) {
        if (!activeTarget) return;
        var action = button.getAttribute("data-key-action") || "insert";
        var value = button.getAttribute("data-key-value") || "";
        if (action === "insert") insertText(value);
        else if (action === "backspace") backspace();
        else if (action === "clear") clearValue();
        else if (action === "left") moveCaret(-1);
        else if (action === "right") moveCaret(1);
        else if (action === "minus") toggleMinus();
        else if (action === "pinyin") {
            if (pinyinComposition.length < 48) {
                pinyinComposition += String(value || "").toLowerCase();
                candidatePage = 0;
                render();
            }
        }
        else if (action === "space") {
            if (chineseMode && pinyinComposition) {
                commitPinyinComposition();
                render();
            } else {
                insertText(" ");
            }
        }
        else if (action === "candidate") {
            insertText(value);
            rememberWord(value);
            pinyinComposition = "";
            candidatePage = 0;
            render();
        }
        else if (action === "candidate-page") {
            var direction = Number(value) || 0;
            var matches = pinyinComposition && window.EdgeOfflinePinyin ?
                window.EdgeOfflinePinyin.search(pinyinComposition, recentWords()) :
                commonCandidateItems(commonWords());
            var pages = Math.max(1, Math.ceil(matches.length / 8));
            candidatePage = Math.max(0, Math.min(candidatePage + direction, pages - 1));
            render();
        }
        else if (action === "language") {
            commitPinyinComposition();
            chineseMode = Boolean(activeConfig.chinese) && !chineseMode;
            uppercase = false;
            candidatePage = 0;
            render();
        }
        else if (action === "shift") {
            var keepPressed = button.classList.contains("is-pressed");
            uppercase = !uppercase;
            render();
            if (keepPressed) {
                setPressedKey(keyboardRoot.querySelector('[data-key-action="shift"]'));
            }
        }
        else if (action === "complete") {
            completeTarget();
        }
        if (activeTarget) activeTarget.focus();
    }

    function scrollableAncestor(target) {
        var node = target.parentElement;
        while (node && node !== document.body) {
            var style = window.getComputedStyle ? window.getComputedStyle(node) : null;
            var overflow = style ? style.overflowY : "";
            if ((overflow === "auto" || overflow === "scroll") && node.scrollHeight > node.clientHeight) return node;
            node = node.parentElement;
        }
        return null;
    }

    function ensureVisible() {
        if (!activeTarget || !keyboardRoot || keyboardRoot.hidden) return;
        var keyboardTop = keyboardRoot.getBoundingClientRect().top;
        var rect = activeTarget.getBoundingClientRect();
        var visibleTop = rect.top;
        var visibleBottom = rect.bottom;
        var activeForm = formForTarget(activeTarget);
        var loginSubmitter = document.body.classList.contains("login-body") &&
            activeForm && activeForm.classList.contains("login-form") ? defaultSubmitter(activeForm) : null;
        if (loginSubmitter) {
            var submitterRect = loginSubmitter.getBoundingClientRect();
            visibleTop = Math.min(visibleTop, submitterRect.top);
            visibleBottom = Math.max(visibleBottom, submitterRect.bottom);
        }
        var topLimit = 12;
        var bottomLimit = keyboardTop - 16;
        var delta = 0;
        if (visibleBottom > bottomLimit) delta = visibleBottom - bottomLimit;
        else if (visibleTop < topLimit) delta = visibleTop - topLimit;
        if (!delta) return;

        var scroller = scrollableAncestor(activeTarget);
        if (scroller) {
            scroller.scrollTop += delta;
        } else {
            window.scrollBy(0, delta);
        }
    }

    function activate(target, config) {
        if (!enabled || !config) return;
        if (activeTarget === target) {
            ensureVisible();
            return;
        }
        if (activeTarget) finishTarget(true);

        activeTarget = target;
        activeConfig = config;
        dirty = false;
        uppercase = false;
        chineseMode = false;
        pinyinComposition = "";
        candidatePage = 0;
        temporaryNumberInput = target.getAttribute("type") === "number";
        if (temporaryNumberInput) target.setAttribute("type", "text");

        createKeyboard();
        render();
        if (window.EdgeMotion) window.EdgeMotion.cancelExit(keyboardRoot);
        keyboardRoot.hidden = false;
        keyboardRoot.setAttribute("aria-hidden", "false");
        document.body.classList.add("virtual-keyboard-open");
        window.requestAnimationFrame(function () {
            if (!keyboardRoot || keyboardRoot.hidden) return;
            document.documentElement.style.setProperty("--virtual-keyboard-height", keyboardRoot.offsetHeight + "px");
            dispatchKeyboardLayoutEvent(true);
            ensureVisible();
            window.setTimeout(ensureVisible, 160);
        });
    }

    function reevaluate() {
        enabled = shouldEnable();
        document.body.classList.toggle("virtual-keyboard-enabled", enabled);
        if (!enabled && activeTarget) hide();
    }

    function initialize() {
        createKeyboard();
        reevaluate();
        var focusedConfig = targetConfig(document.activeElement);
        if (focusedConfig) activate(document.activeElement, focusedConfig);
    }

    function markPointerTarget(event) {
        var config = targetConfig(event.target);
        if (!config) return;
        pendingPointerTarget = event.target;
        if (pendingPointerTimer) window.clearTimeout(pendingPointerTimer);
        pendingPointerTimer = window.setTimeout(function () {
            var target = pendingPointerTarget;
            pendingPointerTarget = null;
            pendingPointerTimer = 0;
            if (target && document.activeElement === target) activate(target, targetConfig(target));
        }, 700);
    }

    function clearPointerTarget() {
        pendingPointerTarget = null;
        if (pendingPointerTimer) window.clearTimeout(pendingPointerTimer);
        pendingPointerTimer = 0;
    }

    document.addEventListener("mousedown", markPointerTarget, true);
    document.addEventListener("touchstart", markPointerTarget, true);

    document.addEventListener("focusin", function (event) {
        var target = event.target;
        var config = targetConfig(event.target);
        if (!config) return;
        // 指针操作统一在 click 完成后显示，避免页面避让改变同一次点击的抬起落点。
        if (pendingPointerTarget === target) return;
        window.setTimeout(function () {
            if (document.activeElement === target) activate(target, config);
        }, 0);
    });

    document.addEventListener("focusout", function (event) {
        var target = event.target;
        if (target !== activeTarget) return;
        window.setTimeout(function () {
            if (!activeTarget || activeTarget !== target) return;
            var nextTarget = document.activeElement;
            if (keyboardRoot && keyboardRoot.contains(nextTarget)) {
                target.focus();
                return;
            }
            if (targetConfig(nextTarget)) return;
            hide();
        }, 0);
    });

    document.addEventListener("click", function (event) {
        if (!keyboardRoot || keyboardRoot.contains(event.target)) return;
        var config = targetConfig(event.target);
        if (config) {
            clearPointerTarget();
            if (document.activeElement === event.target) activate(event.target, config);
            return;
        }
        clearPointerTarget();
        if (!activeTarget) return;
        hide();
    });

    document.addEventListener("keydown", function (event) {
        if (!activeTarget) return;
        if (event.key === "Escape") {
            event.preventDefault();
            hide();
            return;
        }
        if (activeConfig.type !== "number" || event.ctrlKey || event.altKey || event.metaKey || event.key.length !== 1) return;
        event.preventDefault();
        if (/^\d$/.test(event.key)) insertText(event.key);
        else if (event.key === "." && activeConfig.decimal) insertText(event.key);
        else if (event.key === "-" && activeConfig.negative) toggleMinus();
    }, true);

    window.addEventListener("resize", function () {
        reevaluate();
        if (activeTarget) window.setTimeout(ensureVisible, 0);
    });
    window.addEventListener("pagehide", function () {
        if (activeTarget) hide();
    });

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", initialize);
    } else {
        initialize();
    }

    window.EdgeVirtualKeyboard = {
        hide: hide,
        isEnabled: function () { return enabled; },
        refresh: reevaluate
    };
}());
