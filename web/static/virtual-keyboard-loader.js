// 板端虚拟键盘按需入口。桌面浏览和未编辑表单的页面不解析词库及键盘实现。
(function () {
    "use strict";

    var STORAGE_KEY = "edge.virtualKeyboard.override";
    var sourceScript = document.currentScript;
    var assetVersion = sourceScript ? String(sourceScript.getAttribute("data-asset-version") || "") : "";
    var stylePath = "/static/virtual-keyboard.css";
    var assetPaths = [
        "/static/virtual-keyboard-words.js",
        "/static/virtual-keyboard-pinyin-data.js",
        "/static/virtual-keyboard-pinyin-hsk-data.js",
        "/static/virtual-keyboard-pinyin.js",
        "/static/virtual-keyboard.js"
    ];
    var loading = null;
    var loaded = false;

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
            } catch (_error) {
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
            try { window.sessionStorage.setItem(STORAGE_KEY, value); } catch (_error) { /* optional */ }
            return value;
        }
        try { value = window.sessionStorage.getItem(STORAGE_KEY) || ""; } catch (_error) { value = ""; }
        return value === "0" || value === "1" ? value : "";
    }

    function shouldLoad() {
        var override = currentOverride();
        if (override === "1") return true;
        if (override === "0") return false;
        var width = window.innerWidth || document.documentElement.clientWidth || 0;
        var height = window.innerHeight || document.documentElement.clientHeight || 0;
        var touch = (Number(window.navigator.maxTouchPoints) || 0) > 0 ||
            (Number(window.navigator.msMaxTouchPoints) || 0) > 0 ||
            "ontouchstart" in window;
        return touch && width > 0 && height > 0 && width <= 1280 && height <= 800;
    }

    function isEditableTarget(target) {
        if (!target || target.nodeType !== 1 || target.disabled || target.readOnly) return false;
        var tag = String(target.tagName || "").toLowerCase();
        if (tag !== "input" && tag !== "textarea") return false;
        var explicit = String(target.getAttribute("data-keyboard") || "").toLowerCase();
        if (explicit === "none") return false;
        // 显式键盘类型优先于原生 input type。这样选择器的 search 输入可以按需加载
        // 文本键盘，同时不会让未声明 data-keyboard 的其它搜索框自动弹出键盘。
        if (["text", "text-cn", "integer", "decimal", "ip", "password"].indexOf(explicit) >= 0) {
            return true;
        }
        var type = String(target.getAttribute("type") || "text").toLowerCase();
        return [
            "hidden", "checkbox", "radio", "search", "date", "datetime-local", "time", "month", "week",
            "file", "color", "range", "button", "submit", "reset"
        ].indexOf(type) < 0;
    }

    function versionedURL(path) {
        return assetVersion ? path + "?v=" + encodeURIComponent(assetVersion) : path;
    }

    function loadScript(path) {
        return new Promise(function (resolve, reject) {
            var script = document.createElement("script");
            script.src = versionedURL(path);
            script.async = false;
            script.onload = resolve;
            script.onerror = function () {
                if (script.parentNode) script.parentNode.removeChild(script);
                reject(new Error("virtual keyboard asset failed: " + path));
            };
            (document.head || document.documentElement).appendChild(script);
        });
    }

    function loadStyle(path) {
        return new Promise(function (resolve, reject) {
            var existing = document.querySelector('link[data-virtual-keyboard-style="true"]');
            if (existing) {
                if (existing.getAttribute("data-loaded") === "true") resolve();
                else {
                    var handleLoad = function () {
                        existing.removeEventListener("load", handleLoad);
                        existing.removeEventListener("error", handleError);
                        resolve();
                    };
                    var handleError = function () {
                        existing.removeEventListener("load", handleLoad);
                        existing.removeEventListener("error", handleError);
                        reject(new Error("virtual keyboard style failed: " + path));
                    };
                    existing.addEventListener("load", handleLoad);
                    existing.addEventListener("error", handleError);
                }
                return;
            }
            var link = document.createElement("link");
            link.rel = "stylesheet";
            link.href = versionedURL(path);
            link.setAttribute("data-virtual-keyboard-style", "true");
            link.onload = function () {
                link.setAttribute("data-loaded", "true");
                resolve();
            };
            link.onerror = function () {
                if (link.parentNode) link.parentNode.removeChild(link);
                reject(new Error("virtual keyboard style failed: " + path));
            };
            (document.head || document.documentElement).appendChild(link);
        });
    }

    // 保持原有依赖顺序；失败时保留浏览器原生输入，并允许下一次聚焦重试。
    function loadKeyboard() {
        if (loaded || window.EdgeVirtualKeyboard) return Promise.resolve(window.EdgeVirtualKeyboard);
        if (loading) return loading;
        loading = loadStyle(stylePath);
        assetPaths.forEach(function (path) {
            loading = loading.then(function () { return loadScript(path); });
        });
        loading = loading.then(function () {
            loaded = true;
            removeTriggers();
            return window.EdgeVirtualKeyboard;
        }, function () {
            loading = null;
            return null;
        });
        return loading;
    }

    function trigger(event) {
        if (!isEditableTarget(event.target) || !shouldLoad()) return;
        loadKeyboard();
    }

    function removeTriggers() {
        document.removeEventListener("mousedown", trigger, true);
        document.removeEventListener("touchstart", trigger, true);
        document.removeEventListener("focusin", trigger, true);
    }

    document.addEventListener("mousedown", trigger, true);
    document.addEventListener("touchstart", trigger, true);
    document.addEventListener("focusin", trigger, true);

    window.EdgeVirtualKeyboardLoader = {
        load: loadKeyboard,
        isLoaded: function () { return loaded || Boolean(window.EdgeVirtualKeyboard); }
    };
}());
