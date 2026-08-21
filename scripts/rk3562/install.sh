#!/usr/bin/env bash

# 将已解压的 RK3562 发布包安装到 /opt/edge-controller。
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
SOURCE_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd -P)"
TARGET_ROOT="/opt/edge-controller"
NO_START=0
PRE_START_INTERFACE=""
PRE_START_IP_ADDRESS=""
PRE_START_GATEWAY=""
PRE_START_DATA_WAS_FRESH=0
RECOVERY_MODE="${EDGE_UPGRADE_RECOVERY_MODE:-0}"

log()
{
    printf '[edge-controller-install] %s\n' "$*"
}

die()
{
    printf '[edge-controller-install] ERROR: %s\n' "$*" >&2
    exit 1
}

need_cmd()
{
    command -v "$1" >/dev/null 2>&1 || die "缺少安装命令：$1"
}

capture_pre_start_state()
{
    PRE_START_INTERFACE="$(ip -4 route show default 2>/dev/null | awk '
        NR == 1 { for (i = 1; i <= NF; ++i) if ($i == "dev" && i < NF) { print $(i + 1); exit } }')"
    if [ -n "${PRE_START_INTERFACE}" ]; then
        PRE_START_IP_ADDRESS="$(ip -4 -o addr show dev "${PRE_START_INTERFACE}" scope global 2>/dev/null | awk '
            NR == 1 { split($4, address, "/"); print address[1]; exit }')"
        PRE_START_GATEWAY="$(ip -4 route show default dev "${PRE_START_INTERFACE}" 2>/dev/null | awk '
            NR == 1 { for (i = 1; i <= NF; ++i) if ($i == "via" && i < NF) { print $(i + 1); exit } }')"
    fi
    [ -f "${TARGET_ROOT}/data/edge-config.db" ] || PRE_START_DATA_WAS_FRESH=1
    log "已记录服务启动前网络状态：网口=${PRE_START_INTERFACE:-<无>}，IP=${PRE_START_IP_ADDRESS:-<无>}，网关=${PRE_START_GATEWAY:-<无>}"
}

set_exact_directory_mode()
{
    directory="$1"
    mode="$2"
    chmod u-s,g-s,o-t -- "${directory}"
    chmod "${mode}" -- "${directory}"
    actual_mode="$(stat -c '%a' -- "${directory}")"
    expected_mode="${mode#0}"
    [ "${actual_mode}" = "${expected_mode}" ] ||
        die "目录权限设置失败：${directory}；实际=${actual_mode}；期望=${expected_mode}（不得包含 setuid/setgid/sticky）"
}

