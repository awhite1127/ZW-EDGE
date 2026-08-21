#!/usr/bin/env bash

# 安装、启动并验证本地 RK3562 发布包；脚本不执行任何网络传输。
set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
TARGET_ROOT="/opt/edge-controller"
INITIAL_LOG="/tmp/edge-controller-deploy.$$.log"
FINAL_LOG="${TARGET_ROOT}/log/deploy.log"
CURRENT_STAGE="初始化"
TEMP_DIR=""
PACKAGE_PATH=""
NO_START=0
VERIFY_ONLY=0
FIRST_INSTALL=0
PRE_INTERFACE=""
PRE_IP_ADDRESS=""
PRE_DEFAULT_ROUTES=""
POST_INTERFACE=""
POST_IP_ADDRESS=""
POST_DEFAULT_ROUTES=""

: > "${INITIAL_LOG}"
chmod 0600 "${INITIAL_LOG}"
exec > >(tee -a "${INITIAL_LOG}") 2>&1

log()
{
    printf '[deploy-rk3562] %s\n' "$*"
}

die()
{
    log "ERROR: $*" >&2
    return 1
}

stage()
{
    CURRENT_STAGE="$1"
    log "阶段：${CURRENT_STAGE}"
}

need_cmd()
{
    command -v "$1" >/dev/null 2>&1 || die "缺少命令：$1"
}

persist_log()
{
    if [ -d "${TARGET_ROOT}/log" ]; then
        cp -- "${INITIAL_LOG}" "${FINAL_LOG}" 2>/dev/null || return 0
        chown root:edge-controller "${FINAL_LOG}" 2>/dev/null || true
        chmod 0640 "${FINAL_LOG}" 2>/dev/null || true
    fi
}

cleanup()
{
    status="$?"
    set +e
    if [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}" ]; then
        case "${TEMP_DIR}" in
            /tmp/edge-controller-deploy.*) rm -rf -- "${TEMP_DIR}" ;;
            *) log "WARNING: 拒绝清理非预期临时目录：${TEMP_DIR}" ;;
        esac
    fi
    if [ -d "${TARGET_ROOT}/log" ]; then
        log "部署日志：${FINAL_LOG}"
        persist_log
        rm -f -- "${INITIAL_LOG}"
    else
        log "部署日志：${INITIAL_LOG}"
    fi
    return "${status}"
}

on_error()
{
    status="$?"
    error_line="$1"
    error_command="$2"
    trap - ERR
    set +e
    log "ERROR: 部署失败"
    log "失败阶段：${CURRENT_STAGE}"
    log "失败命令：${error_command}"
    log "失败行号：${error_line}"
    log "建议检查：systemctl status edge-controller edge-web edge-log-rotator --no-pager"
    log "建议检查：tail -n 200 ${TARGET_ROOT}/log/edge-controller.log"
    exit "${status}"
}

usage()
{
    cat <<'EOF'
用法：
  sudo bash ./deploy-rk3562.sh [发布包路径]
  sudo bash ./deploy-rk3562.sh [--package <发布包路径>] [--no-start]
  sudo bash ./deploy-rk3562.sh --verify-only

参数：
  --package <path>  显式指定 edge-controller-rk3562-*.tar.gz
  --no-start       安装并启用服务，但保持服务停止
  --verify-only    仅验证 /opt/edge-controller 的现有安装
  --help           显示帮助
EOF
}

parse_arguments()
{
    positional_package=""
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --package)
                [ "$#" -ge 2 ] || die "--package 缺少路径参数"
                [ -z "${PACKAGE_PATH}" ] || die "发布包只能指定一次"
                PACKAGE_PATH="$2"
                shift 2
                ;;
            --no-start)
                NO_START=1
                shift
                ;;
            --verify-only)
                VERIFY_ONLY=1
                shift
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            --*) die "未知参数：$1" ;;
            *)
                [ -z "${positional_package}" ] || die "只能提供一个发布包路径"
                positional_package="$1"
                shift
                ;;
        esac
    done
    if [ -n "${positional_package}" ]; then
        [ -z "${PACKAGE_PATH}" ] || die "不能同时使用位置参数和 --package"
        PACKAGE_PATH="${positional_package}"
    fi
    [ "${NO_START}" -eq 0 ] || [ "${VERIFY_ONLY}" -eq 0 ] ||
        die "--no-start 与 --verify-only 不能同时使用"
    if [ "${VERIFY_ONLY}" -eq 1 ] && [ -n "${PACKAGE_PATH}" ]; then
        die "--verify-only 不接受发布包路径"
    fi
}

