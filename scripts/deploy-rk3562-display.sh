#!/usr/bin/env bash

# 复用公共 RK3562 部署入口，并在其成功后安装 kickpi 桌面 Kiosk 资产。
set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
COMMON_DEPLOY="${SCRIPT_DIR}/deploy-rk3562.sh"
DISPLAY_ASSET_DIR="${SCRIPT_DIR}/rk3562/display"
DISPLAY_USER="kickpi"
DISPLAY_HOME="/home/kickpi"

log()
{
    printf '[deploy-rk3562-display] %s\n' "$*"
}

die()
{
    log "ERROR: $*" >&2
    exit 1
}

usage()
{
    cat <<'EOF'
用法：
  sudo bash ./deploy-rk3562-display.sh [发布包路径]
  sudo bash ./deploy-rk3562-display.sh [--package <发布包路径>] [--no-start]

说明：
  先调用同目录 deploy-rk3562.sh 完成公共后台部署；成功后安装 kickpi Kiosk。
  显示版入口不支持 --verify-only；公共后台可继续用 deploy-rk3562.sh --verify-only 验证。
EOF
}

reject_unsupported_arguments()
{
    for argument in "$@"; do
        case "${argument}" in
            --help|-h)
                usage
                exit 0
                ;;
            --verify-only)
                die "显示版入口不支持 --verify-only；请使用 deploy-rk3562.sh --verify-only"
                ;;
        esac
    done
}

install_display_assets()
{
    getent passwd "${DISPLAY_USER}" >/dev/null 2>&1 ||
        die "系统不存在显示桌面用户：${DISPLAY_USER}"
    [ "$(getent passwd "${DISPLAY_USER}" | cut -d: -f6)" = "${DISPLAY_HOME}" ] ||
        die "${DISPLAY_USER} 的 home 不是 ${DISPLAY_HOME}，拒绝安装到错误用户目录"

    if command -v chromium >/dev/null 2>&1; then
        chromium_path="$(command -v chromium)"
    elif command -v chromium-browser >/dev/null 2>&1; then
        chromium_path="$(command -v chromium-browser)"
    else
        die "系统未安装 chromium 或 chromium-browser"
    fi

    kiosk_script="${DISPLAY_ASSET_DIR}/start-edge-kiosk.sh"
    desktop_file="${DISPLAY_ASSET_DIR}/edge-kiosk.desktop"
    [ -f "${kiosk_script}" ] && [ ! -L "${kiosk_script}" ] ||
        die "Kiosk 启动脚本缺失或不安全：${kiosk_script}"
    [ -f "${desktop_file}" ] && [ ! -L "${desktop_file}" ] ||
        die "Kiosk autostart 文件缺失或不安全：${desktop_file}"

    display_uid="$(id -u "${DISPLAY_USER}")"
    display_gid="$(id -g "${DISPLAY_USER}")"

    install -d -o "${display_uid}" -g "${display_gid}" -m 0755 \
        "${DISPLAY_HOME}/.local" \
        "${DISPLAY_HOME}/.local/bin" \
        "${DISPLAY_HOME}/.local/share" \
        "${DISPLAY_HOME}/.config" \
        "${DISPLAY_HOME}/.config/autostart"
    install -d -o "${display_uid}" -g "${display_gid}" -m 0700 \
        "${DISPLAY_HOME}/.config/edge-kiosk-chromium"

    install -o "${display_uid}" -g "${display_gid}" -m 0755 \
        "${kiosk_script}" "${DISPLAY_HOME}/.local/bin/start-edge-kiosk.sh"
    install -o "${display_uid}" -g "${display_gid}" -m 0644 \
        "${desktop_file}" "${DISPLAY_HOME}/.config/autostart/edge-kiosk.desktop"

    kiosk_log="${DISPLAY_HOME}/.local/share/edge-kiosk.log"
    if [ -e "${kiosk_log}" ] || [ -L "${kiosk_log}" ]; then
        [ -f "${kiosk_log}" ] && [ ! -L "${kiosk_log}" ] ||
            die "Kiosk 日志路径不是安全的常规文件：${kiosk_log}"
        chown "${display_uid}:${display_gid}" "${kiosk_log}"
        chmod 0640 "${kiosk_log}"
    else
        install -o "${display_uid}" -g "${display_gid}" -m 0640 /dev/null "${kiosk_log}"
    fi

    log "Kiosk 已安装：Chromium=${chromium_path}"
    log "启动脚本：${DISPLAY_HOME}/.local/bin/start-edge-kiosk.sh"
    log "桌面自启动：${DISPLAY_HOME}/.config/autostart/edge-kiosk.desktop"
    log "现有 Chromium profile 和其他 autostart 文件均已保留"
}

main()
{
    reject_unsupported_arguments "$@"
    [ "$(id -u)" -eq 0 ] || die "必须以 root 运行：sudo bash ./deploy-rk3562-display.sh"
    [ -f "${COMMON_DEPLOY}" ] && [ ! -L "${COMMON_DEPLOY}" ] ||
        die "公共部署入口缺失或不安全：${COMMON_DEPLOY}"

    log "开始公共后台部署"
    if bash "${COMMON_DEPLOY}" "$@"; then
        :
    else
        common_status="$?"
        die "公共后台部署失败（exit=${common_status}），未安装 Kiosk"
    fi
    log "公共后台部署成功，开始安装显示终端资产"
    install_display_assets
    log "显示终端部署完成；重启并登录 kickpi 桌面后将自动进入 Kiosk"
}

main "$@"