set_install_permissions()
{
    chown root:edge-controller "${TARGET_ROOT}"
    chown -R root:root \
        "${TARGET_ROOT}/bin" "${TARGET_ROOT}/lib" \
        "${TARGET_ROOT}/systemd" "${TARGET_ROOT}/scripts"
    chown root:edge-controller "${TARGET_ROOT}/config" "${TARGET_ROOT}/log" "${TARGET_ROOT}/run"
    chown root:root "${TARGET_ROOT}/data"
    chown root:edge-controller \
        "${TARGET_ROOT}/update" "${TARGET_ROOT}/update/incoming" "${TARGET_ROOT}/update/upload"
    chown root:root \
        "${TARGET_ROOT}/update/jobs" "${TARGET_ROOT}/update/backup" "${TARGET_ROOT}/update/recovery"
    chown root:root "${TARGET_ROOT}/security"

    find \
        "${TARGET_ROOT}/bin" "${TARGET_ROOT}/lib" \
        "${TARGET_ROOT}/systemd" "${TARGET_ROOT}/scripts" \
        -type f -exec chmod 0644 {} +
    while IFS= read -r -d '' directory; do
        set_exact_directory_mode "${directory}" 0755
    done < <(find \
        "${TARGET_ROOT}/bin" "${TARGET_ROOT}/lib" \
        "${TARGET_ROOT}/systemd" "${TARGET_ROOT}/scripts" \
        -type d -print0)
    set_exact_directory_mode "${TARGET_ROOT}" 0750
    set_exact_directory_mode "${TARGET_ROOT}/config" 0750
    set_exact_directory_mode "${TARGET_ROOT}/data" 0750
    set_exact_directory_mode "${TARGET_ROOT}/log" 0750
    set_exact_directory_mode "${TARGET_ROOT}/run" 0770
    set_exact_directory_mode "${TARGET_ROOT}/update" 0750
    set_exact_directory_mode "${TARGET_ROOT}/update/incoming" 0750
    set_exact_directory_mode "${TARGET_ROOT}/update/upload" 0770
    set_exact_directory_mode "${TARGET_ROOT}/update/jobs" 0700
    set_exact_directory_mode "${TARGET_ROOT}/update/backup" 0700
    set_exact_directory_mode "${TARGET_ROOT}/update/recovery" 0700
    set_exact_directory_mode "${TARGET_ROOT}/security" 0750

    chmod 0755 \
        "${TARGET_ROOT}/bin/edge-controller" \
        "${TARGET_ROOT}/bin/edge-web" \
        "${TARGET_ROOT}/scripts/install.sh" \
        "${TARGET_ROOT}/scripts/verify-install.sh" \
        "${TARGET_ROOT}/scripts/rotate-logs.sh" \
        "${TARGET_ROOT}/scripts/update-manager.py"
    if [ -f "${TARGET_ROOT}/update/recovery/update-manager.py" ]; then
        chown root:root "${TARGET_ROOT}/update/recovery/update-manager.py"
        chmod 0700 "${TARGET_ROOT}/update/recovery/update-manager.py"
    fi
    if [ -f "${TARGET_ROOT}/security/upgrade-ed25519.pub" ]; then
        chown root:root "${TARGET_ROOT}/security/upgrade-ed25519.pub"
        chmod 0644 "${TARGET_ROOT}/security/upgrade-ed25519.pub"
    fi
    chown root:edge-controller \
        "${TARGET_ROOT}/config/edge-controller.env" \
        "${TARGET_ROOT}/config/edge-controller.env.default"
    chmod 0640 \
        "${TARGET_ROOT}/config/edge-controller.env" \
        "${TARGET_ROOT}/config/edge-controller.env.default"

    if [ -e "${TARGET_ROOT}/update/status.json" ] || [ -L "${TARGET_ROOT}/update/status.json" ]; then
        [ -f "${TARGET_ROOT}/update/status.json" ] && [ ! -L "${TARGET_ROOT}/update/status.json" ] ||
            die "升级状态路径必须是常规文件且不能是软链接"
        chown root:edge-controller "${TARGET_ROOT}/update/status.json"
        chmod 0640 "${TARGET_ROOT}/update/status.json"
    fi

    for metadata in VERSION package-info.json MANIFEST.sha256 README.md; do
        chown root:root "${TARGET_ROOT}/${metadata}"
        chmod 0644 "${TARGET_ROOT}/${metadata}"
    done
    if [ -f "${TARGET_ROOT}/MANIFEST.sha256.sig" ]; then
        chown root:root "${TARGET_ROOT}/MANIFEST.sha256.sig"
        chmod 0644 "${TARGET_ROOT}/MANIFEST.sha256.sig"
    fi

    while IFS= read -r -d '' log_file; do
        chown root:edge-controller "${log_file}"
        chmod 0640 "${log_file}"
    done < <(find "${TARGET_ROOT}/log" -maxdepth 1 -type f \
        \( -name 'edge-controller.log' -o -name 'edge-controller.log.[0-9]*' \
           -o -name 'edge-web.log' -o -name 'edge-web.log.[0-9]*' \
           -o -name 'update.log' -o -name 'update.log.[0-9]*' \) -print0)
}