capture_pre_network()
{
    PRE_DEFAULT_ROUTES="$(ip -4 route show default 2>/dev/null || true)"
    PRE_INTERFACE="$(printf '%s\n' "${PRE_DEFAULT_ROUTES}" | awk '
        NR == 1 { for (i = 1; i <= NF; ++i) if ($i == "dev" && i < NF) { print $(i + 1); exit } }')"
    if [ -n "${PRE_INTERFACE}" ]; then
        PRE_IP_ADDRESS="$(ip -4 -o addr show dev "${PRE_INTERFACE}" scope global 2>/dev/null | awk '
            NR == 1 { split($4, address, "/"); print address[1]; exit }')"
    fi
}

capture_post_network()
{
    POST_DEFAULT_ROUTES="$(ip -4 route show default 2>/dev/null || true)"
    POST_INTERFACE="$(printf '%s\n' "${POST_DEFAULT_ROUTES}" | awk '
        NR == 1 { for (i = 1; i <= NF; ++i) if ($i == "dev" && i < NF) { print $(i + 1); exit } }')"
    if [ -n "${POST_INTERFACE}" ]; then
        POST_IP_ADDRESS="$(ip -4 -o addr show dev "${POST_INTERFACE}" scope global 2>/dev/null | awk '
            NR == 1 { split($4, address, "/"); print address[1]; exit }')"
    fi
}

print_network_comparison()
{
    log "部署前网络：网口=${PRE_INTERFACE:-<无>}，IPv4=${PRE_IP_ADDRESS:-<无>}"
    log "部署前默认路由：${PRE_DEFAULT_ROUTES:-<无>}"
    log "部署后网络：网口=${POST_INTERFACE:-<无>}，IPv4=${POST_IP_ADDRESS:-<无>}"
    log "部署后默认路由：${POST_DEFAULT_ROUTES:-<无>}"
}

select_package()
{
    if [ -z "${PACKAGE_PATH}" ]; then
        shopt -s nullglob
        candidates=("${SCRIPT_DIR}"/edge-controller-rk3562-*.tar.gz)
        shopt -u nullglob
        case "${#candidates[@]}" in
            0) die "脚本目录未找到 edge-controller-rk3562-*.tar.gz；请使用 --package 指定" ;;
            1) PACKAGE_PATH="${candidates[0]}" ;;
            *)
                printf '[deploy-rk3562] 找到多个发布包：\n' >&2
                printf '  %s\n' "${candidates[@]}" >&2
                die "请使用 --package 明确指定一个发布包"
                ;;
        esac
    fi
    [ -f "${PACKAGE_PATH}" ] || die "发布包不存在：${PACKAGE_PATH}"
    PACKAGE_PATH="$(readlink -f -- "${PACKAGE_PATH}")"
    [ -n "${PACKAGE_PATH}" ] && [ -f "${PACKAGE_PATH}" ] || die "无法解析发布包路径"
    PACKAGE_DIRECTORY="$(dirname -- "${PACKAGE_PATH}")"
    PACKAGE_BASENAME="$(basename -- "${PACKAGE_PATH}")"
    case "${PACKAGE_BASENAME}" in
        edge-controller-rk3562-*.tar.gz) ;;
        *) die "发布包名称不符合 edge-controller-rk3562-*.tar.gz：${PACKAGE_BASENAME}" ;;
    esac
    log "发布包：${PACKAGE_PATH}"
}

verify_release_checksums()
{
    checksum_file="${PACKAGE_DIRECTORY}/SHA256SUMS"
    [ -f "${checksum_file}" ] || die "发布目录缺少 SHA256SUMS：${checksum_file}"
    grep -F "  ${PACKAGE_BASENAME}" "${checksum_file}" >/dev/null ||
        die "SHA256SUMS 未包含发布包：${PACKAGE_BASENAME}"
    (cd "${PACKAGE_DIRECTORY}" && sha256sum -c SHA256SUMS)
}

