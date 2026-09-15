// 实时监控页控制器：周期获取后端聚合快照，并同步更新三级筛选树、数据表及分段滚动状态。
// 刷新失败时保留最近一次成功数据，同时明确标记连接异常，避免把网络故障误显示为设备离线。
(function () {
    "use strict";

    const EdgeApp = window.EdgeApp;
    if (!EdgeApp) return;

    const readTreeState = EdgeApp.readTreeState;
    const writeTreeState = EdgeApp.writeTreeState;
    const friendlyApiMessage = EdgeApp.friendlyApiMessage;
    const escapeHtml = EdgeApp.escapeHtml;
    const dataItemDisplayName = EdgeApp.dataItemDisplayName;
    const isPageHidden = EdgeApp.isPageHidden || function () { return false; };
    const EdgeMotion = window.EdgeMotion;
    let pageScope = null;

    function mount(scope) {
        pageScope = scope;
        initRealtimeTable();
    }


    // ---------- 实时数据快照、层级筛选和表格刷新 ----------
    // 页面只启动一个刷新循环；可见性变化时沿用同一状态缓存，避免重复定时器并发请求。
    function initRealtimeTable() {
        const scope = pageScope;
        const table = document.getElementById("realtime-table");
        if (!table) {
            return;
        }

        const refreshUrl = table.dataset.refreshUrl;
        const tbody = document.getElementById("realtime-table-body");
        const refreshTime = document.getElementById("realtime-refresh-time");
        const errorBox = document.getElementById("realtime-error");
        const tableWrap = table.closest(".realtime-table-wrap");
        const emptyPanel = document.getElementById("realtime-empty-panel");
        const emptyTitle = document.getElementById("realtime-empty-title");
        const topBackendState = document.getElementById("backend-status-pill");
        const connectionAlert = document.getElementById("realtime-connection-alert");
        const reconnectButtons = document.querySelectorAll("[data-realtime-reconnect]");
        const refreshButton = document.getElementById("realtime-refresh-button");
        const refreshIntervalSelector = document.getElementById("realtime-refresh-interval");
        const refreshPreferenceKey = "edge.realtime.refresh-interval.v1";
        const allowedRefreshIntervals = [0, 1000, 3000, 5000, 10000];
        let refreshIntervalMS = 10000;
        try {
            const saved = window.localStorage.getItem(refreshPreferenceKey);
            if (saved !== null && allowedRefreshIntervals.includes(Number(saved))) refreshIntervalMS = Number(saved);
        } catch (_) { /* 浏览器禁止存储时使用默认周期。 */ }
        if (refreshIntervalSelector) refreshIntervalSelector.value = String(refreshIntervalMS);
        const channelList = document.getElementById("realtime-channel-list");
        const masterList = document.getElementById("realtime-master-list");
        const channelSummary = document.getElementById("realtime-channel-summary");
        const masterSummary = document.getElementById("realtime-master-summary");
        const deviceState = document.getElementById("realtime-device-state");
        const deviceOnline = document.getElementById("realtime-device-online");
        const deviceTotal = document.getElementById("realtime-device-total");
        const alarmCard = document.getElementById("realtime-alarm-card");
        const alarmIcon = document.getElementById("realtime-alarm-icon");
        const alarmCount = document.getElementById("realtime-alarm-count");
        const alarmState = document.getElementById("realtime-alarm-state");
        const alarmDeviceViewport = document.getElementById("realtime-alarm-device-viewport");
        const alarmDeviceTrack = document.getElementById("realtime-alarm-device-track");
        const tableStatus = document.getElementById("realtime-table-status");
        const filterTree = document.getElementById("realtime-filter-tree");
        const deviceSelectorWrap = document.getElementById("realtime-device-selector-wrap");
        const deviceSelector = document.getElementById("realtime-device-selector");
        const currentDeviceName = document.getElementById("realtime-current-device-name");
        let allRows = readInitialRealtimeRows();
        let activeFilter = { type: "all", id: "", label: "全部设备" };
        let selectedDeviceID = "";
        let lastRows = allRows.length ? allRows : readTableRows();
        let realtimeDataStale = false;
        let lastSuccessfulRefreshText = refreshTime ? String(refreshTime.textContent || "").trim() : "";
        let realtimeRefreshInFlight = false;
        let realtimeRefreshTimer = null;
        let rowLayoutTimer = null;
        let alarmCarouselTimer = null;
        let alarmCarouselIndex = 0;
        let alarmCarouselSignature = "";
        let currentAlarmDeviceNames = [];
        let channelAutoScroller = null;
        let masterAutoScroller = null;
        let autoScrollResizeTimer = null;
        const reduceAlarmMotion = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
        const realtimeTreeStorageKey = "edge.realtime.device-tree.v1";
        const realtimeSelectionStorageKey = "edge.realtime.selection.v1";
        let realtimeTreeState = readTreeState(realtimeTreeStorageKey);
        const realtimePointValues = {};
        const realtimePointStates = {};

        restoreRealtimeSelection();
        reconcileActiveSelection();
        renderFilterTree(allRows);
        // 服务端已经输出“全部设备”首屏表格；默认筛选时只接管状态，
        // 避免脚本启动后把同一批行完整替换一次。
        if (activeFilter.type === "all" && tbody && tbody.children.length) {
            updateFilterState(allRows.length);
        } else {
            renderCurrentRows(table.dataset.emptyText || "暂无实时数据");
        }
        // 每行按内容宽度分配空间；说明不足以容纳短文本时让位给数据。
        function scheduleRowLayout() {
            if (rowLayoutTimer !== null) return;
            rowLayoutTimer = scope.setTimeout(function () {
                rowLayoutTimer = null;
                const cells = Array.from(tbody.querySelectorAll(".realtime-cell-error"));
                // 先读后写，避免逐行交错触发布局。
                const hidden = cells.map(function (cell) { return cell.clientWidth < 170; });
                cells.forEach(function (cell, index) {
                    cell.classList.toggle("is-space-hidden", hidden[index]);
                    cell.setAttribute("aria-hidden", hidden[index] ? "true" : "false");
                });
            }, 0);
        }
        if (window.ResizeObserver) {
            const rowObserver = new ResizeObserver(scheduleRowLayout);
            rowObserver.observe(table);
            scope.onDispose(function () { rowObserver.disconnect(); });
        }
        scope.listen(window, "resize", scheduleRowLayout);
        scheduleRowLayout();
        hydrateAlarmDeviceItems();
        channelAutoScroller = createSmoothLoopScroller(channelList, 1800);
        masterAutoScroller = createSmoothLoopScroller(masterList, 2300);

        // 每次响应作为同一代快照整体替换，防止树、统计和表格展示不同采集时刻的数据。
        async function refreshRealtime() {
            if (isPageHidden() || realtimeRefreshInFlight) {
                return;
            }
            realtimeRefreshInFlight = true;
            if (refreshButton) {
                refreshButton.disabled = true;
                refreshButton.textContent = "刷新中…";
            }
            try {
                const response = await scope.fetch(refreshUrl, {
                    method: "GET",
                    headers: { "Accept": "application/json" }
                });

                const payload = await response.json();
                if (!scope.isActive()) return;
                if (!response.ok || !payload.success) {
                    const message = payload && payload.error ? payload.error.message : "实时刷新失败";
                    throw new Error(friendlyApiMessage(message, "实时刷新失败"));
                }

                const data = payload.data || {};
                const rows = data.rows || [];
                const dashboard = data.dashboard || {};
                const backendReachable = data.backend_reachable !== false;
                realtimeDataStale = !backendReachable;
                if (backendReachable || rows.length) {
                    allRows = rows;
                }
                reconcileActiveSelection();
                renderFilterTree(allRows);
                renderCurrentRows(dashboard.empty_state_text || table.dataset.emptyText || "暂无实时数据");
                if (refreshTime) {
                    refreshTime.textContent = data.refreshed_at || "未知";
                }
                updateBackendState(backendReachable);
                if (backendReachable) {
                    lastRows = allRows;
                    lastSuccessfulRefreshText = data.refreshed_at || lastSuccessfulRefreshText || "未知";
                    clearRealtimeStalePresentation();
                    renderDashboard(dashboard, true);
                } else {
                    markRealtimeDataStale(friendlyApiMessage(data.error_message || "", "后端服务未连接"));
                }

                if (errorBox) {
                    errorBox.className = "flash flash-error hidden";
                    errorBox.textContent = "";
                }
                if (data.backend_reachable === false) {
                    showConnectionAlert(true, friendlyApiMessage(data.error_message || "", "后端服务未连接"));
                } else {
                    showConnectionAlert(false, "");
                }
            } catch (error) {
                if (!scope.isActive() || EdgeApp.isStalePageError(error)) return;
                const message = friendlyApiMessage(error.message, "实时刷新失败");
                if (errorBox) {
                    errorBox.className = "flash flash-error hidden";
                    errorBox.textContent = "";
                }
                updateBackendState(false);
                showConnectionAlert(true, message);
                markRealtimeDataStale(message);
            } finally {
                realtimeRefreshInFlight = false;
                if (refreshButton) {
                    refreshButton.disabled = false;
                    refreshButton.textContent = "立即刷新";
                }
            }
        }

        // 读取初始实时行。
        function readInitialRealtimeRows() {
            if (!table || !table.dataset.initialRows) {
                return [];
            }
            try {
                const parsed = JSON.parse(table.dataset.initialRows);
                return Array.isArray(parsed) ? parsed : [];
            } catch (error) {
                return [];
            }
        }

        // 渲染当前行。
        function renderCurrentRows(emptyText) {
            const filteredRows = currentFilteredRows();
            renderRows(filteredRows, activeFilter.type === "all" ? emptyText : "当前筛选范围内暂无设备数据");
            updateFilterState(filteredRows.length);
        }

        // 获取当前筛选后行。
        function currentFilteredRows() {
            if (activeFilter.type === "all") {
                return allRows;
            }
            return allRows.filter(function (row) {
                if (activeFilter.type === "channel") {
                    return rowKey(row, "channel") === activeFilter.id;
                }
                if (activeFilter.type === "master") {
                    return rowKey(row, "master") === activeFilter.id &&
                        rowKey(row, "device") === selectedDeviceID;
                }
                return true;
            });
        }

        // 生成实时数据行的稳定标识。
        function fallbackStableKey(kind, parts) {
            return [kind].concat(parts).map(function (value) {
                const text = String(value == null ? "" : value);
                return text.length + ":" + text + "|";
            }).join("");
        }

        function rowKey(row, type) {
            if (type === "channel") {
                return String(row.channel_id || "未知通道");
            }
            if (type === "master") {
                return String(row.master_id || "未知主站");
            }
            if (type === "device") {
                return String(row.stable_key || fallbackStableKey("row", [
                    row.channel_id || "",
                    row.master_id || "",
                    row.device_id || row.device_name || ""
                ]));
            }
            return "";
        }

        // 生成实时数据行的显示名称。
        function rowLabel(row, type) {
            if (type === "channel") {
                return row.channel_name || row.channel_id || "未知通道";
            }
            if (type === "master") {
                return row.master_name || row.master_id || "未知主站";
            }
            if (type === "device") {
                return row.device_name || row.device_id || "未知设备";
            }
            return "全部设备";
        }

        // 渲染行。
        function renderRows(rows, emptyText) {
            if (!rows.length) {
                // 空数据不再渲染表格头，直接切换到同一区域内的面板级空状态。
                setHTMLIfChanged(tbody, "");
                showRealtimeEmpty(emptyText || "暂无实时数据");
                return;
            }

            showRealtimeTable();
            const visiblePointKeys = {};
            const motionBudget = { remaining: 8 };
            const valueChanges = [];
            const qualityChanges = [];
            const renderedRows = rows.map(function (row) {
                const rowErrorMessage = displayRealtimeRowError(row.error_message || "");
                const rowClass = realtimeDataStale
                    ? "row-muted row-stale"
                    : rowErrorMessage
                    ? "row-alert"
                    : (row.has_status ? (row.online ? "row-live" : "row-offline") : "row-muted");
                const rowStatus = realtimeRowStatus(row, rowErrorMessage);
                const readingCell = renderSummaryPoints(
                    row,
                    visiblePointKeys,
                    motionBudget,
                    valueChanges,
                    qualityChanges,
                    rowErrorMessage
                );
                const errorCell = (realtimeDataStale
                    ? '<span class="table-message table-message-neutral">缓存值 · 最后更新 ' +
                        escapeHtml(lastSuccessfulRefreshText || "未知") + "</span>"
                    : '<span class="realtime-explanation">' + escapeHtml(row.explanation || (rowErrorMessage ? "采集异常，请检查设备" : !row.has_status ? "尚未采集，等待数据" : !row.online ? "设备离线，请检查连接" : "—")) + '</span>');
                const hierarchy = [
                    '<div class="hierarchy-cell">',
                    '<span class="hierarchy-line">', escapeHtml(row.channel_name || row.channel_id || "-"), " / ", escapeHtml(row.master_name || row.master_id || "-"), "</span>",
                    '<span class="hierarchy-primary"><strong>', escapeHtml(row.device_name || "未命名设备"), "</strong>",
                    "</span>",
                    "</div>"
                ].join("");
                const key = rowKey(row, "device");
                return {
                    key: key,
                    className: rowClass,
                    hierarchy: hierarchy,
                    reading: readingCell,
                    error: errorCell,
                    status: rowStatus,
                    markup: [
                    '<tr data-realtime-row-key="', escapeHtml(key), '" class="', rowClass, '">',
                    '<td class="realtime-cell-hierarchy">', hierarchy, "</td>",
                    '<td class="realtime-cell-reading">', readingCell, "</td>",
                    '<td class="realtime-cell-error">', errorCell, "</td>",
                    "</tr>"
                    ].join("")
                };
            });
            const currentRows = Array.from(tbody.children);
            const canPatchRows = currentRows.length === renderedRows.length &&
                currentRows.every(function (node, index) {
                    return node.dataset.realtimeRowKey === renderedRows[index].key;
                });
            const rowStatusChanges = [];
            if (canPatchRows) {
                renderedRows.forEach(function (entry, index) {
                    const rowNode = currentRows[index];
                    if (rowNode.className !== entry.className) rowNode.className = entry.className;
                    setHTMLIfChanged(rowNode.querySelector(".realtime-cell-hierarchy"), entry.hierarchy);
                    reconcileRealtimeReading(rowNode.querySelector(".realtime-cell-reading"), entry.reading);
                    setHTMLIfChanged(rowNode.querySelector(".realtime-cell-error"), entry.error);
                });
            } else {
                setHTMLIfChanged(tbody, renderedRows.map(function (entry) {
                    return entry.markup;
                }).join(""));
            }
            scheduleRowLayout();
            applyRealtimeMotion(valueChanges, qualityChanges, rowStatusChanges);
            Object.keys(realtimePointValues).forEach(function (key) {
                if (!visiblePointKeys[key]) delete realtimePointValues[key];
            });
            Object.keys(realtimePointStates).forEach(function (key) {
                if (!visiblePointKeys[key]) delete realtimePointStates[key];
            });
        }

        function applyRealtimeMotion(valueChanges, qualityChanges, rowStatusChanges) {
            if (!EdgeMotion) return;
            const valuesByKey = {};
            tbody.querySelectorAll("[data-realtime-value-key]").forEach(function (element) {
                valuesByKey[element.dataset.realtimeValueKey || ""] = element;
            });
            valueChanges.forEach(function (change) {
                const element = valuesByKey[change.key];
                if (element) EdgeMotion.markValue(element, change.previous, change.next);
            });

            const pointsByKey = {};
            tbody.querySelectorAll("[data-realtime-point-key]").forEach(function (element) {
                pointsByKey[element.dataset.realtimePointKey || ""] = element;
            });
            qualityChanges.forEach(function (change) {
                const element = pointsByKey[change.key];
                if (element) {
                    EdgeMotion.markStatus(element, change.previous, change.next, {
                        critical: change.next !== "status-ok"
                    });
                }
            });
            rowStatusChanges.forEach(function (change) {
                EdgeMotion.markStatus(change.element, change.previous, change.next, {
                    critical: change.critical
                });
            });
        }

        // 汇总实时数据行的状态。
        function realtimeRowStatus(row, errorMessage) {
            if (realtimeDataStale) {
                return { text: "缓存值", className: "status-neutral", showLabel: true };
            }
            if (errorMessage) {
                return { text: "异常", className: "status-bad", showLabel: true };
            }
            if (!row.has_status) {
                return { text: "未采集", className: "status-neutral", showLabel: true };
            }
            if (row.online) {
                return { text: "在线", className: "status-ok", showLabel: false };
            }
            return { text: "离线", className: "status-warn", showLabel: true };
        }

        // 显示实时空状态。
        function showRealtimeEmpty(emptyText) {
            if (emptyTitle) {
                emptyTitle.textContent = emptyText;
            }
            if (tableWrap) {
                tableWrap.classList.add("is-hidden");
            }
            if (emptyPanel) {
                emptyPanel.classList.remove("is-hidden");
                if (EdgeMotion) EdgeMotion.reveal(emptyPanel);
            }
        }

        // 显示实时表格。
        function showRealtimeTable() {
            if (emptyPanel) {
                emptyPanel.classList.add("is-hidden");
            }
            if (tableWrap) {
                tableWrap.classList.remove("is-hidden");
            }
        }

        // 渲染摘要点位。
        function renderSummaryPoints(
            row,
            visiblePointKeys,
            motionBudget,
            valueChanges,
            qualityChanges,
            rowErrorMessage
        ) {
            const points = Array.isArray(row.summary_points) ? row.summary_points : [];
            if (points.length) {
                const renderPoint = function (point, index) {
                    const valid = point.valid !== false;
                    const title = realtimeDataStale
                        ? "缓存值，最后更新 " + (lastSuccessfulRefreshText || "未知")
                        : (point.message || point.quality || "");
                    const displayParts = realtimePointDisplayParts(point, valid);
                    const valueText = displayParts.value;
                    const unitText = displayParts.unit;
                    const qualityState = realtimeDataStale
                        ? "status-neutral"
                        : String(point.state_class || (valid ? "status-ok" : "status-warn"));
                    const valueKey = String(point.stable_key || fallbackStableKey("point", [
                        row.channel_id || "",
                        row.master_id || "",
                        row.device_id || row.device_name || "",
                        point.key || point.name || index
                    ]));
                    const hadPrevious = Object.prototype.hasOwnProperty.call(realtimePointValues, valueKey);
                    const previousText = hadPrevious ? realtimePointValues[valueKey] : valueText;
                    const hadPreviousState = Object.prototype.hasOwnProperty.call(realtimePointStates, valueKey);
                    const previousState = hadPreviousState ? realtimePointStates[valueKey] : qualityState;
                    visiblePointKeys[valueKey] = true;
                    realtimePointValues[valueKey] = valueText;
                    realtimePointStates[valueKey] = qualityState;
                    const shouldAnimate = realtimeValueMotionAllowed(
                        row,
                        point,
                        rowErrorMessage,
                        qualityState
                    ) && hadPrevious && previousText !== valueText &&
                        motionBudget.remaining > 0;
                    if (shouldAnimate) {
                        motionBudget.remaining -= 1;
                        valueChanges.push({ key: valueKey, previous: previousText, next: valueText });
                    }
                    if (hadPreviousState && previousState !== qualityState) {
                        qualityChanges.push({ key: valueKey, previous: previousState, next: qualityState });
                    }
                    return [
                        '<span class="metric-chip ', valid ? '' : 'metric-chip-invalid ', qualityState === "status-ok" ? "" : "metric-chip-warn ", qualityState,
                        '" data-realtime-point-key="', escapeHtml(valueKey), '" data-quality-state="', escapeHtml(qualityState),
                        '" title="', escapeHtml(title), '">',
                        "<b>", escapeHtml(dataItemDisplayName(point.name, point.key)), "</b>",
                        '<span class="metric-chip-reading"><em class="metric-chip-value" data-realtime-value-key="',
                        escapeHtml(valueKey), '">', escapeHtml(valueText), "</em>",
                        unitText ? '<small class="metric-chip-unit">' + escapeHtml(unitText) + "</small>" : "",
                        "</span>",
                        "</span>"
                    ].join("");
                };
                const groups = Array.isArray(row.realtime_groups) ? row.realtime_groups : [];
                if (row.realtime_grouping_enabled === true && groups.length) {
                    return '<div class="metric-group-list">' + groups.map(function (group, groupIndex) {
                        const groupPoints = Array.isArray(group.points) ? group.points : [];
                        const groupKey = String(group.id || group.name || groupIndex);
                        return '<section class="metric-group" data-realtime-group-key="' +
                            escapeHtml(groupKey) + '"><strong class="metric-group-title">' +
                            escapeHtml(group.name || "未命名分组") + '</strong><div class="metric-group-points">' +
                            groupPoints.map(renderPoint).join("") + "</div></section>";
                    }).join("") + "</div>";
                }
                return [
                    '<div class="metric-list metric-list-ungrouped',
                    points.length === 1 ? " metric-list-single" : "",
                    '">',
                    points.map(renderPoint).join(""),
                    "</div>"
                ].join("");
            }
            const readingClass = !realtimeDataStale && row.has_realtime
                ? "reading reading-live"
                : "reading reading-muted";
            const summaryText = row.summary_text || "暂无数据";
            const summaryKey = String(row.summary_key || fallbackStableKey("summary", [
                row.channel_id || "",
                row.master_id || "",
                row.device_id || row.device_name || ""
            ]));
            const hadSummary = Object.prototype.hasOwnProperty.call(realtimePointValues, summaryKey);
            const previousSummary = hadSummary ? realtimePointValues[summaryKey] : summaryText;
            visiblePointKeys[summaryKey] = true;
            realtimePointValues[summaryKey] = summaryText;
            const shouldAnimateSummary = realtimeValueMotionAllowed(row, null, rowErrorMessage, "status-ok") &&
                row.has_realtime && hadSummary && previousSummary !== summaryText &&
                motionBudget.remaining > 0;
            if (shouldAnimateSummary) {
                motionBudget.remaining -= 1;
                valueChanges.push({ key: summaryKey, previous: previousSummary, next: summaryText });
            }
            return '<div class="metric-list"><span class="' + readingClass +
                ' metric-chip-value" data-realtime-value-key="' + escapeHtml(summaryKey) + '">' +
                escapeHtml(summaryText) + "</span></div>";
        }

        // API 结构保持不变：刷新响应仍使用既有 text 字段；仅在前端把标准
        // 数字与其尾部单位拆成稳定节点，文本枚举值保持原样。
        function realtimePointDisplayParts(point, valid) {
            const text = String(point && (point.value_text || point.text) || "-").trim();
            if (!valid) return { value: text && text !== "-" ? text : "数据无效", unit: "" };
            let unit = String(point && point.unit || "").trim();
            if (!point || !point.value_text) {
                const match = text.match(/^([-+]?(?:(?:\d{1,3}(?:,\d{3})+)|\d+)(?:\.\d+)?)(?:\s+(.+))?$/);
                if (match) {
                    if (!unit) unit = String(match[2] || "").trim();
                    return { value: match[1], unit: unit };
                }
            }
            return { value: text, unit: unit };
        }

        // 普通方向反馈只用于在线、质量良好且没有告警/通讯异常的物理量。
        function realtimeValueMotionAllowed(row, point, rowErrorMessage, qualityState) {
            if (realtimeDataStale) return false;
            if (!row || rowErrorMessage || !row.has_status || !row.online || !row.has_realtime) return false;
            if (point && point.valid === false) return false;
            if (qualityState && qualityState !== "status-ok") return false;
            const diagnosis = row.diagnosis || {};
            const signature = [
                diagnosis.status,
                diagnosis.level,
                diagnosis.error_code,
                diagnosis.message
            ].join("|").toLowerCase();
            return !/(data_alarm|alarm|error|fault|offline|invalid|异常|告警|报警|越限|离线|无效)/.test(signature);
        }

        // 返回实时数据行的可展示错误。
        function displayRealtimeRowError(message) {
            const text = friendlyApiMessage(message || "", "");
            return isGenericRealtimeRowMessage(text) ? "" : text;
        }

        // 判断是否为通用实时行消息。
        function isGenericRealtimeRowMessage(message) {
            const text = String(message || "").trim();
            return [
                "存在设备暂无有效采集数据或采集失败",
                "暂无有效采集数据",
                "暂无有效采集值",
                "暂无采集记录",
                "采集失败",
                "暂无数据"
            ].includes(text);
        }

        // 左侧树只保留通道和主站；设备切换放在内容区，避免树层级与查看动作重复。
        function renderFilterTree(rows) {
            if (!filterTree) {
                return;
            }
            if (!rows.length) {
                // 左侧仍保留“全部设备”入口，便于数据恢复后筛选状态能沿用同一套事件处理。
                setHTMLIfChanged(filterTree, [
                    filterNodeHtml("all", "", "全部设备", "overview", 0, 0),
                    '<div class="realtime-empty-mini tree-empty">暂无设备数据</div>'
                ].join(""));
                applyFilterActiveState();
                return;
            }

            const channelOrder = [];
            const channels = Object.create(null);
            rows.forEach(function (row) {
                const channelID = rowKey(row, "channel");
                const masterID = rowKey(row, "master");
                if (!channels[channelID]) {
                    channels[channelID] = {
                        id: channelID,
                        label: rowLabel(row, "channel"),
                        count: 0,
                        issueCount: 0,
                        hasIssue: false,
                        masterOrder: [],
                        masters: Object.create(null)
                    };
                    channelOrder.push(channelID);
                }
                const channel = channels[channelID];
                channel.count += 1;
                if (!channel.masters[masterID]) {
                    channel.masters[masterID] = {
                        id: masterID,
                        label: rowLabel(row, "master"),
                        count: 0,
                        issueCount: 0,
                        hasIssue: false
                    };
                    channel.masterOrder.push(masterID);
                }
                const master = channel.masters[masterID];
                master.count += 1;
                const hasIssue = Boolean(row.error_message) || Boolean(row.has_status && !row.online);
                channel.hasIssue = channel.hasIssue || hasIssue;
                master.hasIssue = master.hasIssue || hasIssue;
                if (hasIssue) {
                    channel.issueCount += 1;
                    master.issueCount += 1;
                }
            });

            const parts = [filterNodeHtml("all", "", "全部设备", "overview", 0, rows.length)];
            const currentTreeKeys = Object.create(null);
            channelOrder.forEach(function (channelID) {
                const channel = channels[channelID];
                currentTreeKeys["channel:" + channel.id] = true;
                parts.push(treeBranchOpen("channel", channel.id, channel.hasIssue));
                parts.push('<div class="tree-branch-row">', treeToggleHtml("channel", channel.id, channel.hasIssue), filterNodeHtml("channel", channel.id, "通道 / " + channel.label, "channel", 0, treeCountText(channel.count, channel.issueCount), channel.label, channel.hasIssue), "</div>");
                parts.push('<div class="tree-branch-children">');
                channel.masterOrder.forEach(function (masterID) {
                    const master = channel.masters[masterID];
                    parts.push('<div class="tree-branch-row tree-depth-1">', filterNodeHtml("master", master.id, "主站 / " + master.label, "master", 0, treeCountText(master.count, master.issueCount), master.label, master.hasIssue), "</div>");
                });
                parts.push("</div></div>");
            });
            let treeStateChanged = false;
            Object.keys(realtimeTreeState).forEach(function (key) {
                if (currentTreeKeys[key]) return;
                delete realtimeTreeState[key];
                treeStateChanged = true;
            });
            if (treeStateChanged) writeTreeState(realtimeTreeStorageKey, realtimeTreeState);
            setHTMLIfChanged(filterTree, parts.join(""));
            applyFilterActiveState();
        }

        // 判断树形筛选分支是否展开。
        function treeBranchOpen(type, id, hasIssue) {
            const key = type + ":" + id;
            const expanded = realtimeTreeExpanded(type, id, hasIssue);
            return '<div class="tree-branch ' + (expanded ? "" : "is-collapsed") + '" data-tree-key="' + escapeHtml(key) + '">';
        }

        // 生成树节点展开按钮的安全 HTML。
        function treeToggleHtml(type, id, hasIssue) {
            const expanded = realtimeTreeExpanded(type, id, hasIssue);
            return '<button type="button" class="tree-toggle" data-tree-toggle="' + escapeHtml(type + ":" + id) + '" aria-label="展开或收起" aria-expanded="' + (expanded ? "true" : "false") + '"><span class="tree-toggle-chevron" aria-hidden="true"></span></button>';
        }

        // 读取实时筛选树的展开状态。
        function realtimeTreeExpanded(type, id, hasIssue) {
            if (realtimeActivePathContains(type, id)) return true;
            const key = type + ":" + id;
            if (typeof realtimeTreeState[key] === "boolean") return realtimeTreeState[key];
            return type === "channel" || hasIssue;
        }

        // 判断当前选中路径是否包含指定节点。
        function realtimeActivePathContains(type, id) {
            if (activeFilter.type === type && activeFilter.id === id) return true;
            if (activeFilter.type === "master" && type === "channel") {
                return allRows.some(function (row) {
                    return rowKey(row, "master") === activeFilter.id && rowKey(row, "channel") === id;
                });
            }
            return false;
        }

        // 格式化树节点的数量文本。
        function treeCountText(count, issueCount) {
            return issueCount > 0 ? (String(count) + " / 异常 " + String(issueCount)) : String(count);
        }

        // 筛选并更新当前选择状态。
        function filterNodeHtml(type, id, filterLabelText, icon, depth, count, visibleLabel, hasIssue) {
            const label = visibleLabel || filterLabelText;
            return [
                '<button type="button" class="tree-node tree-filter-node ',
                depth > 0 ? "tree-depth-" + depth + " " : "",
                hasIssue ? "tree-node-issue " : "",
                '" data-filter-type="', escapeHtml(type),
                '" data-filter-id="', escapeHtml(id),
                '" data-filter-label="', escapeHtml(filterLabelText),
                '">',
                '<span class="tree-node-icon" aria-hidden="true"><svg class="ui-icon"><use href="#icon-', escapeHtml(icon), '"></use></svg></span>',
                "<strong title=\"", escapeHtml(label), "\">", escapeHtml(label), "</strong>",
                (typeof count === "number" || typeof count === "string") ? "<b>" + escapeHtml(String(count)) + "</b>" : "",
                "</button>"
            ].join("");
        }

        // 应用筛选活动状态。
        function applyFilterActiveState() {
            if (!filterTree) {
                return;
            }
            filterTree.querySelectorAll("[data-filter-type]").forEach(function (node) {
                const active = node.dataset.filterType === activeFilter.type &&
                    (node.dataset.filterId || "") === activeFilter.id;
                node.classList.toggle("active", active);
            });
        }

        // 更新筛选状态。
        function updateFilterState(filteredCount) {
            applyFilterActiveState();
            renderDeviceSelector();
        }

        // 恢复实时选择。
        function restoreRealtimeSelection() {
            try {
                const stored = JSON.parse(window.localStorage.getItem(realtimeSelectionStorageKey) || "{}");
                const type = stored && stored.type;
                if (type === "master" || type === "channel") {
                    activeFilter = {
                        type: type,
                        id: String(stored.id || ""),
                        label: String(stored.label || "")
                    };
                }
                selectedDeviceID = String(stored && stored.device_id || "");
            } catch (error) {
                activeFilter = { type: "all", id: "", label: "全部设备" };
                selectedDeviceID = "";
            }
        }

        // 将实时数据筛选和展开状态保存到本地存储。
        function persistRealtimeSelection() {
            try {
                window.localStorage.setItem(realtimeSelectionStorageKey, JSON.stringify({
                    type: activeFilter.type,
                    id: activeFilter.id,
                    label: activeFilter.label,
                    device_id: selectedDeviceID
                }));
            } catch (error) {
                // 存储不可用时仍保持当前页内选择，不影响实时刷新。
            }
        }

        // 去重设备行。
        function uniqueDeviceRows(rows) {
            const seen = {};
            return rows.filter(function (row) {
                const id = rowKey(row, "device");
                if (seen[id]) return false;
                seen[id] = true;
                return true;
            });
        }

        // 获取当前主站设备。
        function currentMasterDevices() {
            if (activeFilter.type !== "master") return [];
            return uniqueDeviceRows(allRows.filter(function (row) {
                return rowKey(row, "master") === activeFilter.id;
            }));
        }

        // 校正活动选择。
        function reconcileActiveSelection() {
            if (activeFilter.type === "master") {
                const devices = currentMasterDevices();
                if (!devices.length) {
                    activeFilter = { type: "all", id: "", label: "全部设备" };
                    selectedDeviceID = "";
                } else if (!devices.some(function (row) {
                    return rowKey(row, "device") === selectedDeviceID;
                })) {
                    selectedDeviceID = rowKey(devices[0], "device");
                }
            } else if (activeFilter.type === "channel") {
                const channelExists = allRows.some(function (row) {
                    return rowKey(row, "channel") === activeFilter.id;
                });
                if (!channelExists) {
                    activeFilter = { type: "all", id: "", label: "全部设备" };
                }
                selectedDeviceID = "";
            } else {
                activeFilter = { type: "all", id: "", label: "全部设备" };
                selectedDeviceID = "";
            }
            persistRealtimeSelection();
        }

        // 渲染设备选择器。
        function renderDeviceSelector() {
            if (!deviceSelectorWrap || !deviceSelector || !currentDeviceName) return;
            const devices = currentMasterDevices();
            const selected = devices.find(function (row) {
                return rowKey(row, "device") === selectedDeviceID;
            });
            currentDeviceName.textContent = selected
                ? "· " + rowLabel(selected, "device")
                : "";
            const showSelector = devices.length > 1;
            deviceSelectorWrap.classList.toggle("is-hidden", !showSelector);
            if (!showSelector) {
                setHTMLIfChanged(deviceSelector, "");
                return;
            }
            setHTMLIfChanged(deviceSelector, devices.map(function (row) {
                const id = rowKey(row, "device");
                return '<option value="' + escapeHtml(id) + '"' +
                    (id === selectedDeviceID ? " selected" : "") + ">" +
                    escapeHtml(rowLabel(row, "device")) + "</option>";
            }).join(""));
        }

        // 更新后端状态。
        function updateBackendState(reachable) {
            const text = reachable ? "后端可达" : "后端不可达";
            const klass = reachable ? "status-ok" : "status-bad";

            if (topBackendState) {
                if (EdgeMotion) EdgeMotion.updateStatus(topBackendState, text, "pill " + klass);
                else {
                    topBackendState.textContent = text;
                    topBackendState.className = "pill " + klass;
                }
            }
        }

        // 后端不可达时保留最后成功快照，但统一降级行、点位和统计的事实状态。
        function markRealtimeDataStale(message) {
            realtimeDataStale = true;
            renderCurrentRows(table.dataset.emptyText || "暂无实时数据");
            [channelList, masterList, filterTree].forEach(function (element) {
                if (element) element.classList.add("is-stale");
            });
            if (channelSummary) channelSummary.textContent = "状态不可确认";
            if (masterSummary) masterSummary.textContent = "状态不可确认";
            if (deviceState) {
                if (EdgeMotion) EdgeMotion.updateStatus(deviceState, "状态不可确认", "pill status-neutral");
                else {
                    deviceState.textContent = "状态不可确认";
                    deviceState.className = "pill status-neutral";
                }
            }
            if (deviceOnline) setMotionText(deviceOnline, "--", "value-neutral");
            if (deviceTotal) setMotionText(deviceTotal, String(lastRows.length), "value-neutral");
            if (tableStatus) {
                tableStatus.textContent = "缓存值 · 最后更新 " + (lastSuccessfulRefreshText || "未知");
            }
            showConnectionAlert(true, message);
        }

        // 下一次成功快照到达后自动恢复原有在线、质量和统计展示。
        function clearRealtimeStalePresentation() {
            realtimeDataStale = false;
            [channelList, masterList, filterTree].forEach(function (element) {
                if (element) element.classList.remove("is-stale");
            });
        }

        // 顶部统计与设备列表使用同一份 dashboard，连接不可达时单独显示可用性状态。
        function renderDashboard(dashboard, reachable) {
            const channelListChanged = renderLayerList(channelList, dashboard.channels || [], "暂无通道配置", false);
            const masterListChanged = renderLayerList(masterList, dashboard.masters || [], "暂无主站配置", true);
            if (channelAutoScroller) channelAutoScroller.refresh(channelListChanged);
            if (masterAutoScroller) masterAutoScroller.refresh(masterListChanged);
            renderLayerSummary(channelSummary, dashboard.channel_summary || {});
            renderLayerSummary(masterSummary, dashboard.master_summary || {});

            const deviceSummary = dashboard.device_summary || {};
            if (deviceState) {
                if (EdgeMotion) EdgeMotion.updateStatus(deviceState, deviceSummary.state_text || "状态未知", "pill " + (deviceSummary.state_class || "status-neutral"));
                else {
                    deviceState.textContent = deviceSummary.state_text || "状态未知";
                    deviceState.className = "pill " + (deviceSummary.state_class || "status-neutral");
                }
            }
            if (deviceOnline) {
                setMotionText(deviceOnline, String(deviceSummary.online_count || 0), "value-neutral");
            }
            if (deviceTotal) {
                setMotionText(deviceTotal, String(deviceSummary.total_count || 0), "value-neutral");
            }
            const currentAlarmCount = Math.max(0, Number(deviceSummary.alarm_count || 0));
            if (alarmCard) alarmCard.classList.toggle("is-alarm", currentAlarmCount > 0);
            if (alarmIcon) {
                alarmIcon.classList.toggle("is-active", currentAlarmCount > 0);
                alarmIcon.classList.toggle("is-clear", currentAlarmCount === 0);
            }
            if (alarmCount) {
                setMotionText(alarmCount, String(currentAlarmCount), "value-neutral");
            }
            if (alarmState) {
                const alarmText = currentAlarmCount > 0 ? "存在告警" : "状态正常";
                const alarmClass = "pill " + (currentAlarmCount > 0 ? "status-warn" : "status-ok");
                if (EdgeMotion) EdgeMotion.updateStatus(alarmState, alarmText, alarmClass);
                else {
                    alarmState.textContent = alarmText;
                    alarmState.className = alarmClass;
                }
            }
            renderAlarmDeviceCarousel(deviceSummary);
            if (tableStatus) {
                tableStatus.textContent = dashboard.table_status_text || (reachable ? "实时刷新正常" : "后端不可达");
            }
        }

        // 渲染层级摘要。
        function renderLayerSummary(container, summary) {
            if (!container) return;
            container.textContent = String(summary.online_count || 0) + " / " + String(summary.total_count || 0) + " 在线";
        }

        // 渲染层级列表。
        function renderLayerList(container, items, emptyText, showDeviceCounts) {
            if (!container) {
                return false;
            }
            if (!items.length) {
                return setHTMLIfChanged(container, '<div class="realtime-empty-mini">' + escapeHtml(emptyText) + "</div>");
            }
            return setHTMLIfChanged(container, items.map(function (item) {
                return [
                    '<div class="realtime-layer-item">',
                    '<span class="layer-dot ', escapeHtml(item.state_class || "status-neutral"), '"></span>',
                    "<div><strong>", escapeHtml(item.name || "-"), "</strong>", showDeviceCounts ? "<small>" + escapeHtml(item.state_text || "状态未知") + "</small>" : "", "</div>",
                    showDeviceCounts
                        ? "<b>" + String(item.online_count || 0) + "/" + String(item.total_count || 0) + " 在线</b>"
                        : '<b class="realtime-layer-state ' + escapeHtml(item.state_class || "status-neutral") + '" title="' + escapeHtml(item.error_message || item.state_text || "状态未知") + '">' + escapeHtml(item.state_text || "状态未知") + "</b>",
                    "</div>"
                ].join("");
            }).join(""));
        }

        // 渲染告警设备轮播。
        function renderAlarmDeviceCarousel(deviceSummary) {
            if (!alarmDeviceViewport || !alarmDeviceTrack) return;
            const names = Array.isArray(deviceSummary.alarm_device_names)
                ? deviceSummary.alarm_device_names.map(function (name) { return String(name || "").trim(); }).filter(Boolean)
                : [];
            const signature = JSON.stringify(names);
            currentAlarmDeviceNames = names;
            alarmDeviceViewport.classList.toggle("is-hidden", names.length === 0);
            alarmDeviceViewport.classList.toggle("is-carousel", names.length > 3);
            if (signature === alarmCarouselSignature) {
                if (!isPageHidden()) startAlarmCarousel();
                return;
            }
            alarmCarouselSignature = signature;
            stopAlarmCarousel();
            alarmCarouselIndex = 0;
            alarmDeviceTrack.style.transform = "translateY(0)";
            const groups = [];
            for (let index = 0; index < names.length; index += 3) {
                groups.push(names.slice(index, index + 3));
            }
            setHTMLIfChanged(alarmDeviceTrack, groups.map(function (group) {
                return '<div class="realtime-alarm-device-group">' + group.map(function (name) {
                    return renderAlarmDeviceItem(name);
                }).join("") + "</div>";
            }).join(""));
            startAlarmCarousel();
        }

        // 填充告警设备项目。
        function hydrateAlarmDeviceItems() {
            if (!alarmDeviceTrack) return;
            alarmDeviceTrack.querySelectorAll("[data-alarm-name]").forEach(function (item) {
                const rawName = item.dataset.alarmName || item.textContent || "";
                item.outerHTML = renderAlarmDeviceItem(rawName);
            });
        }

        // 渲染告警设备点位。
        function renderAlarmDeviceItem(rawName) {
            const entry = alarmDisplayEntry(rawName);
            const title = entry.pointName ? (entry.deviceName + " / " + entry.pointName) : entry.deviceName;
            return [
                '<span class="realtime-alarm-device-item" data-alarm-name="', escapeHtml(rawName),
                '" title="', escapeHtml(title), '">',
                "<strong>", escapeHtml(entry.deviceName), "</strong>",
                entry.pointName ? "<small>" + escapeHtml(entry.pointName) + "</small>" : "",
                "</span>"
            ].join("");
        }

        // 将活动告警转换为轮播展示项。
        function alarmDisplayEntry(rawName) {
            const text = String(rawName || "").trim() || "未知设备";
            const separated = text.split(/\s*[·•]\s*/, 2);
            const deviceName = String(separated[0] || "").trim() || text;
            if (separated.length > 1 && String(separated[1] || "").trim()) {
                return { deviceName: deviceName, pointName: String(separated[1]).trim() };
            }
            const row = allRows.find(function (item) {
                return String(item.device_name || "").trim() === deviceName ||
                    String(item.device_id || "").trim() === deviceName;
            });
            if (!row || !row.diagnosis || row.diagnosis.error_code !== "data_alarm") {
                return { deviceName: deviceName, pointName: "" };
            }
            const message = String(row.error_message || row.diagnosis.message || "").trim();
            const point = (Array.isArray(row.summary_points) ? row.summary_points : []).find(function (item) {
                const name = String(item.name || "").trim();
                return name && (message === name || message.startsWith(name + " "));
            });
            const targetName = String(row.diagnosis.target_name || "").trim();
            const messagePointMatch = /^(.+?)\s+[-+]?(?:\d|\.)/.exec(message);
            const messagePointName = messagePointMatch ? String(messagePointMatch[1] || "").trim() : "";
            return {
                deviceName: deviceName,
                pointName: point ? String(point.name || "").trim() :
                    (targetName && targetName !== deviceName ? targetName : messagePointName)
            };
        }

        // 告警设备较多时才启动轮播，隐藏页或无告警时及时释放定时器。
        function startAlarmCarousel() {
            if (alarmCarouselTimer || reduceAlarmMotion || isPageHidden() || currentAlarmDeviceNames.length <= 3) return;
            alarmCarouselTimer = scope.setInterval(function () {
                if (isPageHidden() || currentAlarmDeviceNames.length <= 3) return;
                const groupCount = Math.ceil(currentAlarmDeviceNames.length / 3);
                alarmCarouselIndex = (alarmCarouselIndex + 1) % groupCount;
                const distance = alarmDeviceViewport.clientHeight || 66;
                alarmDeviceTrack.style.transform = "translateY(" + String(-alarmCarouselIndex * distance) + "px)";
            }, 5000);
        }

        // 停止告警轮播。
        function stopAlarmCarousel() {
            if (!alarmCarouselTimer) return;
            window.clearInterval(alarmCarouselTimer);
            alarmCarouselTimer = null;
        }

        // 显示连接告警提示。
        function showConnectionAlert(show, message) {
            if (!connectionAlert) {
                return;
            }
            connectionAlert.classList.toggle("hidden", !show);
            const text = connectionAlert.querySelector("p");
            if (text) {
                text.textContent = message || "后端服务未连接";
            }
        }

        // 读取表格行。
        function readTableRows() {
            return Array.from(tbody.querySelectorAll("tr")).filter(function (row) {
                return !row.querySelector(".empty-cell");
            }).map(function (row) {
                return {
                    has_status: !row.classList.contains("row-muted"),
                    online: row.classList.contains("row-live")
                };
            });
        }

        // 统计在线行。
        function countOnlineRows(rows) {
            return rows.filter(function (row) { return row.has_status && row.online; }).length;
        }

        // 缓存最近一次由本控制器生成的标记；数据未变化时不触发 DOM 解析和节点替换。
        function setHTMLIfChanged(element, markup) {
            if (!element || element.__edgeRealtimeMarkup === markup) {
                return false;
            }
            if (element.__edgeRealtimeMarkup === undefined && element.innerHTML === markup) {
                element.__edgeRealtimeMarkup = markup;
                return false;
            }
            if (EdgeMotion && EdgeMotion.cancelWithin) EdgeMotion.cancelWithin(element);
            element.innerHTML = markup;
            element.__edgeRealtimeMarkup = markup;
            return true;
        }

        // 行结构与点位 key 稳定时只更新数值、单位和质量节点，标题、分组和卡片
        // 保留原 DOM；结构发生变化时才回退为整块替换。
        function reconcileRealtimeReading(element, markup) {
            if (!element || element.__edgeRealtimeMarkup === markup) return false;

            const nextShell = document.createElement("div");
            nextShell.innerHTML = markup;
            const currentRoot = element.firstElementChild;
            const nextRoot = nextShell.firstElementChild;
            if (!currentRoot || !nextRoot || currentRoot.className !== nextRoot.className) {
                return setHTMLIfChanged(element, markup);
            }

            const currentValues = Array.from(element.querySelectorAll("[data-realtime-value-key]"));
            const nextValues = Array.from(nextShell.querySelectorAll("[data-realtime-value-key]"));
            const currentPoints = Array.from(element.querySelectorAll("[data-realtime-point-key]"));
            const nextPoints = Array.from(nextShell.querySelectorAll("[data-realtime-point-key]"));
            const currentGroups = Array.from(element.querySelectorAll("[data-realtime-group-key]"));
            const nextGroups = Array.from(nextShell.querySelectorAll("[data-realtime-group-key]"));
            const currentGroupTitles = Array.from(element.querySelectorAll(".metric-group-title"));
            const nextGroupTitles = Array.from(nextShell.querySelectorAll(".metric-group-title"));
            const sameKeys = currentValues.length === nextValues.length &&
                currentValues.every(function (node, index) {
                    return node.dataset.realtimeValueKey === nextValues[index].dataset.realtimeValueKey;
                });
            const samePointKeys = currentPoints.length === nextPoints.length &&
                currentPoints.every(function (node, index) {
                    return node.dataset.realtimePointKey === nextPoints[index].dataset.realtimePointKey;
                });
            const sameGroups = currentGroups.length === nextGroups.length &&
                currentGroups.every(function (group, index) {
                    return group.dataset.realtimeGroupKey === nextGroups[index].dataset.realtimeGroupKey;
                });
            const sameGroupMembership = currentPoints.every(function (point, index) {
                const currentGroup = point.closest("[data-realtime-group-key]");
                const nextGroup = nextPoints[index] && nextPoints[index].closest("[data-realtime-group-key]");
                return String(currentGroup && currentGroup.dataset.realtimeGroupKey || "") ===
                    String(nextGroup && nextGroup.dataset.realtimeGroupKey || "");
            });
            if (!sameKeys || !samePointKeys || !sameGroups || !sameGroupMembership ||
                    currentGroupTitles.length !== nextGroupTitles.length) {
                return setHTMLIfChanged(element, markup);
            }

            for (let index = 0; index < currentValues.length; index += 1) {
                const currentReading = currentValues[index].closest(".metric-chip-reading");
                const nextReading = nextValues[index].closest(".metric-chip-reading");
                const currentUnit = currentReading && currentReading.querySelector(".metric-chip-unit");
                const nextUnit = nextReading && nextReading.querySelector(".metric-chip-unit");
                if (Boolean(currentUnit) !== Boolean(nextUnit)) {
                    return setHTMLIfChanged(element, markup);
                }
            }

            currentGroupTitles.forEach(function (group, index) {
                if (group.textContent !== nextGroupTitles[index].textContent) {
                    group.textContent = nextGroupTitles[index].textContent;
                }
            });
            currentPoints.forEach(function (point, index) {
                const nextPoint = nextPoints[index];
                const transientClasses = Array.from(point.classList).filter(function (className) {
                    return className.indexOf("motion-") === 0;
                });
                point.className = nextPoint.className;
                transientClasses.forEach(function (className) { point.classList.add(className); });
                point.setAttribute("data-quality-state", nextPoint.getAttribute("data-quality-state") || "");
                point.setAttribute("title", nextPoint.getAttribute("title") || "");
                const label = point.querySelector("b");
                const nextLabel = nextPoint.querySelector("b");
                if (label && nextLabel && label.textContent !== nextLabel.textContent) {
                    label.textContent = nextLabel.textContent;
                }
            });
            currentValues.forEach(function (value, index) {
                const nextValue = nextValues[index];
                if (value.textContent !== nextValue.textContent) value.textContent = nextValue.textContent;
                const currentReading = value.closest(".metric-chip-reading");
                const nextReading = nextValue.closest(".metric-chip-reading");
                const currentUnit = currentReading && currentReading.querySelector(".metric-chip-unit");
                const nextUnit = nextReading && nextReading.querySelector(".metric-chip-unit");
                if (currentUnit && nextUnit && currentUnit.textContent !== nextUnit.textContent) {
                    currentUnit.textContent = nextUnit.textContent;
                }
                const transientClasses = Array.from(value.classList).filter(function (className) {
                    return className.indexOf("motion-") === 0;
                });
                value.className = nextValue.className;
                transientClasses.forEach(function (className) { value.classList.add(className); });
            });
            element.__edgeRealtimeMarkup = markup;
            return true;
        }

        // 10.1 寸板端只在列表真实溢出时复制一轮只读镜像并低速连续位移；
        // 到达镜像起点后等距归位，因此视觉上首尾相接，不出现逐项跳转或反向折返。
        function createSmoothLoopScroller(container, initialDelay) {
            if (!container) return null;
            const pixelsPerSecond = 14;
            const resumeDelay = 3200;
            let scheduleTimer = null;
            let frameID = null;
            let lastFrameTime = 0;
            let scrollPosition = container.scrollTop;
            let loopSpan = 0;
            let enabled = false;
            let hovering = false;
            let pressing = false;
            let touching = false;
            let focused = false;
            let destroyed = false;
            let itemCount = originalItems().length;

            function originalItems() {
                return Array.from(container.querySelectorAll(
                    ".realtime-layer-item:not([data-auto-scroll-clone])"
                ));
            }

            function clearScheduleTimer() {
                if (scheduleTimer !== null) {
                    window.clearTimeout(scheduleTimer);
                    scheduleTimer = null;
                }
            }

            function removeLoopClones() {
                if (loopSpan > 1 && container.scrollTop >= loopSpan) {
                    container.scrollTop %= loopSpan;
                }
                container.querySelectorAll("[data-auto-scroll-clone]").forEach(function (clone) {
                    if (clone.parentNode) clone.parentNode.removeChild(clone);
                });
                loopSpan = 0;
            }

            function cancelScrollFrame() {
                if (frameID !== null) {
                    window.cancelAnimationFrame(frameID);
                    frameID = null;
                }
                lastFrameTime = 0;
                container.classList.remove("is-auto-scrolling");
                removeLoopClones();
                scrollPosition = container.scrollTop;
            }

            function isInteractionHeld() {
                return hovering || pressing || touching || focused;
            }

            function maxScrollTop() {
                return Math.max(0, container.scrollHeight - container.clientHeight);
            }

            function hasOverflow() {
                return container.clientHeight > 0 && maxScrollTop() > 1;
            }

            function isBoardViewport() {
                return window.innerWidth <= 1366 && window.innerHeight <= 900;
            }

            function prefersReducedMotion() {
                return Boolean(window.matchMedia &&
                    window.matchMedia("(prefers-reduced-motion: reduce)").matches);
            }

            function canRun() {
                return !destroyed && enabled && scope.isActive() && !isPageHidden() &&
                    !isInteractionHeld() && hasOverflow() && isBoardViewport() &&
                    !prefersReducedMotion() && typeof window.requestAnimationFrame === "function";
            }

            function prepareLoopClones() {
                removeLoopClones();
                const items = originalItems();
                if (!items.length || !hasOverflow()) return false;
                items.forEach(function (item) {
                    const clone = item.cloneNode(true);
                    clone.setAttribute("data-auto-scroll-clone", "true");
                    clone.setAttribute("aria-hidden", "true");
                    container.appendChild(clone);
                });
                const firstClone = container.querySelector("[data-auto-scroll-clone]");
                loopSpan = firstClone
                    ? firstClone.getBoundingClientRect().top - items[0].getBoundingClientRect().top
                    : 0;
                return loopSpan > 1;
            }

            function scheduleStart(delay) {
                clearScheduleTimer();
                if (!canRun()) return;
                scheduleTimer = scope.setTimeout(function () {
                    scheduleTimer = null;
                    if (!canRun()) return;
                    if (!prepareLoopClones()) return;
                    scrollPosition = container.scrollTop;
                    lastFrameTime = 0;
                    container.classList.add("is-auto-scrolling");
                    frameID = window.requestAnimationFrame(runFrame);
                }, delay);
            }

            function runFrame(frameTime) {
                frameID = null;
                if (!canRun()) {
                    cancelScrollFrame();
                    return;
                }
                if (lastFrameTime === 0) {
                    lastFrameTime = frameTime;
                } else {
                    const elapsed = Math.min(64, Math.max(0, frameTime - lastFrameTime));
                    lastFrameTime = frameTime;
                    scrollPosition += pixelsPerSecond * elapsed / 1000;
                    if (loopSpan > 1 && scrollPosition >= loopSpan) {
                        scrollPosition %= loopSpan;
                    }
                    container.scrollTop = scrollPosition;
                }
                frameID = window.requestAnimationFrame(runFrame);
            }

            function pause() {
                clearScheduleTimer();
                cancelScrollFrame();
            }

            function scheduleResume() {
                pause();
                if (!enabled || isInteractionHeld() || isPageHidden()) return;
                scheduleTimer = scope.setTimeout(function () {
                    scheduleTimer = null;
                    refresh();
                }, resumeDelay);
            }

            function refresh(contentChanged) {
                if (destroyed) return;
                if (contentChanged) pause();
                const nextItemCount = originalItems().length;
                if (nextItemCount !== itemCount) {
                    itemCount = nextItemCount;
                    pause();
                }
                const maximum = maxScrollTop();
                if (container.clientHeight <= 0 || maximum <= 1) {
                    enabled = false;
                    pause();
                    if (container.scrollTop !== 0) container.scrollTop = 0;
                    return;
                }
                if (container.scrollTop > maximum) {
                    cancelScrollFrame();
                    container.scrollTop = maximum;
                }
                scrollPosition = container.scrollTop;
                if (!enabled) {
                    enabled = true;
                    scheduleStart(initialDelay);
                    return;
                }
                if (scheduleTimer === null && frameID === null) {
                    scheduleStart(180);
                }
            }

            function transientInteraction() {
                pause();
                scheduleResume();
            }

            scope.listen(container, "wheel", transientInteraction, { passive: true });
            scope.listen(container, "keydown", transientInteraction);
            scope.listen(container, "mouseenter", function () {
                hovering = true;
                pause();
            });
            scope.listen(container, "mouseleave", function () {
                hovering = false;
                scheduleResume();
            });
            scope.listen(container, "mousedown", function () {
                pressing = true;
                pause();
            });
            scope.listen(window, "mouseup", function () {
                if (!pressing) return;
                pressing = false;
                scheduleResume();
            });
            scope.listen(container, "touchstart", function () {
                touching = true;
                pause();
            }, { passive: true });
            scope.listen(window, "touchend", function () {
                if (!touching) return;
                touching = false;
                scheduleResume();
            }, { passive: true });
            scope.listen(window, "touchcancel", function () {
                if (!touching) return;
                touching = false;
                scheduleResume();
            }, { passive: true });
            scope.listen(container, "focusin", function () {
                focused = true;
                pause();
            });
            scope.listen(container, "focusout", function (event) {
                if (event.relatedTarget && container.contains(event.relatedTarget)) return;
                focused = false;
                scheduleResume();
            });

            const controller = {
                refresh: refresh,
                pause: pause,
                destroy: function () {
                    destroyed = true;
                    enabled = false;
                    clearScheduleTimer();
                    cancelScrollFrame();
                }
            };
            controller.refresh();
            return controller;
        }

        // 仅在展示值实际变化时反馈，避免定时刷新让未变化卡片反复闪动。
        function setMotionText(element, value, kind) {
            if (!element) return;
            if (EdgeMotion) EdgeMotion.updateText(element, value, kind);
            else if (element.textContent !== value) element.textContent = value;
        }

        reconnectButtons.forEach(function (button) {
            scope.listen(button, "click", function () {
                refreshRealtime();
            });
        });
        if (refreshButton) scope.listen(refreshButton, "click", refreshRealtime);
        if (refreshIntervalSelector) scope.listen(refreshIntervalSelector, "change", function () {
            const requested = Number(refreshIntervalSelector.value);
            refreshIntervalMS = allowedRefreshIntervals.includes(requested) ? requested : 10000;
            refreshIntervalSelector.value = String(refreshIntervalMS);
            try { window.localStorage.setItem(refreshPreferenceKey, String(refreshIntervalMS)); } catch (_) {}
            startRealtimeRefreshTimer();
            if (refreshIntervalMS > 0) refreshRealtime();
        });

        if (filterTree) {
            scope.listen(filterTree, "click", function (event) {
                const toggle = event.target.closest("[data-tree-toggle]");
                if (toggle && filterTree.contains(toggle)) {
                    const branch = toggle.closest("[data-tree-key]");
                    if (branch) {
                        const collapsed = branch.classList.toggle("is-collapsed");
                        toggle.setAttribute("aria-expanded", collapsed ? "false" : "true");
                        realtimeTreeState[toggle.dataset.treeToggle] = !collapsed;
                        writeTreeState(realtimeTreeStorageKey, realtimeTreeState);
                        if (!collapsed && EdgeMotion) {
                            const children = branch.querySelector(".tree-branch-children");
                            if (children) EdgeMotion.reveal(children);
                        }
                    }
                    return;
                }
                const node = event.target.closest("[data-filter-type]");
                if (!node || !filterTree.contains(node)) {
                    return;
                }
                activeFilter = {
                    type: node.dataset.filterType || "all",
                    id: node.dataset.filterId || "",
                    label: node.dataset.filterLabel || "全部设备"
                };
                selectedDeviceID = "";
                reconcileActiveSelection();
                renderCurrentRows(table.dataset.emptyText || "暂无实时数据");
            });
        }

        if (deviceSelector) {
            scope.listen(deviceSelector, "change", function () {
                if (activeFilter.type !== "master") return;
                const nextID = deviceSelector.value || "";
                const valid = currentMasterDevices().some(function (row) {
                    return rowKey(row, "device") === nextID;
                });
                if (!valid) {
                    reconcileActiveSelection();
                    renderCurrentRows(table.dataset.emptyText || "暂无实时数据");
                    return;
                }
                selectedDeviceID = nextID;
                persistRealtimeSelection();
                renderCurrentRows(table.dataset.emptyText || "暂无实时数据");
            });
        }

        function stopRealtimeRefreshTimer() {
            if (realtimeRefreshTimer !== null) {
                window.clearInterval(realtimeRefreshTimer);
                realtimeRefreshTimer = null;
            }
        }

        function startRealtimeRefreshTimer() {
            stopRealtimeRefreshTimer();
            if (!isPageHidden() && refreshIntervalMS > 0) {
                realtimeRefreshTimer = scope.setInterval(refreshRealtime, refreshIntervalMS);
            }
        }

        function pauseLayerAutoScrollers() {
            if (channelAutoScroller) channelAutoScroller.pause();
            if (masterAutoScroller) masterAutoScroller.pause();
        }

        function refreshLayerAutoScrollers() {
            if (channelAutoScroller) channelAutoScroller.refresh();
            if (masterAutoScroller) masterAutoScroller.refresh();
        }

        scope.listen(window, "resize", function () {
            if (autoScrollResizeTimer !== null) window.clearTimeout(autoScrollResizeTimer);
            autoScrollResizeTimer = scope.setTimeout(function () {
                autoScrollResizeTimer = null;
                refreshLayerAutoScrollers();
            }, 120);
        });
        scope.listen(window, "blur", pauseLayerAutoScrollers);
        scope.listen(window, "focus", refreshLayerAutoScrollers);

        scope.onVisibilityChange(function () {
            if (isPageHidden()) {
                stopAlarmCarousel();
                stopRealtimeRefreshTimer();
                pauseLayerAutoScrollers();
            } else {
                startAlarmCarousel();
                if (refreshIntervalMS > 0) refreshRealtime();
                startRealtimeRefreshTimer();
                refreshLayerAutoScrollers();
            }
        });

        // 首屏已经包含同一后端快照，不再在脚本初始化后立即重复请求。
        startRealtimeRefreshTimer();
        scope.listen(window, "pagehide", function () {
            stopAlarmCarousel();
            stopRealtimeRefreshTimer();
            pauseLayerAutoScrollers();
        });
        scope.listen(window, "pageshow", function (event) {
            if (event.persisted) {
                if (refreshIntervalMS > 0) refreshRealtime();
                startRealtimeRefreshTimer();
                startAlarmCarousel();
                refreshLayerAutoScrollers();
            }
        });
        scope.onDispose(function () {
            stopAlarmCarousel();
            stopRealtimeRefreshTimer();
            if (autoScrollResizeTimer !== null) {
                window.clearTimeout(autoScrollResizeTimer);
                autoScrollResizeTimer = null;
            }
            if (channelAutoScroller) channelAutoScroller.destroy();
            if (masterAutoScroller) masterAutoScroller.destroy();
        });
    }

    EdgeApp.registerPageController("realtime", ["realtime"], { mount: mount });
})();
