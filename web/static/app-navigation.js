// 同源页面导航器：点击后先提交导航状态和轻量页面结构，
// 完整 HTML 与页面控制器随后接管，避免网络和同步初始化阻塞首个视觉帧。
(function () {
    "use strict";

    var EdgeApp = window.EdgeApp;
    if (!EdgeApp || !window.fetch || !window.DOMParser || !window.history) return;

    var SOFT_PATHS = {
        "/overview": "overview",
        "/realtime": "realtime",
        "/collection": "collection",
        "/history": "history",
        "/events": "events",
        "/settings": "settings",
        "/settings/mqtt": "settings",
        "/settings/device-types": "settings",
        "/settings/device-types/editor": "settings",
        "/settings/modbus-server": "settings",
        "/operations": "operations",
        "/operations/users": "operations",
		"/upgrade-wait": "operations",
        "/device-history": "history"
    };
    var PAGE_LABELS = {
        overview: "系统概览",
        realtime: "实时监控",
        collection: "采集管理",
        history: "历史数据",
        events: "事件与告警",
        settings: "系统设置",
        operations: "运维管理"
    };
    var controllers = {};
    var controllerLoads = {};
    var activeControllers = {};
    var navigationVersion = 0;
    var navigationBusy = false;
    var navigationRequest = null;
    var navigationTarget = "";
    var lastMetrics = null;

    function now() {
        return window.performance && typeof window.performance.now === "function"
            ? window.performance.now() : Date.now();
    }

    function normalizedPath(pathname) {
        var path = String(pathname || "/");
        if (path.length > 1 && path.charAt(path.length - 1) === "/") {
            path = path.slice(0, -1);
        }
        return path;
    }

    function pageKeyForPath(pathname) {
        var path = normalizedPath(pathname);
        if (SOFT_PATHS[path]) return SOFT_PATHS[path];
        if (/^\/collection\/channels\/[^/]+\/communication-traces$/.test(path)) return "collection";
        return "";
    }

    function currentPageKey() {
        return String(document.body && document.body.dataset.pageKey || "");
    }

    function staleNavigationError() {
        var error = new Error("stale page task");
        error.name = "EdgeStalePageError";
        error.edgeStale = true;
        return error;
    }

    function PageScope(controllerKey, version) {
        this.controllerKey = controllerKey;
        this.version = version;
        this.disposed = false;
        this.disposers = [];
        this.abortControllers = [];
    }

    PageScope.prototype.isActive = function () {
        return !this.disposed && this.version === navigationVersion &&
            activeControllers[this.controllerKey] &&
            activeControllers[this.controllerKey].scope === this;
    };

    PageScope.prototype.onDispose = function (callback) {
        if (typeof callback !== "function") return callback;
        if (this.disposed) callback();
        else this.disposers.push(callback);
        return callback;
    };

    PageScope.prototype.listen = function (target, type, listener, options) {
        if (!target || typeof target.addEventListener !== "function" || typeof listener !== "function") {
            return listener;
        }
        target.addEventListener(type, listener, options);
        this.onDispose(function () {
            target.removeEventListener(type, listener, options);
        });
        return listener;
    };

    PageScope.prototype.onVisibilityChange = function (listener) {
        if ("hidden" in document) return this.listen(document, "visibilitychange", listener);
        if ("webkitHidden" in document) return this.listen(document, "webkitvisibilitychange", listener);
        return listener;
    };

    PageScope.prototype.setTimeout = function (callback, delay) {
        var scope = this;
        var disposer;
        var timer = window.setTimeout(function () {
            var index = scope.disposers.indexOf(disposer);
            if (index >= 0) scope.disposers.splice(index, 1);
            if (scope.isActive()) callback();
        }, delay);
        disposer = function () { window.clearTimeout(timer); };
        this.onDispose(disposer);
        return timer;
    };

    PageScope.prototype.setInterval = function (callback, delay) {
        var scope = this;
        var timer = window.setInterval(function () {
            if (scope.isActive()) callback();
        }, delay);
        this.onDispose(function () { window.clearInterval(timer); });
        return timer;
    };

    PageScope.prototype.fetch = function (url, options) {
        var scope = this;
        if (!scope.isActive()) return Promise.reject(staleNavigationError());
        var requestOptions = Object.assign({}, options || {});
        var controller = null;
        if (window.AbortController && !requestOptions.signal) {
            controller = new window.AbortController();
            requestOptions.signal = controller.signal;
            scope.abortControllers.push(controller);
        }
        function releaseController() {
            if (!controller) return;
            var index = scope.abortControllers.indexOf(controller);
            if (index >= 0) scope.abortControllers.splice(index, 1);
            controller = null;
        }
        return window.fetch(url, requestOptions).then(
            function (response) {
                if (!scope.isActive()) throw staleNavigationError();
                ["arrayBuffer", "blob", "formData", "json", "text"].forEach(function (methodName) {
                    var readBody = response && response[methodName];
                    if (typeof readBody !== "function") return;
                    try {
                        response[methodName] = function () {
                            var bodyPromise;
                            try {
                                bodyPromise = readBody.apply(response, arguments);
                            } catch (error) {
                                releaseController();
                                if (!scope.isActive()) throw staleNavigationError();
                                throw error;
                            }
                            return Promise.resolve(bodyPromise).then(
                                function (body) {
                                    releaseController();
                                    if (!scope.isActive()) throw staleNavigationError();
                                    return body;
                                },
                                function (error) {
                                    releaseController();
                                    if (!scope.isActive()) throw staleNavigationError();
                                    throw error;
                                }
                            );
                        };
                    } catch (_error) {
                        // Response 方法不可覆写时保留 controller，随 scope.dispose 统一中止。
                    }
                });
                return response;
            },
            function (error) {
                releaseController();
                if (!scope.isActive()) throw staleNavigationError();
                throw error;
            }
        );
    };

    PageScope.prototype.csrfFetch = function (url, options) {
        var scope = this;
        if (!scope.isActive()) return Promise.reject(staleNavigationError());
        return EdgeApp.csrfFetch(url, options, function (requestURL, requestOptions) {
            return scope.fetch(requestURL, requestOptions);
        });
    };

    PageScope.prototype.dispose = function () {
        if (this.disposed) return;
        this.disposed = true;
        this.abortControllers.forEach(function (controller) {
            try { controller.abort(); } catch (_error) { /* older implementations may throw */ }
        });
        this.abortControllers = [];
        while (this.disposers.length) {
            try { this.disposers.pop()(); } catch (_error) { /* cleanup must continue */ }
        }
    };

    function registerPageController(key, pageKeys, controller) {
        if (!key || !controller || typeof controller.mount !== "function") return;
        controllers[key] = {
            pageKeys: Array.isArray(pageKeys) ? pageKeys.slice() : [String(pageKeys || key)],
            mount: controller.mount,
            unmount: typeof controller.unmount === "function" ? controller.unmount : null
        };
        if (!navigationBusy) activatePageControllers();
    }

    function controllerMatchesPage(controller, pageKey) {
        return controller.pageKeys.indexOf(pageKey) >= 0;
    }

    function activatePageControllers() {
        var pageKey = currentPageKey();
        Object.keys(controllers).forEach(function (key) {
            var controller = controllers[key];
            if (!controllerMatchesPage(controller, pageKey) || activeControllers[key]) return;
            var scope = new PageScope(key, navigationVersion);
            activeControllers[key] = { controller: controller, scope: scope };
            try {
                controller.mount(scope);
            } catch (error) {
                scope.dispose();
                delete activeControllers[key];
                window.setTimeout(function () { throw error; }, 0);
            }
        });
    }

    function deactivatePageControllers() {
        Object.keys(activeControllers).forEach(function (key) {
            var active = activeControllers[key];
            if (active.controller.unmount) {
                try { active.controller.unmount(active.scope); } catch (_error) { /* scope still disposes */ }
            }
            active.scope.dispose();
            delete activeControllers[key];
        });
    }

    function updatePrimaryNavigation(pageKey, targetURL) {
        document.querySelectorAll(".side-nav .nav-item, .global-nav-grid .global-nav-item").forEach(function (item) {
            var href = item.getAttribute("href") || "";
            var active = false;
            try {
                active = pageKeyForPath(new URL(href, window.location.href).pathname) === pageKey;
            } catch (_error) {
                active = false;
            }
            item.classList.toggle("active", active);
            item.classList.toggle("is-navigation-target", active);
            if (active) item.setAttribute("aria-current", "page");
            else item.removeAttribute("aria-current");
        });
        if (targetURL) {
            var accountRedirect = document.querySelector("#account-password-modal input[name='redirect']");
            if (accountRedirect) accountRedirect.value = targetURL.pathname + targetURL.search;
        }
    }

    function updatePendingPageContext(pageKey) {
        var activeItem = document.querySelector(".side-nav .nav-item.active");
        var context = document.querySelector(".topbar-page-context");
        if (!activeItem || !context || !pageKey) return;
        var sourceLabel = activeItem.querySelector(".nav-label");
        var sourceDescription = activeItem.querySelector(".nav-desc");
        var heading = context.querySelector("h1");
        var description = context.querySelector("p");
        var sourceIcon = activeItem.querySelector("use");
        var targetIcon = context.querySelector("use");
        if (heading && sourceLabel) heading.textContent = sourceLabel.textContent;
        if (description && sourceDescription) description.textContent = sourceDescription.textContent;
        if (sourceIcon && targetIcon) {
            var href = sourceIcon.getAttribute("href") || sourceIcon.getAttribute("xlink:href");
            if (href) targetIcon.setAttribute("href", href);
        }
    }

    function markLocalNavigationTarget(targetURL) {
        document.querySelectorAll(".history-period-tab, .subpage-back-button, [data-navigation-link]").forEach(function (item) {
            var matches = false;
            try {
                matches = new URL(item.getAttribute("href") || "", window.location.href).href === targetURL.href;
            } catch (_error) {
                matches = false;
            }
            item.classList.toggle("is-navigation-target", matches);
        });
    }

    function markNavigationPending(targetURL, pageKey, sameSection) {
        var main = document.querySelector(".main-shell");
        var content = document.querySelector(".content-stack");
        updatePrimaryNavigation(pageKey || currentPageKey(), targetURL);
        updatePendingPageContext(pageKey || currentPageKey());
        markLocalNavigationTarget(targetURL);
        if (main) {
            main.classList.add("page-navigation-pending");
            main.classList.toggle("page-local-navigation-pending", Boolean(sameSection));
        }
        if (content) {
            if (window.EdgeMotion && window.EdgeMotion.beginNavigation) {
                window.EdgeMotion.beginNavigation(content);
            } else if (window.EdgeMotion && window.EdgeMotion.cancelWithin) {
                window.EdgeMotion.cancelWithin(content);
            } else if (window.EdgeMotion && window.EdgeMotion.cancel) {
                window.EdgeMotion.cancel(content, "motion-page-enter");
            }
            content.setAttribute("aria-busy", "true");
        }
    }

    function clearNavigationPending() {
        var main = document.querySelector(".main-shell");
        var content = document.querySelector(".content-stack");
        if (main) {
            main.classList.remove(
                "page-navigation-pending",
                "page-local-navigation-pending",
                "page-navigation-shell-visible"
            );
        }
        if (content) content.removeAttribute("aria-busy");
        document.querySelectorAll(".is-navigation-target").forEach(function (item) {
            item.classList.remove("is-navigation-target");
        });
    }

    function showNavigationSkeleton(pageKey) {
        var content = document.querySelector(".content-stack");
        var main = document.querySelector(".main-shell");
        if (!content || !main) return false;

        var shell = document.createElement("section");
        shell.className = "page-loading-shell";
        shell.setAttribute("data-navigation-skeleton", "");
        shell.setAttribute("role", "status");
        shell.setAttribute("aria-live", "polite");

        var header = document.createElement("header");
        header.className = "page-loading-head tech-frame";
        var copy = document.createElement("div");
        var title = document.createElement("strong");
        title.textContent = "正在打开" + (PAGE_LABELS[pageKey] || "页面");
        var description = document.createElement("span");
        description.textContent = "页面结构已就绪，数据加载中";
        copy.appendChild(title);
        copy.appendChild(description);
        var state = document.createElement("span");
        state.className = "pill status-neutral";
        state.textContent = "加载中";
        header.appendChild(copy);
        header.appendChild(state);

        var grid = document.createElement("div");
        grid.className = "page-loading-grid";
        grid.setAttribute("aria-hidden", "true");
        for (var index = 0; index < 3; index += 1) {
            var card = document.createElement("div");
            card.className = "page-loading-card tech-frame";
            var heading = document.createElement("span");
            heading.className = "page-loading-line page-loading-line-title";
            var line = document.createElement("span");
            line.className = "page-loading-line";
            var shortLine = document.createElement("span");
            shortLine.className = "page-loading-line page-loading-line-short";
            card.appendChild(heading);
            card.appendChild(line);
            card.appendChild(shortLine);
            grid.appendChild(card);
        }
        shell.appendChild(header);
        shell.appendChild(grid);
        replaceContents(content, shell);
        main.classList.add("page-navigation-shell-visible");
        return true;
    }

    function afterNextPaint(callback) {
        if (!window.requestAnimationFrame) {
            window.setTimeout(callback, 0);
            return;
        }
        window.requestAnimationFrame(function () {
            window.setTimeout(callback, 0);
        });
    }

    function enterCommittedContent(content, sameSection) {
        if (!window.EdgeMotion) return;
        if (sameSection && window.EdgeMotion.enterDataRegion) {
            var regions = content.querySelectorAll([
                ".history-summary-grid",
                ".history-chart-panel",
                ".history-records-panel",
                ".history-overview-shell",
                "[data-local-update-region]"
            ].join(","));
            if (regions.length) {
                Array.prototype.forEach.call(regions, window.EdgeMotion.enterDataRegion);
            } else if (content.firstElementChild) {
                window.EdgeMotion.enterDataRegion(content.firstElementChild);
            }
            return;
        }
    }

    function replaceContents(target, replacement) {
        if (!target) return;
        while (target.firstChild) target.removeChild(target.firstChild);
        if (replacement) target.appendChild(replacement);
    }

    function replaceElement(current, next) {
        if (current && current.parentNode && next) {
            current.parentNode.replaceChild(next, current);
        }
    }

    function importChildren(source, target) {
        var fragment = document.createDocumentFragment();
        Array.prototype.forEach.call(source.childNodes, function (node) {
            fragment.appendChild(document.importNode(node, true));
        });
        replaceContents(target, fragment);
    }

    // 服务端 flash 和页面级错误位于稳定 shell 中；软导航提交时必须和主内容一起更新。
    function syncToastContainer(snapshot) {
        var nextContainer = snapshot.querySelector("[data-toast-container]");
        var currentContainer = document.querySelector("[data-toast-container]");
        if (!nextContainer || !currentContainer) return null;
        var replacement = document.importNode(nextContainer, true);
        replaceElement(currentContainer, replacement);
        return replacement;
    }

    function updateStableShell(snapshot, targetURL) {
        var nextContext = snapshot.querySelector(".topbar-page-context");
        var currentContext = document.querySelector(".topbar-page-context");
        if (nextContext && currentContext) {
            replaceElement(currentContext, document.importNode(nextContext, true));
        }

        var nextStatus = snapshot.getElementById("backend-status-pill");
        var currentStatus = document.getElementById("backend-status-pill");
        if (nextStatus && currentStatus) {
            currentStatus.className = nextStatus.className;
            currentStatus.textContent = nextStatus.textContent;
        }

        var nextMain = snapshot.querySelector(".main-shell");
        var currentMain = document.querySelector(".main-shell");
        if (nextMain && currentMain) {
            var pending = currentMain.classList.contains("page-navigation-pending");
            currentMain.className = nextMain.className;
            if (pending) currentMain.classList.add("page-navigation-pending");
        }

        var nextMeta = snapshot.querySelector('meta[name="csrf-token"]');
        var currentMeta = document.querySelector('meta[name="csrf-token"]');
        if (nextMeta && currentMeta) currentMeta.content = nextMeta.content;

        var committedToastContainer = syncToastContainer(snapshot);

        document.title = snapshot.title || document.title;
        document.body.dataset.pageKey = snapshot.body.dataset.pageKey || pageKeyForPath(targetURL.pathname);
        document.body.dataset.currentPath = snapshot.body.dataset.currentPath || targetURL.pathname + targetURL.search;
        document.body.dataset.passwordChangeRecommended = snapshot.body.dataset.passwordChangeRecommended || "false";
        updatePrimaryNavigation(document.body.dataset.pageKey, targetURL);
        return committedToastContainer;
    }

    function ensureControllerScripts(snapshot) {
        var scripts = Array.prototype.slice.call(snapshot.querySelectorAll("script[data-page-controller]"));
        return Promise.all(scripts.map(function (source) {
            var key = source.getAttribute("data-page-controller") || "";
            var src = source.getAttribute("src") || "";
            if (!key || !src || controllers[key]) return Promise.resolve();
            if (controllerLoads[key]) return controllerLoads[key];
            controllerLoads[key] = new Promise(function (resolve, reject) {
                var script = document.createElement("script");
                script.src = src;
                script.async = false;
                script.setAttribute("data-page-controller", key);
                script.onload = function () {
                    script.onload = null;
                    script.onerror = null;
                    if (controllers[key]) {
                        resolve();
                        return;
                    }
                    delete controllerLoads[key];
                    if (script.parentNode) script.parentNode.removeChild(script);
                    reject(new Error("页面控制器未注册：" + key));
                };
                script.onerror = function () {
                    delete controllerLoads[key];
                    if (script.parentNode) script.parentNode.removeChild(script);
                    reject(new Error("页面控制器加载失败：" + key));
                };
                document.body.appendChild(script);
            });
            return controllerLoads[key];
        }));
    }

    function hardNavigate(targetURL, pageKey) {
        markNavigationPending(targetURL, pageKey, false);
        window.location.assign(targetURL.href);
    }

    function isHashOnlyNavigation(targetURL) {
        return targetURL && targetURL.origin === window.location.origin &&
            normalizedPath(targetURL.pathname) === normalizedPath(window.location.pathname) &&
            targetURL.search === window.location.search &&
            targetURL.hash !== window.location.hash;
    }

    function navigate(target, options) {
        var targetURL;
        try {
            targetURL = new URL(target, window.location.href);
        } catch (_error) {
            return false;
        }
        if (isHashOnlyNavigation(targetURL)) {
            window.location.assign(targetURL.href);
            return true;
        }
        var pageKey = pageKeyForPath(targetURL.pathname);
        var previousPageKey = currentPageKey();
        var sameSection = Boolean(pageKey && pageKey === previousPageKey &&
            normalizedPath(targetURL.pathname) === normalizedPath(window.location.pathname));
        if (!pageKey || !pageKeyForPath(window.location.pathname) ||
                targetURL.origin !== window.location.origin || currentPageKey() === "login") {
            hardNavigate(targetURL, pageKey);
            return false;
        }
        if (!navigationBusy && targetURL.href === window.location.href &&
                (!options || !options.forceReload)) {
            updatePrimaryNavigation(pageKey, targetURL);
            clearNavigationPending();
            return true;
        }
        if (navigationBusy && navigationTarget === targetURL.href) {
            markNavigationPending(targetURL, pageKey, sameSection);
            return true;
        }

        navigationVersion += 1;
        var version = navigationVersion;
        var metrics = {
            target: targetURL.pathname + targetURL.search,
            navigationStart: now()
        };
        navigationBusy = true;
        navigationTarget = targetURL.href;
        if (navigationRequest) {
            try { navigationRequest.abort(); } catch (_error) { /* optional cancellation */ }
        }
        navigationRequest = window.AbortController ? new window.AbortController() : null;

        try {
            markNavigationPending(targetURL, pageKey, sameSection);
            metrics.visualResponse = now() - metrics.navigationStart;
            if (EdgeApp.resetTransientUI) {
                EdgeApp.resetTransientUI();
            } else if (window.EdgeVirtualKeyboard && window.EdgeVirtualKeyboard.hide) {
                window.EdgeVirtualKeyboard.hide();
            }
            deactivatePageControllers();
            if (!sameSection && showNavigationSkeleton(pageKey)) {
                var content = document.querySelector(".content-stack");
                if (content && window.EdgeMotion && window.EdgeMotion.enterPage) {
                    window.EdgeMotion.enterPage(content);
                }
                metrics.structureVisible = now() - metrics.navigationStart;
            }
        } catch (_syncError) {
            navigationBusy = false;
            navigationTarget = "";
            navigationRequest = null;
            hardNavigate(targetURL, pageKey);
            return false;
        }

        var requestOptions = {
            method: "GET",
            credentials: "same-origin",
            cache: "no-store",
            headers: {
                "Accept": "text/html",
                "X-Requested-With": "Edge-Soft-Navigation"
            }
        };
        if (navigationRequest) requestOptions.signal = navigationRequest.signal;

        window.fetch(targetURL.pathname + targetURL.search, requestOptions)
            .then(function (response) {
                if (version !== navigationVersion) throw staleNavigationError();
                metrics.responseReady = now() - metrics.navigationStart;
                if (!response.ok) throw new Error("页面请求失败：" + response.status);
                if (response.redirected && new URL(response.url).pathname === "/login") {
                    throw new Error("会话已失效");
                }
                return response.text();
            })
            .then(function (html) {
                if (version !== navigationVersion) throw staleNavigationError();
                var parseStart = now();
                var snapshot = new DOMParser().parseFromString(html, "text/html");
                var nextContent = snapshot.querySelector(".content-stack");
                var currentContent = document.querySelector(".content-stack");
                if (!nextContent || !currentContent || snapshot.body.dataset.pageKey === "login") {
                    throw new Error("页面结构不完整");
                }
                metrics.parse = now() - parseStart;
                var commitStart = now();
                var committedToastContainer = updateStableShell(snapshot, targetURL);
                importChildren(nextContent, currentContent);
                metrics.commit = now() - commitStart;
                if (!options || options.history !== "none") {
                    window.history.pushState({ edgeSoftNavigation: true }, "", targetURL.href);
                }
                if (EdgeApp.hydratePage) {
                    EdgeApp.hydratePage(currentContent);
                    if (committedToastContainer) EdgeApp.hydratePage(committedToastContainer);
                }
                clearNavigationPending();
                enterCommittedContent(currentContent, sameSection);
                metrics.contentVisible = now() - metrics.navigationStart;
                return ensureControllerScripts(snapshot).then(function () {
                    if (version !== navigationVersion) throw staleNavigationError();
                    return new Promise(function (resolve) {
                        afterNextPaint(resolve);
                    });
                }).then(function () {
                    if (version !== navigationVersion) throw staleNavigationError();
                    var initStart = now();
                    navigationBusy = false;
                    navigationTarget = "";
                    navigationRequest = null;
                    activatePageControllers();
                    metrics.controllerInit = now() - initStart;
                    clearNavigationPending();
                    metrics.total = now() - metrics.navigationStart;
                    lastMetrics = metrics;
                });
            })
            .catch(function (error) {
                if (version !== navigationVersion || error.edgeStale || error.name === "AbortError") return;
                navigationBusy = false;
                navigationTarget = "";
                navigationRequest = null;
                hardNavigate(targetURL, pageKey);
            });
        return true;
    }

    function eligibleLink(event) {
        if (event.defaultPrevented || event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) {
            return null;
        }
        var target = event.target;
        var link = target && typeof target.closest === "function" ? target.closest("a[href]") : null;
        if (!link || link.target || link.hasAttribute("download") || link.closest("[data-no-soft-navigation]")) return null;
        return link;
    }

    document.addEventListener("click", function (event) {
        var link = eligibleLink(event);
        if (!link) return;
        var targetURL;
        try { targetURL = new URL(link.href, window.location.href); } catch (_error) { return; }
        if (targetURL.origin !== window.location.origin) return;
        if (isHashOnlyNavigation(targetURL)) return;
        var pageKey = pageKeyForPath(targetURL.pathname);
        if (!pageKey) {
            markNavigationPending(targetURL, currentPageKey());
            return;
        }
        event.preventDefault();
        navigate(targetURL.href);
    });

    window.addEventListener("popstate", function () {
        if (!pageKeyForPath(window.location.pathname)) {
            window.location.reload();
            return;
        }
        // popstate 触发时地址栏已经切换，必须绕过“同地址”快捷路径并替换页面内容。
        navigate(window.location.href, { history: "none", forceReload: true });
    });

    window.addEventListener("pagehide", function () {
        navigationVersion += 1;
        if (navigationRequest) {
            try { navigationRequest.abort(); } catch (_error) { /* optional cancellation */ }
        }
        deactivatePageControllers();
    });

    window.addEventListener("pageshow", function (event) {
        if (event.persisted) {
            navigationBusy = false;
            navigationTarget = "";
            navigationRequest = null;
            clearNavigationPending();
            activatePageControllers();
        }
    });

    EdgeApp.registerPageController = registerPageController;
    EdgeApp.activatePageControllers = activatePageControllers;
    EdgeApp.navigate = navigate;
    EdgeApp.isStalePageError = function (error) {
        return Boolean(error && (error.edgeStale || error.name === "AbortError"));
    };

    window.EdgeNavigation = {
        getLastMetrics: function () {
            return lastMetrics ? Object.assign({}, lastMetrics) : null;
        },
        getDiagnostics: function () {
            return {
                navigationVersion: navigationVersion,
                busy: navigationBusy,
                target: navigationTarget,
                activeControllers: Object.keys(activeControllers),
                registeredControllers: Object.keys(controllers)
            };
        },
        navigate: navigate
    };
})();
