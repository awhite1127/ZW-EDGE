# Edge Controller 工程文档

本目录只描述当前正式产品基线，不记录研发阶段编号或临时迭代过程。仓库总览和主要运行链见根目录 [`README.md`](../README.md)。

## 文档入口

- [`框架解析.md`](框架解析.md)：进程职责、C++ 分层、IPC、采集、历史、告警、北向、Web 和升级边界。
- [`设备接入规则.md`](设备接入规则.md)：内置设备 ReadBlocks、字段能力、WriteCommands、标准示例和接入检查清单。
- [`部署板端解析.md`](部署板端解析.md)：RK3562/AArch64 正式发布、板端部署、systemd、升级、运维与验收。
- [`产品维护说明.md`](产品维护说明.md)：账号恢复、配置迁移、恢复出厂、诊断日志和版本确认。
- [`../scripts/rk3562/README.md`](../scripts/rk3562/README.md)：随正式包交付的最小安装说明。

## 当前产品基线

| 项目 | 基线 |
| --- | --- |
| 正式硬件 | RK3562 / AArch64 |
| 后端 | C++17 `edge-controller` |
| Web | Go 1.22 `edge-web` |
| 页面 | 服务端模板 + 原生 JavaScript/CSS |
| 数据库 | SQLite 配置、历史、事件三库 |
| 进程通信 | Unix Domain Socket + 长度帧 JSON |
| 南向 | Modbus RTU / TCP 主站 |
| 北向 | MQTT/TLS、Modbus TCP Server |
| 服务管理 | systemd |
| 升级 | 离线包、校验、backup、原子切换、恢复和回滚 |

仓库 [`../VERSION`](../VERSION) 是版本唯一受版本管理的默认来源；发布时允许一次性的显式 SemVer 覆盖。controller、Web 与 UI 同版本发布；当前运行时不维护跨大版本 Web/controller 回退链。

## 必须保持的兼容边界

- IPC method、参数、response schema 和错误语义；
- SQLite 当前 schema 校验，以及未来发布显式提供的前向迁移；
- 应用升级 package metadata、MANIFEST、Ed25519、状态持久化和恢复规则；
- 配置导入导出格式；
- Modbus 字节序/字序、无效值与质量语义；
- 1280×800、1920×1080、2560×1440 页面行为。

如果代码与文档不一致，应在同一次变更中修正文档。不要通过保留过期说明来表达历史，也不要用删除现场数据代替正式迁移设计。
