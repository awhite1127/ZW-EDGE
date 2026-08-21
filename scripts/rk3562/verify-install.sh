#!/usr/bin/env bash

# 验证已安装的 RK3562 系统；日志检查只执行一次安全轮转。
set -uo pipefail

ROOT="/opt/edge-controller"
FAILURES=0

pass()
{
    printf '[verify-install] PASS: %s\n' "$*"
}

fail()
{
    printf '[verify-install] FAIL: %s\n' "$*" >&2
    FAILURES=$((FAILURES + 1))
}

check_path_mode()
{
    path="$1"
    expected="$2"
    [ -e "${path}" ] || { fail "路径不存在：${path}"; return; }
    actual="$(stat -c '%U:%G:%a' -- "${path}" 2>/dev/null || true)"
    actual_mode="${actual##*:}"
    case "${actual_mode}" in
        [1-7][0-7][0-7][0-7])
            fail "权限错误 ${path}：实际=${actual}；期望=${expected}；检测到 setuid、setgid 或 sticky 特殊权限位"
            return
            ;;
    esac
    [ "${actual}" = "${expected}" ] && pass "权限 ${path} = ${actual}（无特殊权限位）" ||
        fail "权限错误 ${path}：实际=${actual:-<无法读取>}；期望=${expected}"
}

check_aarch64()
{
    path="$1"
    [ -e "${path}" ] || [ -L "${path}" ] || { fail "ELF 缺失：${path}"; return; }
    resolved="$(readlink -f -- "${path}" 2>/dev/null || true)"
    header="$(LC_ALL=C readelf -hW "${resolved}" 2>/dev/null || true)"
    if printf '%s\n' "${header}" | grep -F 'Class:                             ELF64' >/dev/null &&
       printf '%s\n' "${header}" | grep -F 'Machine:                           AArch64' >/dev/null; then
        pass "AArch64 ELF：${path}"
    else
        fail "不是 ELF64/AArch64：${path}"
    fi
}

check_service_link()
{
    name="$1"
    expected="${ROOT}/systemd/${name}"
    link="/etc/systemd/system/${name}"
    [ -L "${link}" ] || { fail "systemd 登记不是软链接：${link}"; return; }
    [ "$(readlink -f -- "${link}" 2>/dev/null || true)" = "${expected}" ] &&
        pass "systemd 软链接：${name}" || fail "systemd 软链接目标错误：${name}"
}

read_network_database_state()
{
    python3 - "${ROOT}/data/edge-config.db" <<'PY'
import sqlite3
import sys

database = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
marker = database.execute(
    "SELECT value FROM config_meta WHERE key='network_settings_explicitly_configured'"
).fetchone()
settings = database.execute(
    "SELECT mode, interface_name, ip_address, gateway FROM network_settings WHERE id=1"
).fetchone()
if marker is None or settings is None:
    raise SystemExit(1)
print("\t".join((marker[0],) + settings))
PY
}

current_network_state()
{
    current_interface="$(ip -4 route show default 2>/dev/null | awk '
        NR == 1 { for (i = 1; i <= NF; ++i) if ($i == "dev" && i < NF) { print $(i + 1); exit } }')"
    current_ip_address=""
    current_gateway=""
    if [ -n "${current_interface}" ]; then
        current_ip_address="$(ip -4 -o addr show dev "${current_interface}" scope global 2>/dev/null | awk '
            NR == 1 { split($4, address, "/"); print address[1]; exit }')"
        current_gateway="$(ip -4 route show default dev "${current_interface}" 2>/dev/null | awk '
            NR == 1 { for (i = 1; i <= NF; ++i) if ($i == "via" && i < NF) { print $(i + 1); exit } }')"
    fi
}

