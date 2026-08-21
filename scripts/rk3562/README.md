# Edge Controller RK3562 发布包

本目录的文件会被组装进 RK3562 / AArch64 正式发布包。程序、私有动态库、配置、数据、日志和升级状态安装到 `/opt/edge-controller`，服务由 systemd 管理。

发布包根目录的 `VERSION`、tar.gz 文件名和 `package-info.json.version` 必须保存同一个不带 `V` 前缀的纯 SemVer。

## 安装

在解压后的 `edge-controller` 目录以 root 执行：

```sh
scripts/install.sh
```

只安装并 enable，不立即启动：

```sh
scripts/install.sh --no-start
```

安装后验证：

```sh
scripts/verify-install.sh
```

正式交付通常应使用发布目录外层的 `deploy-rk3562.sh`（无屏）或 `deploy-rk3562-display.sh`（带屏）。显示版先复用公共部署，成功后再安装 `kickpi` 桌面 Kiosk。

## 保留的现场资产

重复安装会更新程序、私有库、systemd、脚本和 package metadata，同时保留：

- `config/edge-controller.env` 及现场配置；
- `data`、`log`、`update` 和 `security`；
- `/etc/edge-controller/certs` 中的 MQTT TLS 资产。

新版本默认环境写入 `config/edge-controller.env.default`。安装器只向现场环境文件补齐缺失的必需键，不覆盖已有值。

全新数据库首次启动启用 `EDGE_CONTROLLER_REQUIRE_EXPLICIT_NETWORK_CONFIG=1`，不会自动应用产品默认静态地址。用户保存并应用网络配置或导入明确配置后，后续启动才恢复该设置。

## 应用升级

`scripts/update-manager.py` 提供：

```text
status
current-version
validate --package <incoming 包名>
import-upload --upload-id <id> --package <正式包名>
start --job-id <id>
```

Web 只能向 `update/upload` 写入随机名、`0600` 的完整暂存文件。root manager 会重新检查文件类型、属主、权限、大小、archive 路径、版本、MANIFEST、架构和签名，再接管到 `incoming` 并创建持久 job。

升级由 `edge-upgrade@.service` 的独立 runner 执行；`edge-upgrade-recovery.service` 和 `edge-upgrade-recovery-verify.service` 处理开机中断、异常退出与最终健康确认。backup、原子切换、失败恢复和回滚状态均持久化。

Ed25519 能力始终保留。默认 `EDGE_UPGRADE_SIGNATURE_POLICY=optional` 可接收 unsigned 正式包；切换为 `required` 前必须先安全部署匹配公钥并建立密钥轮换流程。发布包禁止包含私钥。

## 日志

`edge-log-rotator.service` 维护 `edge-controller.log`、`edge-web.log` 和 `update.log`。默认单文件 5 MiB、保留 3 份备份、每 60 秒检查一次。

手动检查：

```sh
scripts/rotate-logs.sh --once
```

超级管理员密码遗失时，可由板端 root 在停止 Web/controller 后恢复已有超级管理员：

```sh
systemctl stop edge-web edge-controller
/opt/edge-controller/bin/edge-controller --recover-super-admin admin
systemctl start edge-controller edge-web
```

命令交互式读取新密码，复用现有密码哈希与 SQLite 存储，只修改指定的已启用超级管理员账号；执行记录写入 `/opt/edge-controller/log/maintenance.log`。非 root 用户会被拒绝。

详细的发布、部署、升级和验收说明见源码仓库 `docs/部署板端解析.md`。