extract_and_validate_package()
{
    archive_listing="$(tar -tzf "${PACKAGE_PATH}")"
    [ -n "${archive_listing}" ] || die "发布包为空"
    if printf '%s\n' "${archive_listing}" | grep -E '(^/|(^|/)\.\.(/|$))' >/dev/null; then
        die "发布包包含不安全路径"
    fi
    top_levels="$(printf '%s\n' "${archive_listing}" | sed 's#/.*##' | LC_ALL=C sort -u)"
    [ "${top_levels}" = edge-controller ] ||
        die "发布包必须且只能包含 edge-controller 顶层目录：${top_levels}"

    TEMP_DIR="$(mktemp -d /tmp/edge-controller-deploy.XXXXXX)"
    tar -xzf "${PACKAGE_PATH}" -C "${TEMP_DIR}"
    EXTRACTED_ROOT="${TEMP_DIR}/edge-controller"
    [ -d "${EXTRACTED_ROOT}" ] || die "发布包缺少 edge-controller 目录"

    while IFS= read -r -d '' link_path; do
        resolved_link="$(readlink -f -- "${link_path}" 2>/dev/null || true)"
        case "${resolved_link}" in
            "${EXTRACTED_ROOT}"/*) ;;
            *) die "发布包符号链接越出顶层目录：${link_path}" ;;
        esac
    done < <(find "${EXTRACTED_ROOT}" -type l -print0)

    [ -s "${EXTRACTED_ROOT}/VERSION" ] || die "发布包缺少 VERSION"
    [ -s "${EXTRACTED_ROOT}/package-info.json" ] || die "发布包缺少 package-info.json"
    [ -s "${EXTRACTED_ROOT}/MANIFEST.sha256" ] || die "发布包缺少 MANIFEST.sha256"
    package_version="$(tr -d '\r\n' < "${EXTRACTED_ROOT}/VERSION")"
    [ -n "${package_version}" ] || die "VERSION 为空"
    archive_version="${PACKAGE_BASENAME#edge-controller-rk3562-}"
    archive_version="${archive_version%.tar.gz}"
    [ "${archive_version}" = "${package_version}" ] ||
        die "发布包文件名版本与 VERSION 不一致：${archive_version} != ${package_version}"
    python3 "${EXTRACTED_ROOT}/scripts/update-manager.py" \
        validate-root --root "${EXTRACTED_ROOT}" >/dev/null ||
        die "升级包契约、MANIFEST、ELF 或动态库校验失败"
}

wait_for_services()
{
    for attempt in $(seq 1 30); do
        if systemctl is-active --quiet edge-controller.service &&
           systemctl is-active --quiet edge-web.service &&
           systemctl is-active --quiet edge-log-rotator.service &&
           [ -S "${TARGET_ROOT}/run/edge-controller.sock" ] &&
           ss -ltn | grep -E '(^|[[:space:]])[^[:space:]]*:8080[[:space:]]' >/dev/null; then
            log "服务已进入稳定状态（等待 ${attempt} 秒）"
            return 0
        fi
        sleep 1
    done
    die "服务未在 30 秒内进入稳定状态"
}

verify_first_install_network()
{
    [ "${FIRST_INSTALL}" -eq 1 ] || return 0
    failed=0
    if [ -n "${PRE_IP_ADDRESS}" ] &&
       ! ip -4 -o addr show scope global | awk '{ split($4, address, "/"); print address[1] }' |
           grep -F -x "${PRE_IP_ADDRESS}" >/dev/null; then
        log "ERROR: 首次安装后原 IPv4 地址不存在：${PRE_IP_ADDRESS}"
        failed=1
    fi
    if [ -n "${PRE_DEFAULT_ROUTES}" ] && [ "${POST_DEFAULT_ROUTES}" != "${PRE_DEFAULT_ROUTES}" ]; then
        log "ERROR: 首次安装后默认路由发生变化"
        failed=1
    fi
    grep -F -x 'EDGE_CONTROLLER_REQUIRE_EXPLICIT_NETWORK_CONFIG=1' \
        "${TARGET_ROOT}/config/edge-controller.env.default" >/dev/null || {
            log "ERROR: RK3562 默认环境未启用首次网络保护"
            failed=1
        }
    grep -F '首次安装尚未确认网络配置，保留系统当前网络状态' \
        "${TARGET_ROOT}/log"/edge-controller.log* >/dev/null 2>&1 || {
            log "ERROR: 缺少首次网络保护日志"
            failed=1
        }
    if [ "${failed}" -ne 0 ]; then
        print_network_comparison
        die "首次安装网络保护验证失败；请检查 ${TARGET_ROOT}/log/edge-controller.log"
    fi
    log "首次安装网络保护验证通过：原 IPv4 和默认路由保持不变"
}

verify_installed_system()
{
    [ -x "${TARGET_ROOT}/scripts/verify-install.sh" ] ||
        die "安装验证脚本不存在：${TARGET_ROOT}/scripts/verify-install.sh"
    "${TARGET_ROOT}/scripts/verify-install.sh"
    systemctl is-active --quiet edge-controller.service || die "edge-controller 未运行"
    systemctl is-active --quiet edge-web.service || die "edge-web 未运行"
    systemctl is-active --quiet edge-log-rotator.service || die "edge-log-rotator 未运行"
    [ -S "${TARGET_ROOT}/run/edge-controller.sock" ] || die "IPC UDS 不存在"
    ss -ltn | grep -E '(^|[[:space:]])[^[:space:]]*:8080[[:space:]]' >/dev/null ||
        die "TCP 8080 未监听"
    curl -fsS --max-time 5 -o /dev/null http://127.0.0.1:8080/ ||
        die "本机 HTTP 访问失败：http://127.0.0.1:8080/"
    [ -f "${TARGET_ROOT}/data/edge-config.db" ] &&
        [ -f "${TARGET_ROOT}/data/edge-history.db" ] &&
        [ -f "${TARGET_ROOT}/data/edge-events.db" ] || die "产品数据库不完整"
    [ -f "${TARGET_ROOT}/log/edge-controller.log" ] &&
        [ -f "${TARGET_ROOT}/log/edge-web.log" ] || die "产品日志不完整"
    log "安装、服务、UDS、HTTP、数据库和日志验证通过"
}

main()
{
    trap cleanup EXIT
    trap 'on_error "${LINENO}" "${BASH_COMMAND}"' ERR
    parse_arguments "$@"
    [ "$(id -u)" -eq 0 ] || die "必须以 root 运行：sudo bash ./deploy-rk3562.sh"
    for command_name in bash id uname systemctl ip awk grep sed sort readlink dirname basename sha256sum tar mktemp find tr cp chown chmod tee ss curl seq sleep rm python3; do
        need_cmd "${command_name}"
    done
    [ "$(uname -m)" = aarch64 ] || die "系统架构必须为 aarch64，当前=$(uname -m)"
    [ -d /run/systemd/system ] || die "当前系统未由 systemd 管理"

    capture_pre_network
    [ -f "${TARGET_ROOT}/data/edge-config.db" ] || FIRST_INSTALL=1
    log "安装类型：$([ "${FIRST_INSTALL}" -eq 1 ] && printf '首次安装' || printf '重复安装')"

    if [ "${VERIFY_ONLY}" -eq 1 ]; then
        stage "验证现有安装"
        wait_for_services
        verify_installed_system
        capture_post_network
        print_network_comparison
        log "验证完成"
        return 0
    fi

    stage "选择并校验本地发布包"
    select_package
    verify_release_checksums
    stage "安全解压并验证发布包"
    extract_and_validate_package
    stage "安装到 ${TARGET_ROOT}（暂不启动）"
    bash "${EXTRACTED_ROOT}/scripts/install.sh" --no-start

    if [ "${NO_START}" -eq 1 ]; then
        capture_post_network
        print_network_comparison
        log "安装完成；--no-start 已保持三个服务停止"
        return 0
    fi

    stage "按顺序启动三个 systemd 服务"
    systemctl start edge-controller.service
    systemctl start edge-web.service
    systemctl start edge-log-rotator.service
    wait_for_services
    stage "执行安装与首次网络保护验证"
    verify_installed_system
    capture_post_network
    print_network_comparison
    verify_first_install_network
    log "部署成功：版本=$(tr -d '\r\n' < "${TARGET_ROOT}/VERSION")"
    log "本机访问：http://127.0.0.1:8080/"
}

main "$@"
