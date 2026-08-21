// 运维页高风险维护动作：配置导入与恢复出厂。
// 请求归属当前页面 scope，软导航离开后不得继续更新新页面或触发刷新。
(function () {
    "use strict";

    const EdgeApp = window.EdgeApp;
    if (!EdgeApp || !EdgeApp.registerPageController) return;

    const readApiResponse = EdgeApp.readApiResponse;
    const showToast = EdgeApp.showToast;
    const setFeedback = EdgeApp.setFeedback;
    const closeModal = EdgeApp.closeModal;
    const confirmAction = EdgeApp.confirmAction;

    function initConfigImportActions(scope) {
        const maxFileBytes = 2 * 1024 * 1024;
        document.querySelectorAll("[data-config-import-form]").forEach(function (form) {
            const fileInput = form.querySelector("[data-config-import-file]");
            const fileName = form.querySelector("[data-config-import-file-name]");
            const feedback = form.querySelector("[data-config-import-feedback]");
            const submitButton = form.querySelector("[data-config-import-submit]");
            const submitLabel = submitButton ? submitButton.textContent : "导入配置";
            let submitting = false;

            function selectedFile() {
                return fileInput && fileInput.files && fileInput.files.length ? fileInput.files[0] : null;
            }

            function formatFileSize(bytes) {
                if (bytes < 1024) return bytes + " B";
                return (bytes / 1024 / 1024).toFixed(2) + " MB";
            }

            function updateFileName() {
                const file = selectedFile();
                if (fileName) {
                    fileName.textContent = file
                        ? "已选择：" + file.name + "（" + formatFileSize(file.size) + "）"
                        : "尚未选择文件";
                }
                setFeedback(feedback, "", "");
            }

            if (fileInput) scope.listen(fileInput, "change", updateFileName);

            scope.listen(form, "submit", async function (event) {
                event.preventDefault();
                if (submitting) return;

                const file = selectedFile();
                if (!file) {
                    setFeedback(feedback, "error", "请选择要导入的 JSON 配置文件");
                    if (fileInput) fileInput.focus();
                    return;
                }
                if (!/\.json$/i.test(file.name)) {
                    setFeedback(feedback, "error", "仅支持 JSON 配置文件");
                    return;
                }
                if (file.size > maxFileBytes) {
                    setFeedback(feedback, "error", "配置文件不能超过 2 MB");
                    return;
                }
                if (!await confirmAction({
                    title: "确认网络配置迁移风险",
                    message: "配置包包含网络配置。跨设备导入静态 IP 可能造成地址冲突或当前访问地址失效，请先确认目标设备网络环境。MQTT 密码、TLS 证书文件内容、用户账号、历史数据和事件数据不会迁移。确认继续导入吗？",
                    confirmText: "已确认风险，继续导入",
                    danger: true
                })) return;

                submitting = true;
                if (submitButton) {
                    submitButton.disabled = true;
                    submitButton.textContent = "正在导入...";
                }
                setFeedback(feedback, "", "");

                const body = new FormData();
                body.append("config_file", file, file.name);
                try {
                    const response = await scope.csrfFetch(form.action, {
                        method: "POST",
                        body: body,
                        credentials: "same-origin",
                        cache: "no-store",
                        headers: {
                            "Accept": "application/json",
                            "X-Requested-With": "XMLHttpRequest"
                        }
                    });
                    const result = await readApiResponse(response, "系统配置导入失败");
                    if (!result.ok) {
                        setFeedback(feedback, "error", result.message);
                        return;
                    }

                    const message = result.data && result.data.message
                        ? result.data.message : "系统配置导入成功";
                    const reloadRequired = !result.data || result.data.reload_required !== false;
                    form.reset();
                    updateFileName();
                    showToast("success", message + (reloadRequired ? "；页面即将刷新以加载最新配置。" : ""));
                    const modal = form.closest(".channel-modal");
                    if (modal) closeModal(modal);
                    if (reloadRequired && scope.isActive()) {
                        scope.setTimeout(function () { window.location.reload(); }, 5500);
                    }
                } catch (error) {
                    if (error && error.edgeStale) return;
                    setFeedback(feedback, "error", "配置导入请求失败，请检查网络连接后重试");
                } finally {
                    submitting = false;
                    if (submitButton) {
                        submitButton.disabled = false;
                        submitButton.textContent = submitLabel;
                    }
                }
            });
        });
    }

    // 恢复出厂不可逆，提交前明确说明实际保留和清理的范围。
    function initFactoryResetActions(scope) {
        document.querySelectorAll("[data-factory-reset-form]").forEach(function (form) {
            scope.listen(form, "submit", async function (event) {
                event.preventDefault();
                const confirmed = await confirmAction({
                    title: "确认恢复出厂数据",
                    message: "此操作会恢复基础与时间设置，清除采集配置、MQTT、Modbus 北向配置与映射、用户账号密码、历史采集数据、历史事件、告警配置和状态。当前系统时间不会调整；网络配置、内置及自定义设备类型、软件版本、证书文件、运行日志和升级资料保持不变。完成后会重新开放首次部署管理员入口。",
                    confirmText: "确认恢复",
                    danger: true
                });
                if (confirmed) form.submit();
            });
        });
    }

    function mount(scope) {
        initConfigImportActions(scope);
        initFactoryResetActions(scope);
    }

    EdgeApp.registerPageController("maintenance", ["operations"], { mount: mount });
})();