ensure_environment_key()
{
    environment_file="$1"
    key="$2"
    default_value="$3"
    if ! grep -E "^[[:space:]]*${key}=" "${environment_file}" >/dev/null; then
        printf '\n%s=%s\n' "${key}" "${default_value}" >> "${environment_file}"
        log "已为现有环境文件补入缺失参数：${key}=${default_value}"
    fi
}

path_is_within()
{
    candidate="$1"
    root="$2"
    case "${candidate}" in
        "${root}"|"${root}"/*) return 0 ;;
        *) return 1 ;;
    esac
}

validate_aarch64()
{
    file_path="$1"
    description="$2"
    resolved="$(readlink -f -- "${file_path}" 2>/dev/null || true)"
    [ -n "${resolved}" ] && [ -f "${resolved}" ] || die "${description}缺失或符号链接无效：${file_path}"
    path_is_within "${resolved}" "${SOURCE_ROOT}" || die "${description}解析后越出发布目录：${file_path} -> ${resolved}"
    header="$(LC_ALL=C readelf -hW "${resolved}" 2>&1)" || die "${description}不是有效 ELF：${resolved}"
    printf '%s\n' "${header}" | grep -F 'Class:                             ELF64' >/dev/null ||
        die "${description}不是 ELF64：${resolved}"
    printf '%s\n' "${header}" | grep -F 'Machine:                           AArch64' >/dev/null ||
        die "${description}不是 AArch64：${resolved}"
}

validate_release()
{
    [ -f "${SOURCE_ROOT}/VERSION" ] || die "发布包缺少 VERSION：${SOURCE_ROOT}/VERSION"
    [ -s "${SOURCE_ROOT}/package-info.json" ] || die "发布包缺少 package-info.json"
    [ -s "${SOURCE_ROOT}/MANIFEST.sha256" ] || die "发布包缺少 MANIFEST.sha256"
    [ -x "${SOURCE_ROOT}/scripts/rotate-logs.sh" ] || die "发布包缺少可执行日志轮转脚本"
    [ -f "${SOURCE_ROOT}/systemd/edge-log-rotator.service" ] || die "发布包缺少日志轮转 systemd 服务"
    [ -x "${SOURCE_ROOT}/scripts/update-manager.py" ] || die "发布包缺少可执行升级引擎"
    [ -f "${SOURCE_ROOT}/systemd/edge-upgrade@.service" ] || die "发布包缺少独立升级 systemd 服务"
    [ -f "${SOURCE_ROOT}/systemd/edge-upgrade-recovery.service" ] || die "发布包缺少启动恢复 systemd 服务"
    [ -f "${SOURCE_ROOT}/systemd/edge-upgrade-recovery-verify.service" ] || die "发布包缺少恢复健康确认 systemd 服务"
    version="$(tr -d '\r\n' < "${SOURCE_ROOT}/VERSION")"
    [ -n "${version}" ] || die "VERSION 为空"

    python3 "${SOURCE_ROOT}/scripts/update-manager.py" \
        validate-root --root "${SOURCE_ROOT}" >/dev/null ||
        die "发布包契约、MANIFEST、ELF 或动态库校验失败"
    grep -F -x 'EDGE_CONTROLLER_REQUIRE_EXPLICIT_NETWORK_CONFIG=1' \
        "${SOURCE_ROOT}/config/edge-controller.env.default" >/dev/null ||
        die "RK3562 默认环境未启用首次网络保护"

    log "校验发布包完整性"
    (cd "${SOURCE_ROOT}" && sha256sum -c MANIFEST.sha256) || die "MANIFEST.sha256 校验失败"
    while IFS= read -r packaged_path; do
        relative_path="${packaged_path#./}"
        grep -F "  ./${relative_path}" "${SOURCE_ROOT}/MANIFEST.sha256" >/dev/null ||
            die "MANIFEST.sha256 未覆盖文件：${relative_path}"
    done < <(cd "${SOURCE_ROOT}" && find . \( -type f -o -type l \) \
        ! -name MANIFEST.sha256 ! -name MANIFEST.sha256.sig -print | LC_ALL=C sort)

    for required_path in \
        bin/edge-controller \
        bin/edge-web \
        lib/libmosquitto.so.1 \
        lib/libmosquitto.so.2.0.18 \
        lib/libssl.so.3 \
        lib/libcrypto.so.3 \
        lib/libstdc++.so.6 \
        lib/libgcc_s.so.1; do
        validate_aarch64 "${SOURCE_ROOT}/${required_path}" "${required_path}"
    done
    [ -x "${SOURCE_ROOT}/bin/edge-controller" ] && [ -x "${SOURCE_ROOT}/bin/edge-web" ] ||
        die "发布包关键二进制不可执行"
    [ -L "${SOURCE_ROOT}/lib/libmosquitto.so.1" ] &&
        [ "$(readlink -- "${SOURCE_ROOT}/lib/libmosquitto.so.1")" = "libmosquitto.so.2.0.18" ] ||
        die "libmosquitto.so.1 未正确链接到 libmosquitto.so.2.0.18"

    controller_program="$(LC_ALL=C readelf -lW "${SOURCE_ROOT}/bin/edge-controller")"
    controller_dynamic="$(LC_ALL=C readelf -dW "${SOURCE_ROOT}/bin/edge-controller")"
    printf '%s\n' "${controller_program}" | grep -F '/lib/ld-linux-aarch64.so.1' >/dev/null ||
        die "edge-controller 动态解释器错误"
    printf '%s\n' "${controller_dynamic}" | grep -F 'Shared library: [libmosquitto.so.1]' >/dev/null ||
        die "edge-controller 缺少 libmosquitto.so.1 NEEDED"
    printf '%s\n' "${controller_dynamic}" | grep -F '$ORIGIN/../lib' >/dev/null ||
        die "edge-controller RPATH/RUNPATH 错误"

    web_program="$(LC_ALL=C readelf -lW "${SOURCE_ROOT}/bin/edge-web")"
    web_dynamic="$(LC_ALL=C readelf -dW "${SOURCE_ROOT}/bin/edge-web" 2>/dev/null || true)"
    ! printf '%s\n' "${web_program}" | grep -F 'INTERP' >/dev/null || die "edge-web 不是静态 ELF"
    ! printf '%s\n' "${web_dynamic}" | grep -F '(NEEDED)' >/dev/null || die "edge-web 不是静态 ELF"

    for soname_pair in \
        'libmosquitto.so.1:libmosquitto.so.1' \
        'libssl.so.3:libssl.so.3' \
        'libcrypto.so.3:libcrypto.so.3' \
        'libstdc++.so.6:libstdc++.so.6' \
        'libgcc_s.so.1:libgcc_s.so.1'; do
        library_name="${soname_pair%%:*}"
        expected_soname="${soname_pair#*:}"
        library_dynamic="$(LC_ALL=C readelf -dW "${SOURCE_ROOT}/lib/${library_name}")"
        printf '%s\n' "${library_dynamic}" | grep -F "Library soname: [${expected_soname}]" >/dev/null ||
            die "${library_name} SONAME 错误，期望 ${expected_soname}"
    done
    mosquitto_dynamic="$(LC_ALL=C readelf -dW "${SOURCE_ROOT}/lib/libmosquitto.so.1")"
    printf '%s\n' "${mosquitto_dynamic}" | grep -F 'Shared library: [libssl.so.3]' >/dev/null &&
        printf '%s\n' "${mosquitto_dynamic}" | grep -F 'Shared library: [libcrypto.so.3]' >/dev/null ||
        die "libmosquitto 缺少 OpenSSL TLS 依赖，可能为 WITH_TLS=OFF 构建"

    if find "${SOURCE_ROOT}" -type f \
        \( -iname '*.pem' -o -iname '*.key' -o -iname '*.crt' -o -iname '*.p12' -o -iname '*.pfx' \) \
        -print -quit | grep . >/dev/null; then
        die "基础发布包中禁止包含 MQTT TLS 证书或私钥"
    fi
    log "发布包校验通过：version=${version}"
}

ensure_identity()
{
    if ! getent group edge-controller >/dev/null; then
        groupadd --system edge-controller
        log "已创建系统组 edge-controller"
    fi

    if id edge-web >/dev/null 2>&1; then
        [ "$(id -gn edge-web)" = "edge-controller" ] ||
            die "现有 edge-web 用户的主组不是 edge-controller，拒绝修改未知账号"
        [ "$(getent passwd edge-web | cut -d: -f7)" = "/usr/sbin/nologin" ] ||
            die "现有 edge-web 用户不是非登录账号，拒绝修改未知账号"
        user_groups="$(id -nG edge-web)"
        [ "${user_groups}" = "edge-controller" ] ||
            die "现有 edge-web 用户属于额外系统组：${user_groups}"
    else
        useradd --system --gid edge-controller --shell /usr/sbin/nologin --no-create-home edge-web
        log "已创建非登录用户 edge-web"
    fi
    password_status="$(passwd -S edge-web 2>/dev/null | awk '{print $2}' || true)"
    [ "${password_status}" = "L" ] ||
        die "edge-web 用户密码未锁定，拒绝使用可登录账号"
}

install_payload()
{
    source_real="$(readlink -f -- "${SOURCE_ROOT}")"
    target_real="$(readlink -f -- "${TARGET_ROOT}" 2>/dev/null || true)"
    [ "${source_real}" != "${target_real}" ] || die "请从解压目录运行安装脚本，不能从 ${TARGET_ROOT} 自覆盖安装"

    if [ "${RECOVERY_MODE}" -eq 1 ]; then
        for service_name in edge-web.service edge-controller.service edge-log-rotator.service; do
            if systemctl is-active --quiet "${service_name}"; then
                systemctl stop "${service_name}" >/dev/null 2>&1 || true
            fi
        done
    else
        systemctl stop edge-log-rotator.service >/dev/null 2>&1 || true
        systemctl stop edge-web.service edge-controller.service >/dev/null 2>&1 || true
    fi

    install -d -o root -g edge-controller -m 0750 "${TARGET_ROOT}"
    for managed_dir in bin lib systemd scripts; do
        rm -rf -- "${TARGET_ROOT:?}/${managed_dir}"
        cp -a -- "${SOURCE_ROOT}/${managed_dir}" "${TARGET_ROOT}/${managed_dir}"
    done

    install -d -o root -g edge-controller -m 0750 "${TARGET_ROOT}/config"
    install -o root -g edge-controller -m 0640 \
        "${SOURCE_ROOT}/config/edge-controller.env.default" \
        "${TARGET_ROOT}/config/edge-controller.env.default"
    if [ ! -e "${TARGET_ROOT}/config/edge-controller.env" ]; then
        install -o root -g edge-controller -m 0640 \
            "${SOURCE_ROOT}/config/edge-controller.env.default" \
            "${TARGET_ROOT}/config/edge-controller.env"
        log "已从默认值创建正式环境文件"
    else
        log "保留已有正式环境文件：${TARGET_ROOT}/config/edge-controller.env"
    fi
    ensure_environment_key "${TARGET_ROOT}/config/edge-controller.env" EDGE_LOG_MAX_BYTES 5242880
    ensure_environment_key "${TARGET_ROOT}/config/edge-controller.env" EDGE_LOG_BACKUPS 3
    ensure_environment_key "${TARGET_ROOT}/config/edge-controller.env" EDGE_LOG_ROTATE_INTERVAL_SECONDS 60
    ensure_environment_key "${TARGET_ROOT}/config/edge-controller.env" EDGE_CONTROLLER_REQUIRE_EXPLICIT_NETWORK_CONFIG 1
    ensure_environment_key "${TARGET_ROOT}/config/edge-controller.env" EDGE_UPGRADE_SIGNATURE_POLICY optional
    ensure_environment_key "${TARGET_ROOT}/config/edge-controller.env" EDGE_UPGRADE_PUBLIC_KEY /opt/edge-controller/security/upgrade-ed25519.pub

    install -d -o root -g root -m 0750 "${TARGET_ROOT}/data"
    install -d -o root -g edge-controller -m 0750 "${TARGET_ROOT}/log"
    install -d -o root -g edge-controller -m 0770 "${TARGET_ROOT}/run"
    install -d -o root -g edge-controller -m 0750 \
        "${TARGET_ROOT}/update" "${TARGET_ROOT}/update/incoming"
    install -d -o root -g edge-controller -m 0770 "${TARGET_ROOT}/update/upload"
    install -d -o root -g root -m 0700 \
        "${TARGET_ROOT}/update/jobs" "${TARGET_ROOT}/update/backup"
    install -d -o root -g root -m 0700 "${TARGET_ROOT}/update/recovery"
    install -d -o root -g root -m 0750 "${TARGET_ROOT}/security"
    install -o root -g root -m 0700 \
        "${TARGET_ROOT}/scripts/update-manager.py" \
        "${TARGET_ROOT}/update/recovery/.update-manager.py.new"
    mv -f -- \
        "${TARGET_ROOT}/update/recovery/.update-manager.py.new" \
        "${TARGET_ROOT}/update/recovery/update-manager.py"
    rm -f -- \
        "${TARGET_ROOT}/run/edge-controller.sock" \
        "${TARGET_ROOT}/run/edge-controller.sock.lock" \
        "${TARGET_ROOT}/run/ntpd.pid" \
        "${TARGET_ROOT}/run"/udhcpc-*.pid
    printf 'interface=%s\nip_address=%s\ngateway=%s\ndata_was_fresh=%s\n' \
        "${PRE_START_INTERFACE}" "${PRE_START_IP_ADDRESS}" "${PRE_START_GATEWAY}" "${PRE_START_DATA_WAS_FRESH}" \
        > "${TARGET_ROOT}/run/network-before-first-start.state"
    chown root:root "${TARGET_ROOT}/run/network-before-first-start.state"
    chmod 0600 "${TARGET_ROOT}/run/network-before-first-start.state"

    for log_name in edge-controller.log edge-web.log update.log; do
        [ ! -L "${TARGET_ROOT}/log/${log_name}" ] || die "日志路径不能是符号链接：${TARGET_ROOT}/log/${log_name}"
        [ ! -e "${TARGET_ROOT}/log/${log_name}" ] || [ -f "${TARGET_ROOT}/log/${log_name}" ] ||
            die "日志路径不是常规文件：${TARGET_ROOT}/log/${log_name}"
        if [ ! -e "${TARGET_ROOT}/log/${log_name}" ]; then
            install -o root -g edge-controller -m 0640 /dev/null "${TARGET_ROOT}/log/${log_name}"
        fi
    done

    for metadata in VERSION package-info.json MANIFEST.sha256 README.md; do
        install -o root -g root -m 0644 "${SOURCE_ROOT}/${metadata}" "${TARGET_ROOT}/${metadata}"
    done
    if [ -f "${SOURCE_ROOT}/MANIFEST.sha256.sig" ]; then
        install -o root -g root -m 0644 \
            "${SOURCE_ROOT}/MANIFEST.sha256.sig" "${TARGET_ROOT}/MANIFEST.sha256.sig"
    else
        rm -f -- "${TARGET_ROOT}/MANIFEST.sha256.sig"
    fi

    set_install_permissions
    "${TARGET_ROOT}/scripts/update-manager.py" status >/dev/null
}

register_services()
{
    rm -f -- \
        /etc/systemd/system/edge-controller.service \
        /etc/systemd/system/edge-web.service \
        /etc/systemd/system/edge-log-rotator.service
    ln -s /opt/edge-controller/systemd/edge-controller.service /etc/systemd/system/edge-controller.service
    ln -s /opt/edge-controller/systemd/edge-web.service /etc/systemd/system/edge-web.service
    ln -s /opt/edge-controller/systemd/edge-log-rotator.service /etc/systemd/system/edge-log-rotator.service
    rm -f -- /etc/systemd/system/edge-upgrade@.service
    install -o root -g root -m 0644 \
        "${TARGET_ROOT}/systemd/edge-upgrade@.service" \
        /etc/systemd/system/edge-upgrade@.service
    rm -f -- \
        /etc/systemd/system/edge-upgrade-recovery.service \
        /etc/systemd/system/edge-upgrade-recovery-verify.service
    install -o root -g root -m 0644 \
        "${TARGET_ROOT}/systemd/edge-upgrade-recovery.service" \
        /etc/systemd/system/edge-upgrade-recovery.service
    install -o root -g root -m 0644 \
        "${TARGET_ROOT}/systemd/edge-upgrade-recovery-verify.service" \
        /etc/systemd/system/edge-upgrade-recovery-verify.service
    for service_name in edge-controller.service edge-web.service edge-log-rotator.service; do
        gate_directory="/etc/systemd/system/${service_name}.d"
        gate_file="${gate_directory}/edge-upgrade-recovery-gate.conf"
        gate_temporary="${gate_directory}/.edge-upgrade-recovery-gate.conf.new"
        install -d -o root -g root -m 0755 "${gate_directory}"
        install -o root -g root -m 0644 /dev/null "${gate_temporary}"
        {
            printf '%s\n' '[Service]'
            printf '%s\n' 'ExecCondition='
            printf '%s\n' 'ExecCondition=+/usr/bin/python3 /opt/edge-controller/update/recovery/update-manager.py service-start-allowed'
        } > "${gate_temporary}"
        mv -f -- "${gate_temporary}" "${gate_file}"
    done
    systemctl daemon-reload
    systemctl enable \
        edge-upgrade-recovery.service edge-upgrade-recovery-verify.service \
        edge-controller.service edge-web.service edge-log-rotator.service

    if [ "${NO_START}" -eq 0 ]; then
        systemctl restart edge-controller.service
        systemctl restart edge-web.service
        systemctl restart edge-log-rotator.service
        log "服务已启动"
    else
        log "--no-start：服务已安装并启用，但未启动"
    fi
}

main()
{
    [ "$(id -u)" -eq 0 ] || die "安装脚本仅允许 root 执行"
    case "${RECOVERY_MODE}" in
        0|1) ;;
        *) die "EDGE_UPGRADE_RECOVERY_MODE 只能为 0 或 1" ;;
    esac
    case "${1:-}" in
        '') ;;
        --no-start) NO_START=1 ;;
        *) die "用法：install.sh [--no-start]" ;;
    esac
    [ "$#" -le 1 ] || die "用法：install.sh [--no-start]"

    for command_name in readelf readlink sha256sum find sort grep tr getent groupadd useradd id cut passwd awk stat \
        install cp mv rm chown chmod ln systemctl ip python3; do
        need_cmd "${command_name}"
    done

    validate_release
    ensure_identity
    capture_pre_start_state
    install_payload
    register_services
    if [ "${NO_START}" -eq 0 ]; then
        "${TARGET_ROOT}/scripts/verify-install.sh"
    fi
    log "安装完成：${TARGET_ROOT}；可运行 ${TARGET_ROOT}/scripts/verify-install.sh 验证"
}

main "$@"
