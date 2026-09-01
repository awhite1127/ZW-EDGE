// 运维管理页角色说明与权限预览；这里只展示后端定义的角色边界，不执行授权判定。
(function () {
    "use strict";

    const confirmAction = window.EdgeApp && window.EdgeApp.confirmAction;
    let pageScope = null;

    const roleSummaries = {
        super_admin: {
            name: "超级管理员",
            description: "系统内置最高权限角色，拥有全部配置、运维和用户管理权限。",
            allow: ["用户管理", "系统设置", "运维管理", "采集配置", "告警规则", "设备操作", "恢复出厂"],
            deny: ["不支持通过新建用户创建", "不支持在此页面由普通用户提升"]
        },
        engineer: {
            name: "工程师",
            description: "维护采集配置、设备类型、告警规则和现场诊断。",
            allow: ["采集配置", "设备类型", "设备命名", "告警规则", "设备操作", "确认告警", "导出诊断包"],
            deny: ["不允许用户管理", "不允许系统设置", "不允许恢复出厂"]
        },
        operator: {
            name: "操作员",
            description: "现场值守常用角色，可查看运行数据、确认告警并导出诊断包。",
            allow: ["查看概览", "查看实时", "查看历史", "查看事件", "确认告警", "导出诊断包"],
            deny: ["不允许采集配置", "不允许系统设置", "不允许设备类型", "不允许告警规则", "不允许恢复出厂"]
        },
        viewer: {
            name: "只读用户",
            description: "仅用于查看运行页面，不允许任何修改操作。",
            allow: ["查看概览", "查看实时", "查看历史", "查看事件"],
            deny: ["不允许确认告警", "不允许任何修改操作"]
        }
    };

    function createTag(text, className) {
        const item = document.createElement("span");
        item.className = className;
        item.textContent = text;
        return item;
    }

    // 角色卡片仅用于解释允许/禁止范围，真正的页面和接口权限由服务端中间件控制。
    function renderRolePreview(container, role) {
        const summary = roleSummaries[role] || roleSummaries.operator;
        container.replaceChildren();

        const head = document.createElement("div");
        head.className = "role-preview-head";

        const title = document.createElement("strong");
        title.textContent = summary.name;
        const description = document.createElement("p");
        description.textContent = summary.description;
        head.append(title, description);

        const body = document.createElement("div");
        body.className = "role-preview-body";

        const allowGroup = document.createElement("div");
        allowGroup.className = "role-preview-group";
        const allowTitle = document.createElement("span");
        allowTitle.className = "role-preview-label status-ok";
        allowTitle.textContent = "允许";
        const allowTags = document.createElement("div");
        allowTags.className = "role-preview-tags";
        summary.allow.forEach(function (item) {
            allowTags.appendChild(createTag(item, "role-preview-tag role-preview-tag-allow"));
        });
        allowGroup.append(allowTitle, allowTags);

        const denyGroup = document.createElement("div");
        denyGroup.className = "role-preview-group";
        const denyTitle = document.createElement("span");
        denyTitle.className = "role-preview-label muted-text";
        denyTitle.textContent = "关键限制";
        const denyTags = document.createElement("div");
        denyTags.className = "role-preview-tags";
        summary.deny.forEach(function (item) {
            denyTags.appendChild(createTag(item, "role-preview-tag role-preview-tag-deny"));
        });
        denyGroup.append(denyTitle, denyTags);

        body.append(allowGroup, denyGroup);
        container.append(head, body);
    }

    function initRolePreviews() {
        document.querySelectorAll("[data-role-preview]").forEach(function (container) {
            const form = container.closest("form");
            const select = form ? form.querySelector("[data-role-preview-select]") : null;
            const fixedRole = container.dataset.rolePreviewRole || "";
            const role = fixedRole || (select ? select.value : "operator");
            renderRolePreview(container, role);
            if (select) {
                pageScope.listen(select, "change", function () {
                    renderRolePreview(container, select.value);
                });
            }
        });
    }

    // 数据清理不可逆；运维入口页不会加载 history.js，因此在本页单独绑定确认。
    function initHistoryCleanupConfirmation() {
        document.querySelectorAll("[data-history-cleanup-form]").forEach(function (form) {
            pageScope.listen(form, "submit", async function (event) {
                event.preventDefault();
                const confirmed = await confirmAction({
                    title: "确认清理超期数据",
                    message: "仅删除超过保存期限的数据，不影响设备、采集配置与当前实时数据。确定继续吗？",
                    confirmText: "确认清理",
                    danger: true
                });
                if (confirmed) form.submit();
            });
        });
    }

    const updateStateText = {
        IDLE: "等待上传",
        UPLOADING: "正在上传",
        VALIDATING: "正在校验",
        READY: "校验通过",
        BACKING_UP: "正在备份当前版本",
        PREPARING: "正在准备",
        STOPPING: "正在停止服务",
        INSTALLING: "正在安装",
        STARTING: "正在启动服务",
        VERIFYING: "正在验证",
        SUCCESS: "升级成功",
        ROLLING_BACK: "正在自动恢复",
        ROLLED_BACK: "已恢复原版本",
        FAILED: "升级失败"
    };
    const activeUpdateStates = new Set([
        "VALIDATING", "BACKING_UP", "PREPARING", "STOPPING", "INSTALLING", "STARTING", "VERIFYING", "ROLLING_BACK"
    ]);
    const trackableUpdateStates = new Set(["READY"].concat(Array.from(activeUpdateStates)));
    const stageWidths = {
        IDLE: "0%", UPLOADING: "12%", VALIDATING: "22%", READY: "28%",
        BACKING_UP: "36%", PREPARING: "36%", STOPPING: "45%", INSTALLING: "62%", STARTING: "76%",
        VERIFYING: "88%", SUCCESS: "100%", ROLLING_BACK: "86%", ROLLED_BACK: "100%", FAILED: "100%"
    };

    function formatUpdateSize(value) {
        const bytes = Number(value) || 0;
        if (bytes <= 0) return "-";
        if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(bytes >= 10 * 1024 * 1024 ? 0 : 1) + " MiB";
        return Math.max(1, Math.ceil(bytes / 1024)) + " KiB";
    }

    function formatUpdateTime(value) {
        const text = String(value || "").trim();
        if (!text) return "-";
        const date = new Date(text);
        return Number.isNaN(date.getTime()) ? text : date.toLocaleString("zh-CN", { hour12: false });
    }

    // 产品版本在文件和 API 中保持纯 SemVer，仅页面展示时添加 V 前缀。
    function displayProductVersion(value) {
        const normalized = String(value || "").trim().replace(/^[vV]/, "");
        return normalized ? "V" + normalized : "";
    }

    function closeUpdateModal(modal) {
        if (!modal) return;
        modal.classList.remove("open");
        modal.setAttribute("aria-hidden", "true");
        if (!document.querySelector(".channel-modal.open")) document.body.classList.remove("modal-open");
    }

    async function readUpdateResponse(response, fallback) {
        if (window.EdgeApp && typeof window.EdgeApp.readApiResponse === "function") {
            return window.EdgeApp.readApiResponse(response, fallback);
        }
        try {
            const payload = await response.json();
            if (response.ok && payload && payload.success) return { ok: true, data: payload.data };
            return { ok: false, status: response.status, message: payload && payload.error && payload.error.message || fallback };
        } catch (_error) {
            return { ok: false, status: response.status, message: fallback };
        }
    }

    function initApplicationUpdate(scope, root) {
        if (!root || !scope) return null;

        const isPublicWatch = root.dataset.publicWatch === "true";
        const updateModal = isPublicWatch ? null : root.closest(".application-update-modal");
        const nodes = {
            badge: root.querySelector("[data-update-state]"),
            state: root.querySelector("[data-update-state-text]"),
            current: root.querySelector("[data-update-current-version]"),
            package: root.querySelector("[data-update-package]"),
            product: root.querySelector("[data-update-product]"),
            target: root.querySelector("[data-update-target-version]"),
            platform: root.querySelector("[data-update-platform]"),
            arch: root.querySelector("[data-update-arch]"),
            size: root.querySelector("[data-update-package-size]"),
            buildTime: root.querySelector("[data-update-build-time]"),
            progress: root.querySelector("[data-update-progress]"),
            progressBar: root.querySelector("[data-update-progress-bar]"),
            progressTitle: root.querySelector("[data-update-progress-title]"),
            progressMessage: root.querySelector("[data-update-progress-message]"),
            result: root.querySelector("[data-update-result]"),
            start: root.querySelector("[data-update-start]"),
            reload: root.querySelector("[data-update-reload]"),
            form: root.querySelector("[data-update-upload-form]"),
            file: root.querySelector("[data-update-file]"),
            fileName: root.querySelector("[data-update-file-name]"),
            fileSize: root.querySelector("[data-update-file-size]"),
            upload: root.querySelector("[data-update-upload]"),
            selectionStage: root.querySelector("[data-update-selection-stage]"),
            packageStage: root.querySelector("[data-update-package-stage]"),
            packageState: root.querySelector("[data-update-package-state]"),
            reselect: root.querySelector("[data-update-reselect]")
        };
        const modal = document.getElementById("application-update-confirm-modal");
        const confirmButton = modal && modal.querySelector("[data-update-confirm]");
        const confirmCurrent = modal && modal.querySelector("[data-update-confirm-current]");
        const confirmTarget = modal && modal.querySelector("[data-update-confirm-target]");
        let currentStatus = null;
        let requestPending = false;
        let pollTimer = null;
        let trackedJobID = "";
        let hasWatchCredential = isPublicWatch;
        let successReloadScheduled = false;
        let detailsLoaded = isPublicWatch;

        try { trackedJobID = window.sessionStorage.getItem("edge_application_update_job") || ""; } catch (_error) { /* optional */ }

        function setHidden(node, hidden) {
            if (node) node.classList.toggle("hidden", Boolean(hidden));
        }

        function selectedFileIsValid() {
            const file = nodes.file && nodes.file.files && nodes.file.files[0];
            return Boolean(file && file.size > 0 && file.size <= 256 * 1024 * 1024 &&
                /^edge-controller-rk3562-[A-Za-z0-9._+\-]+\.tar\.gz$/.test(file.name));
        }

        function uploadIsLocked(status) {
            const state = String(status && status.state || "IDLE").toUpperCase();
            return requestPending || activeUpdateStates.has(state) || Boolean(status && status.runner_requested);
        }

        function executionIsActive(status) {
            const state = String(status && status.state || "IDLE").toUpperCase();
            return ["BACKING_UP", "PREPARING", "STOPPING", "INSTALLING", "STARTING", "VERIFYING", "ROLLING_BACK"].includes(state) ||
                Boolean(status && status.runner_requested);
        }

        function statusNeedsPolling() {
            const state = String(currentStatus && currentStatus.state || "").toUpperCase();
            if (["SUCCESS", "FAILED", "ROLLED_BACK"].includes(state)) return false;
            return activeUpdateStates.has(state) || Boolean(currentStatus && currentStatus.runner_requested) ||
                hasWatchCredential || isPublicWatch;
        }

        function setModalLocked(locked) {
            if (!updateModal) return;
            updateModal.dataset.updateLocked = locked ? "true" : "false";
            updateModal.querySelectorAll("[data-update-modal-close]").forEach(function (button) {
                button.classList.toggle("hidden", locked);
            });
            if (locked && document.activeElement && document.activeElement.closest("[data-update-modal-close]")) root.focus();
        }

        function updateToast(type, message) {
            if (window.EdgeApp && typeof window.EdgeApp.showToast === "function") {
                window.EdgeApp.showToast(type, message);
            }
        }

        function showResult(message, kind) {
            if (!nodes.result) return;
            nodes.result.textContent = message || "";
            nodes.result.className = "application-update-result" + (kind ? " is-" + kind : "");
            setHidden(nodes.result, !message);
        }

        function renderPackage(status) {
            const info = status && status.package_info || {};
            const target = String(info.version || status && status.target_version || "").trim();
            if (nodes.product) nodes.product.textContent = info.product || "edge-controller";
            if (nodes.target) nodes.target.textContent = displayProductVersion(target) || "-";
            if (nodes.platform) nodes.platform.textContent = String(info.platform || "rk3562").toUpperCase();
            if (nodes.arch) nodes.arch.textContent = String(info.arch || "arm64").toUpperCase();
            if (nodes.size) nodes.size.textContent = formatUpdateSize(info.size_bytes);
            if (nodes.buildTime) nodes.buildTime.textContent = formatUpdateTime(info.build_time);
        }

        function renderStatus(status) {
            if (!status || typeof status !== "object") return;
            currentStatus = status;
            const state = String(status.state || "IDLE").toUpperCase();
            const label = updateStateText[state] || "状态未知";
            const taskActive = activeUpdateStates.has(state) || Boolean(status.runner_requested);
            const trackedTerminal = ["SUCCESS", "FAILED", "ROLLED_BACK"].includes(state) && Boolean(status.job_id) && trackedJobID === status.job_id;
            const showPackage = state === "READY" || taskActive || trackedTerminal || isPublicWatch;
            const currentVersion = String(status.current_version || "").trim();
            if (currentVersion && nodes.current) nodes.current.textContent = displayProductVersion(currentVersion);
            if (nodes.badge) {
                nodes.badge.textContent = label;
                nodes.badge.className = "application-update-state-badge status-neutral state-" + state.toLowerCase();
            }
            if (nodes.state) nodes.state.textContent = label;
            renderPackage(status);
            setHidden(nodes.selectionStage, showPackage || isPublicWatch);
            setHidden(nodes.packageStage, !showPackage);
            if (nodes.packageState) nodes.packageState.textContent = state === "READY" ? "升级包校验通过" : label;

            const showProgress = activeUpdateStates.has(state) || state === "UPLOADING" || state === "VALIDATING" || isPublicWatch;
            setHidden(nodes.progress, !showProgress);
            if (nodes.progressBar) nodes.progressBar.style.width = stageWidths[state] || "8%";
            if (nodes.progressTitle) nodes.progressTitle.textContent = label;
            if (nodes.progressMessage) nodes.progressMessage.textContent = status.message || (activeUpdateStates.has(state) ? "页面会持续查询，服务重启期间无需手动刷新。" : "");

            setHidden(nodes.start, state !== "READY" || isPublicWatch || Boolean(status.runner_requested));
            setHidden(nodes.reselect, state !== "READY" || isPublicWatch || Boolean(status.runner_requested));
            setHidden(nodes.reload, !["SUCCESS", "FAILED", "ROLLED_BACK"].includes(state));
            setModalLocked(executionIsActive(status) || requestPending);
            if (nodes.form) {
                const locked = uploadIsLocked(status);
                Array.from(nodes.form.elements).forEach(function (field) {
                    field.disabled = field === nodes.upload ? (locked || !selectedFileIsValid()) : locked;
                });
            }

            // 只有 READY 或正在执行的任务才能建立浏览器跟踪；历史终态只能消费既有跟踪，不能反向创建。
            if (status.job_id && trackableUpdateStates.has(state)) {
                trackedJobID = status.job_id;
                try { window.sessionStorage.setItem("edge_application_update_job", trackedJobID); } catch (_error) { /* optional */ }
            }

            if (state === "SUCCESS") {
                const trackedSuccess = Boolean(status.job_id) && (trackedJobID === status.job_id || isPublicWatch);
                showResult(trackedSuccess ? "应用升级成功。即将重新载入新版本页面资源。" : "上次升级成功。", "success");
                if (!successReloadScheduled && trackedSuccess) {
                    successReloadScheduled = true;
                    scope.setTimeout(function () { window.location.replace("/login?redirect=/operations"); }, 2600);
                }
            } else if (state === "ROLLED_BACK") {
                showResult("升级未完成，系统已自动恢复到原版本。" + (status.error_message ? " " + status.error_message : ""), "warning");
            } else if (state === "FAILED") {
                let message = status.error_message || "升级失败，当前程序未完成切换。";
                if (status.rollback_performed && status.rollback_error) message += " 自动恢复也未成功，请联系维护人员。";
                showResult(message, "danger");
            } else if (state === "READY") {
                showResult("升级包校验通过。请确认目标版本后开始升级。", "success");
            } else {
                showResult("", "");
            }
            if (["IDLE", "SUCCESS", "FAILED", "ROLLED_BACK"].includes(state)) {
                trackedJobID = "";
                try { window.sessionStorage.removeItem("edge_application_update_job"); } catch (_error) { /* optional */ }
            }
        }

        function renderDisconnected() {
            setHidden(nodes.progress, false);
            if (nodes.badge) nodes.badge.textContent = "等待服务恢复";
            if (nodes.state) nodes.state.textContent = "服务重启中";
            if (nodes.progressTitle) nodes.progressTitle.textContent = "服务正在升级并重新启动";
            if (nodes.progressMessage) nodes.progressMessage.textContent = "连接暂时中断，本页会自动继续尝试；这不表示升级失败。";
            showResult("", "");
        }

        function schedulePoll(delay) {
            if (pollTimer) window.clearTimeout(pollTimer);
            pollTimer = scope.setTimeout(pollStatus, delay);
        }

        async function fetchStatusURL(url) {
            const response = await scope.fetch(url, { headers: { "Accept": "application/json" }, cache: "no-store" });
            return readUpdateResponse(response, "升级状态暂时不可用");
        }

        async function pollStatus() {
            if (requestPending || !scope.isActive()) return;
            requestPending = true;
            let result = null;
            try {
                const primaryURL = isPublicWatch ? root.dataset.watchUrl : root.dataset.statusUrl;
                result = await fetchStatusURL(primaryURL);
                if (!result.ok && !isPublicWatch && hasWatchCredential && root.dataset.watchUrl) {
                    result = await fetchStatusURL(root.dataset.watchUrl);
                }
                if (result.ok) {
                    requestPending = false;
                    renderStatus(result.data);
                }
                else if (trackedJobID || hasWatchCredential || activeUpdateStates.has(String(currentStatus && currentStatus.state || ""))) renderDisconnected();
                else showResult(result.message || "升级状态暂时不可用", "warning");
            } catch (error) {
                if (!error || !error.edgeStale) {
                    if (trackedJobID || hasWatchCredential || isPublicWatch) renderDisconnected();
                    else showResult("升级状态暂时不可用，请稍后重试。", "warning");
                }
            } finally {
                requestPending = false;
                if (statusNeedsPolling()) schedulePoll(3000);
            }
        }

        async function loadDialogDetails(statusIsFresh) {
            if (detailsLoaded || isPublicWatch || !scope.isActive()) return;
            detailsLoaded = true;
            const versionURL = root.dataset.versionUrl;
            const statusURL = root.dataset.statusUrl;
            const requests = [];
            if (!statusIsFresh) requests.push(fetchStatusURL(statusURL));
            if (versionURL) requests.push(fetchStatusURL(versionURL));
            try {
                const results = await Promise.all(requests);
                const statusResult = statusIsFresh ? null : results.shift();
                const versionResult = results.shift();
                if (statusResult && statusResult.ok) renderStatus(statusResult.data);
                else if (statusResult) showResult(statusResult.message || "升级状态暂时不可用", "warning");
                if (versionResult && versionResult.ok && nodes.current) {
                    nodes.current.textContent = displayProductVersion(versionResult.data && versionResult.data.version) || "版本信息不可用";
                }
                if (statusNeedsPolling()) schedulePoll(3000);
            } catch (error) {
                detailsLoaded = false;
                if (!error || !error.edgeStale) showResult("升级信息暂时不可用，请稍后重试。", "warning");
            }
        }

        function openDialog(initialStatus, loadDetails, statusIsFresh) {
            if (updateModal && window.EdgeApp && typeof window.EdgeApp.openModal === "function") {
                window.EdgeApp.openModal(updateModal);
            }
            if (initialStatus) renderStatus(initialStatus);
            if (loadDetails !== false) loadDialogDetails(Boolean(statusIsFresh));
            if (statusNeedsPolling()) schedulePoll(3000);
        }

        if (nodes.file) scope.listen(nodes.file, "change", function () {
            const file = nodes.file.files && nodes.file.files[0];
            const validName = file && /^edge-controller-rk3562-[A-Za-z0-9._+\-]+\.tar\.gz$/.test(file.name);
            const validSize = file && file.size > 0 && file.size <= 256 * 1024 * 1024;
            if (nodes.fileName) nodes.fileName.textContent = file ? file.name : "尚未选择文件";
            if (nodes.fileSize) nodes.fileSize.textContent = file ? formatUpdateSize(file.size) : "仅支持正式升级包，不超过 256 MiB";
            if (nodes.upload) nodes.upload.disabled = uploadIsLocked(currentStatus) || !(validName && validSize);
            showResult(file && !validName ? "升级包文件名不符合正式发布包规则。" : (file && !validSize ? "升级包必须大于 0 且不超过 256 MiB。" : ""), "warning");
        });

        if (nodes.form) scope.listen(nodes.form, "submit", async function (event) {
            event.preventDefault();
            const file = nodes.file && nodes.file.files && nodes.file.files[0];
            if (!file || uploadIsLocked(currentStatus)) return;
            const statusBeforeUpload = currentStatus;
            let uploadFailureMessage = "";
            requestPending = true;
            if (nodes.upload) nodes.upload.disabled = true;
            renderStatus({ state: "UPLOADING", current_version: currentStatus && currentStatus.current_version, message: "正在上传升级包，请勿关闭页面。" });
            setModalLocked(true);
            try {
                const body = new FormData();
                body.append("upgrade_file", file, file.name);
                const response = await scope.csrfFetch(root.dataset.uploadUrl, { method: "POST", body: body, headers: { "Accept": "application/json" } });
                const result = await readUpdateResponse(response, "升级包上传或校验失败");
                if (!result.ok) throw new Error(result.message);
                renderStatus(result.data);
                updateToast("success", "升级包上传完成，校验通过");
            } catch (error) {
                if (!error || !error.edgeStale) uploadFailureMessage = error.message || "升级包上传或校验失败";
            } finally {
                requestPending = false;
                if (nodes.file) nodes.file.value = "";
                if (uploadFailureMessage && statusBeforeUpload) renderStatus(statusBeforeUpload);
                else if (currentStatus) renderStatus(currentStatus);
                if (uploadFailureMessage) showResult(uploadFailureMessage, "danger");
                setModalLocked(executionIsActive(currentStatus));
            }
        });

        if (nodes.reselect) scope.listen(nodes.reselect, "click", function () {
            if (!currentStatus || currentStatus.state !== "READY" || requestPending) return;
            const currentVersion = currentStatus.current_version;
            if (nodes.file) nodes.file.value = "";
            if (nodes.fileName) nodes.fileName.textContent = "尚未选择文件";
            if (nodes.fileSize) nodes.fileSize.textContent = "仅支持 edge-controller-rk3562-<version>.tar.gz";
            renderStatus({ state: "IDLE", current_version: currentVersion });
        });

        if (nodes.start) scope.listen(nodes.start, "click", function () {
            if (!currentStatus || currentStatus.state !== "READY" || !modal) return;
            if (confirmCurrent) confirmCurrent.textContent = displayProductVersion(currentStatus.current_version) || "当前版本";
            if (confirmTarget) confirmTarget.textContent = displayProductVersion(currentStatus.target_version || currentStatus.package_info && currentStatus.package_info.version) || "目标版本";
            modal.classList.add("open");
            modal.setAttribute("aria-hidden", "false");
            document.body.classList.add("modal-open");
            if (confirmButton) confirmButton.focus();
        });

        if (confirmButton) scope.listen(confirmButton, "click", async function () {
            if (requestPending || !currentStatus || currentStatus.state !== "READY") return;
            requestPending = true;
            confirmButton.disabled = true;
            setModalLocked(true);
            const jobID = currentStatus.job_id;
            const body = JSON.stringify({ job_id: jobID });
            try {
                let response = await scope.csrfFetch("/api/application-update/watch-token", { method: "POST", headers: { "Accept": "application/json", "Content-Type": "application/json" }, body: body });
                let result = await readUpdateResponse(response, "无法建立升级等待状态");
                if (!result.ok) throw new Error(result.message);
                hasWatchCredential = true;
                trackedJobID = jobID;
                try { window.sessionStorage.setItem("edge_application_update_job", jobID); } catch (_error) { /* optional */ }
                closeUpdateModal(modal);
                response = await scope.csrfFetch(root.dataset.startUrl, { method: "POST", headers: { "Accept": "application/json", "Content-Type": "application/json" }, body: body });
                result = await readUpdateResponse(response, "升级启动请求暂时没有响应");
                if (result.ok) {
                    renderStatus(result.data);
                    updateToast("success", "升级任务已启动");
                }
                else renderDisconnected();
                setModalLocked(true);
                schedulePoll(500);
            } catch (error) {
                if (hasWatchCredential) {
                    closeUpdateModal(modal);
                    renderDisconnected();
                    schedulePoll(500);
                } else if (!error || !error.edgeStale) {
                    showResult(error.message || "无法开始升级", "danger");
                }
            } finally {
                requestPending = false;
                confirmButton.disabled = false;
                if (!hasWatchCredential) setModalLocked(executionIsActive(currentStatus));
            }
        });

        if (nodes.reload) scope.listen(nodes.reload, "click", function () {
            window.location.assign("/login?redirect=/operations");
        });
        if (updateModal) {
            scope.listen(updateModal, "click", function (event) {
                const closeTarget = event.target.closest("[data-update-modal-close]");
                if (!closeTarget) return;
                if (executionIsActive(currentStatus) || requestPending) {
                    event.preventDefault();
                    event.stopImmediatePropagation();
                    return;
                }
                if (window.EdgeApp && typeof window.EdgeApp.closeModal === "function") window.EdgeApp.closeModal(updateModal);
                if (pollTimer) window.clearTimeout(pollTimer);
                pollTimer = null;
                detailsLoaded = false;
            }, true);
            scope.listen(document, "keydown", function (event) {
                if (event.key !== "Escape" || !updateModal.classList.contains("open")) return;
                if (executionIsActive(currentStatus) || requestPending) {
                    event.preventDefault();
                    event.stopImmediatePropagation();
                } else {
                    if (pollTimer) window.clearTimeout(pollTimer);
                    pollTimer = null;
                    detailsLoaded = false;
                }
            }, true);
        }
        scope.onDispose(function () {
            if (pollTimer) window.clearTimeout(pollTimer);
            pollTimer = null;
        });
        return { open: openDialog, renderStatus: renderStatus, poll: pollStatus };
    }

    function initApplicationUpdateEntry(scope) {
        const entry = document.querySelector("[data-application-update-entry]");
        const root = document.querySelector("#application-update-modal [data-application-update]");
        if (!entry || !root) return;
        const openButton = entry.querySelector("[data-update-open]");
        const entryVersion = entry.querySelector("[data-update-entry-version]");
        const entryCurrent = entry.querySelector("[data-update-entry-current]");
        let module = null;
        let detectedStatus = null;
        let detectionSettled = false;

        function ensureModule() {
            if (!module) module = initApplicationUpdate(scope, root);
            return module;
        }

        if (openButton) scope.listen(openButton, "click", function () {
            const controller = ensureModule();
            if (!controller) return;
            if (detectionSettled) controller.open(detectedStatus, true, Boolean(detectedStatus));
            else {
                controller.open(null, false);
                detectionPromise.then(function () {
                    if (scope.isActive()) controller.open(detectedStatus, true, Boolean(detectedStatus));
                });
            }
        });

        // 首屏只做一次轻量状态探测；无活动任务时不初始化上传逻辑，也不启动轮询。
        const detectionPromise = scope.fetch(entry.dataset.statusUrl, { headers: { "Accept": "application/json" }, cache: "no-store" })
            .then(function (response) { return readUpdateResponse(response, "升级状态暂时不可用"); })
            .then(function (result) {
                detectionSettled = true;
                if (!result.ok || !result.data) return;
                const status = result.data;
                detectedStatus = status;
                if (status.current_version && entryCurrent) {
                    entryCurrent.textContent = displayProductVersion(status.current_version);
                    if (entryVersion) entryVersion.classList.remove("hidden");
                }
                const state = String(status.state || "IDLE").toUpperCase();
                if (["IDLE", "SUCCESS", "FAILED", "ROLLED_BACK"].includes(state)) {
                    try { window.sessionStorage.removeItem("edge_application_update_job"); } catch (_error) { /* optional */ }
                }
                if (!activeUpdateStates.has(state) && !status.runner_requested) return;
                const controller = ensureModule();
                if (controller) controller.open(status, false);
            })
            .catch(function (error) {
                detectionSettled = true;
                if (error && error.edgeStale) return;
            });
    }

    function mount(scope) {
        pageScope = scope;
        initRolePreviews();
        initHistoryCleanupConfirmation();
        const publicWatchRoot = document.querySelector("[data-application-update][data-public-watch='true']");
        if (publicWatchRoot) {
            const controller = initApplicationUpdate(scope, publicWatchRoot);
            if (controller) controller.open(null, false);
        } else {
            initApplicationUpdateEntry(scope);
        }
    }

    if (window.EdgeApp && window.EdgeApp.registerPageController) {
        window.EdgeApp.registerPageController("operations", ["operations"], { mount: mount });
    }
})();
