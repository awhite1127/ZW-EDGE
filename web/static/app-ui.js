// 页面级轮询控制器：只依赖 PageScope，页面切换时自动回收资源。
(function () {
    "use strict";

    var EdgeApp = window.EdgeApp;
    if (!EdgeApp) return;

    function PollingController(scope, callback, delay) {
        this.scope = scope;
        this.callback = typeof callback === "function" ? callback : function () {};
        this.delay = Math.max(100, Number(delay) || 10000);
        this.timer = null;
        this.running = false;
        this.inFlight = false;
        this.disposed = false;
        var self = this;
        if (scope && typeof scope.onVisibilityChange === "function") {
            scope.onVisibilityChange(function () {
                if (self.disposed) return;
                if (self.hidden()) self.stop();
                else self.start(true);
            });
        }
        if (scope && typeof scope.listen === "function") {
            scope.listen(window, "pageshow", function (event) {
                if (event.persisted) self.start(true);
            });
        }
        if (scope && typeof scope.onDispose === "function") {
            scope.onDispose(function () { self.dispose(); });
        }
    }

    PollingController.prototype.hidden = function () {
        return typeof EdgeApp.isPageHidden === "function" && EdgeApp.isPageHidden();
    };

    PollingController.prototype.schedule = function () {
        var self = this;
        if (!this.running || this.disposed || this.hidden()) return;
        this.timer = window.setTimeout(function () {
            self.timer = null;
            self.run();
        }, this.delay);
    };

    PollingController.prototype.run = function () {
        var self = this;
        if (!this.running || this.disposed || this.hidden() || this.inFlight) return;
        this.inFlight = true;
        var finish = function () {
            self.inFlight = false;
            self.schedule();
        };
        try {
            Promise.resolve(this.callback()).then(finish, finish);
        } catch (_error) {
            finish();
        }
    };

    PollingController.prototype.start = function (runImmediately) {
        if (this.disposed) return;
        this.stop();
        this.running = true;
        if (runImmediately) this.run();
        else this.schedule();
    };

    PollingController.prototype.stop = function () {
        this.running = false;
        if (this.timer !== null) {
            window.clearTimeout(this.timer);
            this.timer = null;
        }
    };

    PollingController.prototype.dispose = function () {
        if (this.disposed) return;
        this.disposed = true;
        this.stop();
    };

    EdgeApp.PollingController = PollingController;
})();