main()
{
    [ "$#" -eq 0 ] || { fail "verify-install.sh 不接受参数"; exit 2; }
    for command_name in stat readelf readlink grep find systemctl runuser ss id wc sed tail python3 ip awk cmp; do
        command -v "${command_name}" >/dev/null 2>&1 || fail "缺少验证命令：${command_name}"
    done
    [ "$(id -u)" -eq 0 ] || { fail "verify-install.sh 必须以 root 运行日志轮转检查"; exit 2; }

    check_path_mode "${ROOT}" "root:edge-controller:750"
    check_path_mode "${ROOT}/bin" "root:root:755"
    check_path_mode "${ROOT}/lib" "root:root:755"
    check_path_mode "${ROOT}/systemd" "root:root:755"
    check_path_mode "${ROOT}/scripts" "root:root:755"
    check_path_mode "${ROOT}/config" "root:edge-controller:750"
    check_path_mode "${ROOT}/config/edge-controller.env" "root:edge-controller:640"
    check_path_mode "${ROOT}/config/edge-controller.env.default" "root:edge-controller:640"
    check_path_mode "${ROOT}/data" "root:root:750"
    check_path_mode "${ROOT}/log" "root:edge-controller:750"
    check_path_mode "${ROOT}/run" "root:edge-controller:770"
    check_path_mode "${ROOT}/update" "root:edge-controller:750"
    check_path_mode "${ROOT}/update/incoming" "root:edge-controller:750"
    check_path_mode "${ROOT}/update/upload" "root:edge-controller:770"
    check_path_mode "${ROOT}/update/jobs" "root:root:700"
    check_path_mode "${ROOT}/update/backup" "root:root:700"
    check_path_mode "${ROOT}/update/recovery" "root:root:700"
    check_path_mode "${ROOT}/update/recovery/update-manager.py" "root:root:700"
    check_path_mode "${ROOT}/security" "root:root:750"
    check_path_mode "${ROOT}/update/status.json" "root:edge-controller:640"
    check_path_mode "${ROOT}/bin/edge-controller" "root:root:755"
    check_path_mode "${ROOT}/bin/edge-web" "root:root:755"
    check_path_mode "${ROOT}/systemd/edge-controller.service" "root:root:644"
    check_path_mode "${ROOT}/systemd/edge-web.service" "root:root:644"
    check_path_mode "${ROOT}/systemd/edge-log-rotator.service" "root:root:644"
    check_path_mode "${ROOT}/scripts/install.sh" "root:root:755"
    check_path_mode "${ROOT}/scripts/verify-install.sh" "root:root:755"
    check_path_mode "${ROOT}/scripts/rotate-logs.sh" "root:root:755"
    check_path_mode "${ROOT}/scripts/update-manager.py" "root:root:755"
    check_path_mode "${ROOT}/systemd/edge-upgrade@.service" "root:root:644"
    check_path_mode "${ROOT}/systemd/edge-upgrade-recovery.service" "root:root:644"
    check_path_mode "${ROOT}/systemd/edge-upgrade-recovery-verify.service" "root:root:644"
    cmp -s "${ROOT}/scripts/update-manager.py" "${ROOT}/update/recovery/update-manager.py" &&
        pass "持久恢复 manager 与当前版本一致" || fail "持久恢复 manager 与当前版本不一致"
    if [ -f "${ROOT}/MANIFEST.sha256.sig" ]; then
        check_path_mode "${ROOT}/MANIFEST.sha256.sig" "root:root:644"
        [ "$(stat -c '%s' -- "${ROOT}/MANIFEST.sha256.sig")" -eq 64 ] &&
            pass "已安装 Ed25519 签名长度正确" || fail "已安装 Ed25519 签名长度错误"
    fi
    for expected_environment in \
        'EDGE_CONTROLLER_DATA_DIR=/opt/edge-controller/data' \
        'EDGE_CONTROLLER_LOG_DIR=/opt/edge-controller/log' \
        'EDGE_CONTROLLER_RUN_DIR=/opt/edge-controller/run' \
        'EDGE_CONTROLLER_IPC_SOCK=/opt/edge-controller/run/edge-controller.sock' \
        'EDGE_CONTROLLER_REQUIRE_EXPLICIT_NETWORK_CONFIG=1' \
        'EDGE_UPGRADE_PUBLIC_KEY=/opt/edge-controller/security/upgrade-ed25519.pub' \
        'EDGE_WEB_LISTEN=:8080'; do
        grep -F -x "${expected_environment}" "${ROOT}/config/edge-controller.env" >/dev/null &&
            pass "运行环境：${expected_environment}" || fail "运行环境缺少：${expected_environment}"
    done
    grep -E '^EDGE_UPGRADE_SIGNATURE_POLICY=(optional|required)$' \
        "${ROOT}/config/edge-controller.env" >/dev/null &&
        pass "升级签名策略为 optional/required 显式模式" || fail "升级签名策略缺失或非法"
    signature_policy="$(sed -n 's/^EDGE_UPGRADE_SIGNATURE_POLICY=//p' "${ROOT}/config/edge-controller.env" | tail -n 1)"
    if [ -e "${ROOT}/security/upgrade-ed25519.pub" ] || [ -L "${ROOT}/security/upgrade-ed25519.pub" ]; then
        check_path_mode "${ROOT}/security/upgrade-ed25519.pub" "root:root:644"
    elif [ "${signature_policy}" = required ]; then
        fail "required 签名策略缺少设备 Ed25519 信任公钥"
    else
        pass "optional 开发策略允许尚未预置设备签名公钥"
    fi
    grep -F -x 'EDGE_CONTROLLER_REQUIRE_EXPLICIT_NETWORK_CONFIG=1' \
        "${ROOT}/config/edge-controller.env.default" >/dev/null &&
        pass "RK3562 默认环境启用首次网络保护" ||
        fail "RK3562 默认环境未启用首次网络保护"
    for log_environment_key in EDGE_LOG_MAX_BYTES EDGE_LOG_BACKUPS EDGE_LOG_ROTATE_INTERVAL_SECONDS; do
        grep -E "^[[:space:]]*${log_environment_key}=[^[:space:]]+" \
            "${ROOT}/config/edge-controller.env" >/dev/null &&
            pass "日志轮转环境参数存在：${log_environment_key}" ||
            fail "日志轮转环境参数缺失：${log_environment_key}"
    done

    for elf_path in \
        bin/edge-controller bin/edge-web \
        lib/libmosquitto.so.1 lib/libmosquitto.so.2.0.18 \
        lib/libssl.so.3 lib/libcrypto.so.3 lib/libstdc++.so.6 lib/libgcc_s.so.1; do
        check_aarch64 "${ROOT}/${elf_path}"
    done
    [ -L "${ROOT}/lib/libmosquitto.so.1" ] &&
        [ "$(readlink -- "${ROOT}/lib/libmosquitto.so.1")" = "libmosquitto.so.2.0.18" ] &&
        pass "libmosquitto SONAME 软链接完整" || fail "libmosquitto.so.1 软链接错误"
    for soname_pair in \
        'libmosquitto.so.1:libmosquitto.so.1' \
        'libssl.so.3:libssl.so.3' \
        'libcrypto.so.3:libcrypto.so.3' \
        'libstdc++.so.6:libstdc++.so.6' \
        'libgcc_s.so.1:libgcc_s.so.1'; do
        library_name="${soname_pair%%:*}"
        expected_soname="${soname_pair#*:}"
        library_dynamic="$(LC_ALL=C readelf -dW "${ROOT}/lib/${library_name}" 2>/dev/null || true)"
        printf '%s\n' "${library_dynamic}" | grep -F "Library soname: [${expected_soname}]" >/dev/null &&
            pass "SONAME 正确：${library_name}" || fail "SONAME 错误：${library_name}"
    done

    controller_dynamic="$(LC_ALL=C readelf -dW "${ROOT}/bin/edge-controller" 2>/dev/null || true)"
    printf '%s\n' "${controller_dynamic}" | grep -F 'Shared library: [libmosquitto.so.1]' >/dev/null &&
        pass "edge-controller NEEDED 包含 libmosquitto.so.1" || fail "edge-controller 缺少 libmosquitto.so.1 NEEDED"
    printf '%s\n' "${controller_dynamic}" | grep -F '$ORIGIN/../lib' >/dev/null &&
        pass "edge-controller RPATH/RUNPATH 正确" || fail "edge-controller RPATH/RUNPATH 错误"
    controller_program="$(LC_ALL=C readelf -lW "${ROOT}/bin/edge-controller" 2>/dev/null || true)"
    printf '%s\n' "${controller_program}" | grep -F '/lib/ld-linux-aarch64.so.1' >/dev/null &&
        pass "edge-controller 动态解释器正确" || fail "edge-controller 动态解释器错误"

    web_program="$(LC_ALL=C readelf -lW "${ROOT}/bin/edge-web" 2>/dev/null || true)"
    web_dynamic="$(LC_ALL=C readelf -dW "${ROOT}/bin/edge-web" 2>/dev/null || true)"
    if ! printf '%s\n' "${web_program}" | grep -F 'INTERP' >/dev/null &&
       ! printf '%s\n' "${web_dynamic}" | grep -F '(NEEDED)' >/dev/null; then
        pass "edge-web 为静态 ELF"
    else
        fail "edge-web 不是静态 ELF"
    fi
    for recovery_unit in edge-upgrade-recovery.service edge-upgrade-recovery-verify.service; do
        if [ -f "/etc/systemd/system/${recovery_unit}" ] &&
           [ ! -L "/etc/systemd/system/${recovery_unit}" ] &&
           cmp -s "${ROOT}/systemd/${recovery_unit}" "/etc/systemd/system/${recovery_unit}"; then
            pass "升级恢复 systemd 服务已按 root 文件安装：${recovery_unit}"
        else
            fail "升级恢复 systemd 服务缺失、为软链接或内容不一致：${recovery_unit}"
        fi
        systemctl is-enabled --quiet "${recovery_unit}" &&
            pass "升级恢复服务已启用：${recovery_unit}" || fail "升级恢复服务未启用：${recovery_unit}"
    done
    for service_name in edge-controller.service edge-web.service edge-log-rotator.service; do
        gate_directory="/etc/systemd/system/${service_name}.d"
        gate_file="${gate_directory}/edge-upgrade-recovery-gate.conf"
        check_path_mode "${gate_directory}" "root:root:755"
        check_path_mode "${gate_file}" "root:root:644"
        grep -F -x 'ExecCondition=+/usr/bin/python3 /opt/edge-controller/update/recovery/update-manager.py service-start-allowed' \
            "${gate_file}" >/dev/null && pass "${service_name} 升级恢复启动门禁已安装" || \
            fail "${service_name} 缺少升级恢复启动门禁"
    done

    check_service_link edge-controller.service
    check_service_link edge-web.service
    check_service_link edge-log-rotator.service
    if [ -f /etc/systemd/system/edge-upgrade@.service ] &&
       [ ! -L /etc/systemd/system/edge-upgrade@.service ] &&
       cmp -s "${ROOT}/systemd/edge-upgrade@.service" /etc/systemd/system/edge-upgrade@.service; then
        pass "独立升级 systemd 模板已按 root 文件安装"
    else
        fail "独立升级 systemd 模板缺失、为软链接或内容不一致"
    fi
    python3 - "${ROOT}/update/status.json" <<'PY' >/dev/null 2>&1 &&
import json
import sys
status = json.load(open(sys.argv[1], encoding="utf-8"))
assert status["state"] in {"IDLE", "VALIDATING", "READY", "BACKING_UP", "STOPPING", "INSTALLING", "STARTING", "VERIFYING", "SUCCESS", "FAILED", "ROLLING_BACK", "ROLLED_BACK"}
PY
        pass "升级状态文件 JSON 与状态机合法" || fail "升级状态文件 JSON 或 state 非法"
    python3 - "${ROOT}/package-info.json" "${ROOT}/VERSION" <<'PY' >/dev/null 2>&1 &&
import json
import sys

metadata = json.load(open(sys.argv[1], encoding="utf-8"))
version = open(sys.argv[2], encoding="utf-8").read().strip()
assert metadata["package_format_version"] == 1
assert metadata["product"] == "edge-controller"
assert metadata["platform"] == "rk3562"
assert metadata["arch"] == "aarch64"
assert metadata.get("signature_format_version", 1) == 1
assert metadata.get("signature_algorithm", "none") in {"none", "ed25519"}
assert isinstance(metadata["build_time"], str) and metadata["build_time"]
assert metadata["version"] == version and version
PY
        pass "已安装包元数据与 VERSION 契约一致" || fail "已安装包元数据或 VERSION 契约非法"
    for service_name in edge-controller.service edge-web.service edge-log-rotator.service; do
        systemctl is-enabled --quiet "${service_name}" && pass "服务已启用：${service_name}" || fail "服务未启用：${service_name}"
        systemctl is-active --quiet "${service_name}" && pass "服务运行中：${service_name}" || fail "服务未运行：${service_name}"
    done

    socket_path="${ROOT}/run/edge-controller.sock"
    [ -S "${socket_path}" ] && pass "IPC UDS 存在" || fail "IPC UDS 不存在：${socket_path}"
    if runuser -u edge-web -- test -r "${socket_path}" && runuser -u edge-web -- test -w "${socket_path}"; then
        pass "edge-web 用户具备 UDS 访问权限"
    else
        fail "edge-web 用户不具备 UDS 读写权限"
    fi
    ss -ltn | grep -E '(^|[[:space:]])[^[:space:]]*:8080[[:space:]]' >/dev/null &&
        pass "TCP 8080 正在监听" || fail "TCP 8080 未监听"

    for database_name in edge-config.db edge-history.db edge-events.db; do
        [ -f "${ROOT}/data/${database_name}" ] && pass "数据库位置正确：${database_name}" ||
            fail "数据库缺失：${ROOT}/data/${database_name}"
    done
    if [ -f "${ROOT}/data/edge-config.db" ]; then
        if runuser -u edge-web -- test -r "${ROOT}/data/edge-config.db"; then
            fail "edge-web 用户能够直接读取 SQLite 数据库"
        else
            pass "edge-web 用户无法直接读取 SQLite 数据库"
        fi

        database_network_state="$(read_network_database_state 2>/dev/null || true)"
        if [ -z "${database_network_state}" ]; then
            fail "无法读取网络配置确认状态"
        else
            IFS=$'\t' read -r network_confirmed configured_mode configured_interface configured_ip configured_gateway \
                <<< "${database_network_state}"
            case "${network_confirmed}" in
                false) pass "network_settings_explicitly_configured=false" ;;
                true) pass "network_settings_explicitly_configured=true" ;;
                *) fail "network_settings_explicitly_configured 值无效：${network_confirmed}" ;;
            esac

            current_network_state
            snapshot_path="${ROOT}/run/network-before-first-start.state"
            if [ -f "${snapshot_path}" ]; then
                previous_interface="$(sed -n 's/^interface=//p' "${snapshot_path}" | tail -n 1)"
                previous_ip="$(sed -n 's/^ip_address=//p' "${snapshot_path}" | tail -n 1)"
                previous_gateway="$(sed -n 's/^gateway=//p' "${snapshot_path}" | tail -n 1)"
                data_was_fresh="$(sed -n 's/^data_was_fresh=//p' "${snapshot_path}" | tail -n 1)"
            else
                previous_interface=""
                previous_ip=""
                previous_gateway=""
                data_was_fresh=0
            fi

            if [ "${data_was_fresh}" = 1 ]; then
                [ "${network_confirmed}" = false ] &&
                    pass "全新 data 首次启动保持网络配置未确认" ||
                    fail "全新 data 首次启动错误地确认了网络配置"
                [ -f "${snapshot_path}" ] || fail "缺少首次启动前网络快照"
                [ "${current_interface}" = "${previous_interface}" ] &&
                    [ "${current_ip_address}" = "${previous_ip}" ] &&
                    [ "${current_gateway}" = "${previous_gateway}" ] &&
                    pass "全新 data 首次启动保留原网口、IP 和默认网关" ||
                    fail "首次启动网络发生变化：${previous_interface}/${previous_ip}/${previous_gateway} -> ${current_interface}/${current_ip_address}/${current_gateway}"
                grep -F '首次安装尚未确认网络配置，保留系统当前网络状态' \
                    "${ROOT}/log"/edge-controller.log* >/dev/null 2>&1 &&
                    pass "首次网络保护日志存在" ||
                    fail "缺少首次网络保护日志"
            elif [ "${network_confirmed}" = true ]; then
                if [ "${configured_mode}" = static ]; then
                    [ "${current_interface}" = "${configured_interface}" ] &&
                        [ "${current_ip_address}" = "${configured_ip}" ] &&
                        [ "${current_gateway}" = "${configured_gateway}" ] &&
                        pass "当前运行态已恢复确认的静态网络配置" ||
                        fail "当前运行态与确认的静态网络配置不一致"
                elif [ "${configured_mode}" = dhcp ]; then
                    [ "${current_interface}" = "${configured_interface}" ] && [ -n "${current_ip_address}" ] &&
                        pass "当前运行态已恢复确认的 DHCP 网络配置" ||
                        fail "当前运行态与确认的 DHCP 网络配置不一致"
                else
                    fail "已确认网络配置模式无效：${configured_mode}"
                fi
            fi
        fi
    fi
    [ -f "${ROOT}/log/edge-controller.log" ] && [ -f "${ROOT}/log/edge-web.log" ] &&
        [ -f "${ROOT}/log/update.log" ] &&
        pass "服务日志位于集中日志目录" || fail "集中日志文件不完整"
    if "${ROOT}/scripts/rotate-logs.sh" --once; then
        pass "rotate-logs.sh --once 执行成功"
    else
        fail "rotate-logs.sh --once 执行失败"
    fi
    while IFS= read -r -d '' product_log; do
        check_path_mode "${product_log}" "root:edge-controller:640"
    done < <(find "${ROOT}/log" -maxdepth 1 -type f \
        \( -name 'edge-controller.log' -o -name 'edge-controller.log.[0-9]*' \
           -o -name 'edge-web.log' -o -name 'edge-web.log.[0-9]*' \
           -o -name 'update.log' -o -name 'update.log.[0-9]*' \) -print0)
    configured_backups="$(sed -n 's/^[[:space:]]*EDGE_LOG_BACKUPS=//p' \
        "${ROOT}/config/edge-controller.env" | tail -n 1)"
    case "${configured_backups}" in
        1|2|3|4|5|6|7|8|9|10) ;;
        *) configured_backups=3 ;;
    esac
    for log_name in edge-controller.log edge-web.log update.log; do
        backup_count="$(find "${ROOT}/log" -maxdepth 1 -type f -name "${log_name}.[0-9]*" -print | wc -l)"
        [ "${backup_count}" -le "${configured_backups}" ] &&
            pass "${log_name} 备份数量有界：${backup_count}/${configured_backups}" ||
            fail "${log_name} 备份过多：${backup_count}/${configured_backups}"
    done

    forbidden_secret="$(find \
        "${ROOT}/bin" "${ROOT}/lib" "${ROOT}/systemd" "${ROOT}/scripts" \
        -type f \
        \( -iname '*.pem' -o -iname '*.key' -o -iname '*.crt' -o -iname '*.p12' -o -iname '*.pfx' \) \
        -print -quit 2>/dev/null)"
    [ -z "${forbidden_secret}" ] && pass "程序资产未预置 MQTT TLS 证书或私钥（现场 config 资产允许保留）" ||
        fail "程序资产发现预置证书或私钥：${forbidden_secret}"

    if [ "${FAILURES}" -ne 0 ]; then
        printf '[verify-install] RESULT: FAIL (%s checks failed)\n' "${FAILURES}" >&2
        exit 1
    fi
    printf '[verify-install] RESULT: PASS\n'
}

main "$@"
