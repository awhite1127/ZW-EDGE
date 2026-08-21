// 历史数据页交互入口：维护通道树展开状态和内容区当前设备选择。
// 图表与记录查询由服务端页面数据负责，本文件不在浏览器端重新聚合历史样本。
(function () {
    "use strict";

    const EdgeApp = window.EdgeApp;
    if (!EdgeApp) return;

    const EdgeMotion = window.EdgeMotion;
    const readTreeState = EdgeApp.readTreeState;
    const writeTreeState = EdgeApp.writeTreeState;

    function mount() {
        initHistoryDeviceTree();
        initHistoryDeviceSelector();
        initHistoryPointSelector();
    }


    // 树节点展开状态仅作为浏览器偏好保存，不影响后端设备或采集配置。
    function initHistoryDeviceTree() {
        const tree = document.querySelector(".history-filter-tree");
        if (!tree) return;
        const storageKey = "edge.history.device-tree.v1";
        const state = readTreeState(storageKey);
        const currentTreeKeys = Object.create(null);
        tree.querySelectorAll("[data-history-tree-branch]").forEach(function (branch) {
            const key = branch.dataset.treeKey || "";
            currentTreeKeys[key] = true;
            const containsActive = Boolean(branch.querySelector(".tree-filter-node.active"));
            const expanded = containsActive ||
                (typeof state[key] === "boolean" ? state[key] : true);
            const collapsed = !expanded;
            branch.classList.toggle("is-collapsed", collapsed);
            const toggle = branch.querySelector(":scope > .tree-branch-row [data-history-tree-toggle]");
            if (toggle) toggle.setAttribute("aria-expanded", collapsed ? "false" : "true");
        });
        let treeStateChanged = false;
        Object.keys(state).forEach(function (key) {
            if (currentTreeKeys[key]) return;
            delete state[key];
            treeStateChanged = true;
        });
        if (treeStateChanged) writeTreeState(storageKey, state);
        tree.addEventListener("click", function (event) {
            const toggle = event.target.closest("[data-history-tree-toggle]");
            if (!toggle) return;
            event.preventDefault();
            event.stopPropagation();
            const branch = toggle.closest("[data-history-tree-branch]");
            if (!branch) return;
            const collapsed = branch.classList.toggle("is-collapsed");
            toggle.setAttribute("aria-expanded", collapsed ? "false" : "true");
            state[branch.dataset.treeKey || ""] = !collapsed;
            writeTreeState(storageKey, state);
            if (!collapsed && EdgeMotion) {
                const children = branch.querySelector(".tree-branch-children");
                if (children) EdgeMotion.reveal(children);
            }
        });
    }

    // 初始化历史设备选择器。
    function initHistoryDeviceSelector() {
        const selector = document.querySelector("[data-history-device-selector]");
        if (!selector) return;
        selector.addEventListener("change", function () {
            const target = selector.value || "";
            if (target.indexOf("/history?") !== 0 && target !== "/history") return;
            if (EdgeApp.navigate) EdgeApp.navigate(target);
            else window.location.assign(target);
        });
    }

    // 设备详情的数据项切换沿用软导航，只替换同一代统计、图表和明细。
    function initHistoryPointSelector() {
        const selector = document.querySelector("[data-history-point-selector]");
        const form = selector ? selector.form : null;
        if (!selector || !form) return;
        selector.addEventListener("change", function () {
            const target = new URL(form.action || "/device-history", window.location.href);
            Array.from(form.elements).forEach(function (field) {
                if (!field.name || field.disabled) return;
                target.searchParams.set(field.name, field.value || "");
            });
            if (EdgeApp.navigate) EdgeApp.navigate(target.href);
            else window.location.assign(target.href);
        });
    }

    EdgeApp.registerPageController("history", ["history"], { mount: mount });
})();
