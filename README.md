# Edge Controller

Edge Controller 是面向 RK3562 / AArch64 Linux 边缘控制器的完整产品工程。它把现场 Modbus 采集、实时状态、历史数据、告警、MQTT、Modbus TCP 北向服务、Web 管理和离线升级整合为同一个版本发布。

仓库根目录的 [`VERSION`](VERSION) 是产品版本号唯一受版本管理的默认来源；发布入口也可接收一次性的显式 SemVer 覆盖。C++ 后端、Go Web 和 Web UI 必须作为同一版本构建、部署和回滚，不支持把当前 Web 与旧版 controller 混合运行。

## 产品组成

- `edge-controller`：C++17 后端，负责配置、采集、运行态、SQLite、告警、北向服务和系统操作。
- `edge-web`：Go 1.22 Web 服务，负责 HTTP、登录与权限、页面聚合、导出和升级入口。
- Web UI：服务端模板和无构建链 JavaScript/CSS，兼容板端 Chromium Kiosk。
- SQLite：配置、历史和事件分别持久化，并严格校验当前 schema 版本。
- Unix Domain Socket IPC：Go 与 C++ 间使用长度帧 + JSON 请求/响应。
- Update Manager：完成离线包校验、备份、原子切换、健康检查、失败恢复和回滚。

当前正式硬件平台只有 **RK3562 / AArch64**。10.1 英寸 1280×800 触摸屏、无屏控制器和 PC 浏览器访问使用同一套服务与页面。

## 代码目录

```text
backend/
  src/
    application/
      app/                    进程装配与生命周期
      interface/              UDS IPC、协议编解码和领域 handler
      service/                BackendService 门面、配置应用和运行协调
      manager/                通道与拓扑管理
    communication/
      channel/                串口与 TCP 通道
      collect/                读取计划、轮询采集、解析和写命令
      protocol/               Modbus RTU/TCP 协议
      mqtt/                   MQTT 客户端、payload 和发布服务
      modbus_server/          Modbus TCP 北向服务与寄存器映射
    data/
      datastore/              SQLite 存储、schema 校验和数据维护
      model/                  稳定领域模型
    infrastructure/
      platform/               Linux 平台能力
      security/               密码与安全能力
      maintenance/            恢复与维护能力
    shared/
      common/                 通用类型、工具和兼容层
web/
  cmd/edge-web/         Web 进程入口
  internal/ipc/         可取消、限并发的 IPC 客户端
  internal/service/     页面数据聚合和导出
  internal/http/        路由、鉴权、handler 和模板渲染
  internal/model/       IPC 与页面模型
  templates/            服务端页面模板
  static/               页面控制器、公共前端能力和样式
scripts/
  release-arm64-rk3562.sh   正式发布入口
  deploy-rk3562.sh          无屏控制器安装与验证入口
  deploy-rk3562-display.sh  显示终端入口，复用公共部署后安装 Kiosk
  rk3562/                   systemd、安装、升级、日志和显示附加资产
docs/                       架构、开发、部署与运维说明
```

## 主要运行链路

### Modbus 采集

```text
SQLite 配置
  -> polling / read plan
  -> Modbus RTU 或 TCP transport
  -> 寄存器解析与字段有效性判断
  -> 内存 runtime snapshot
  -> history / alarm
  -> Web / MQTT / Modbus TCP slave
```

设备类型定义读取区块、字段数据类型、字节序/字序、倍率、枚举、bit 和无效值规则。轮询只使用已校验的运行态配置；配置切换会完整重建受影响的通道与拓扑。

新增或维护内置设备时，请遵循 [`设备接入规则`](docs/设备接入规则.md)。

### Web

```text
Browser
  -> HTTP / session / CSRF / permission
  -> Go console service
  -> UDS IPC
  -> BackendService
  -> datastore / runtime / protocol
```

Go 不直接访问业务 SQLite、串口或现场 TCP 设备。页面按钮不是授权边界，所有写操作都由 HTTP 层再次校验角色和 CSRF。

### 历史数据

```text
有效采集值
  -> history queue / sampling policy
  -> SQLite raw history
  -> hour/day aggregate
  -> Go 分页或流式导出
  -> Web trend
```

系统时间无效时暂停历史写入；检测到明显时间跳变时清理尚未落库的采样状态，避免混合两条时间轴。历史保留、聚合和清理均在 SQLite 事务边界内执行。

### 告警

```text
实时点位
  -> alarm rule evaluation
  -> pending / active / recovered 状态机
  -> current event / history event
  -> acknowledge
  -> Web 与 MQTT
```

事件成功落库后才 best-effort 发布 MQTT；北向异常不会阻塞采集、历史或 Web。

### 应用升级

```text
Web upload
  -> 受限暂存区
  -> update manager package verification
  -> READY job
  -> systemd root runner
  -> backup
  -> atomic switch
  -> health check
  -> success 或 rollback
```

升级包格式、Ed25519 校验、状态持久化、中断恢复、数据库兼容和回滚规则属于产品兼容边界，不随普通内部重构改变。

## 开发与构建

本地 C++ 构建需要 CMake、C++17 编译器和 libmosquitto 开发包：

```sh
cmake -S backend -B backend/build
cmake --build backend/build --config Release -j4
```

Go、前端语法和升级管理器检查：

```sh
cd web && go build -o edge-web ./cmd/edge-web
node --check static/app-core.js
node --check static/app-navigation.js
python3 -m py_compile ../scripts/rk3562/update-manager.py
```

开发环境可以分别启动 controller 与 Web；生产环境必须使用发布包中的 systemd 单元和固定目录。环境变量、数据库路径、IPC 帧限制和进程资源边界见 [`docs/README.md`](docs/README.md)。

## 正式发布与部署

在配置好 RK3562 交叉工具链和 AArch64 依赖的 Linux 发布机执行：

```sh
bash scripts/release-arm64-rk3562.sh
```

该入口负责版本校验、C++/Go 交叉构建、AArch64 ELF 与依赖检查、包组装和安装后验证。不得把宿主机产物或单独构建目录直接复制到板端。

在无屏边缘控制器上安装并验证发布包：

```sh
sudo bash scripts/deploy-rk3562.sh --package ./edge-controller-rk3562-<version>.tar.gz
```

带屏智能显示终端使用显示版入口。它先执行同一公共部署链，成功后再安装 `kickpi` 桌面 Kiosk：

```sh
sudo bash scripts/deploy-rk3562-display.sh --package ./edge-controller-rk3562-<version>.tar.gz
```

部署、目录所有权、网络首启保护、日志轮转、升级策略和验收项见 [`docs/README.md`](docs/README.md)。发布包内面向安装人员的最小说明位于 [`scripts/rk3562/README.md`](scripts/rk3562/README.md)。

## 工程边界

- 保持 `BackendService` 作为 IPC 上层的稳定门面，领域实现放在对应 service/datastore/runtime。
- IPC method、参数、response schema 和错误语义是 Web/controller 同版本间的稳定契约。
- 当前代码只接受空库或当前 schema；后续一旦变更 schema，发布版本必须显式提供并验证前向迁移与回滚策略。
- 用户可见术语统一为“设备类型”；内部 `DeviceTemplate` 和既有表/字段不做无收益的破坏式迁移。
- 不引入 Node 前端构建链或板端 Chromium 无法稳定支持的语法。
- 1280×800、1920×1080 和 2560×1440 三种布局均是发布前必须验证的产品形态。
