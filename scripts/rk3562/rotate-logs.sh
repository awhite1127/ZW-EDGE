#!/usr/bin/env bash

# 在不替换已打开 inode 的前提下轮转产品日志和应用升级日志。
set -euo pipefail

ENV_FILE="/opt/edge-controller/config/edge-controller.env"
DEFAULT_LOG_DIR="/opt/edge-controller/log"
DEFAULT_MAX_BYTES=5242880
DEFAULT_BACKUPS=3
DEFAULT_INTERVAL_SECONDS=60
STOP_REQUESTED=0
SLEEP_PID=""

log()
{
    printf '[edge-log-rotator] %s\n' "$*"
}

die()
{
    printf '[edge-log-rotator] ERROR: %s\n' "$*" >&2
    exit 1
}

read_environment_value()
{
    key="$1"
    [ -f "${ENV_FILE}" ] || return 0
    value="$(LC_ALL=C sed -n "s/^[[:space:]]*${key}=//p" "${ENV_FILE}" | tail -n 1)"
    case "${value}" in
        \"*\") value="${value#\"}"; value="${value%\"}" ;;
        \'*\') value="${value#\'}"; value="${value%\'}" ;;
    esac
    printf '%s\n' "${value}"
}

positive_integer_or_default()
{
    value="$1"
    fallback="$2"
    label="$3"
    case "${value}" in
        ''|*[!0-9]*)
            log "${label}=${value:-<空>} 非法，使用默认值 ${fallback}" >&2
            printf '%s\n' "${fallback}"
            ;;
        *)
            while [ "${value#0}" != "${value}" ]; do value="${value#0}"; done
            if [ -z "${value}" ]; then
                log "${label}=0 非法，使用默认值 ${fallback}" >&2
                printf '%s\n' "${fallback}"
            else
                printf '%s\n' "${value}"
            fi
            ;;
    esac
}

bounded_integer_or_default()
{
    value="$1"
    minimum="$2"
    maximum="$3"
    fallback="$4"
    label="$5"
    case "${value}" in
        ''|*[!0-9]*) valid=0 ;;
        *)
            while [ "${value#0}" != "${value}" ]; do value="${value#0}"; done
            [ -n "${value}" ] || value=0
            if [ "${value}" -ge "${minimum}" ] && [ "${value}" -le "${maximum}" ]; then
                valid=1
            else
                valid=0
            fi
            ;;
    esac
    if [ "${valid}" -eq 1 ]; then
        printf '%s\n' "${value}"
    else
        log "${label}=${value:-<空>} 超出 ${minimum}～${maximum}，使用默认值 ${fallback}" >&2
        printf '%s\n' "${fallback}"
    fi
}

decimal_greater_than()
{
    left="$1"
    right="$2"
    while [ "${left#0}" != "${left}" ]; do left="${left#0}"; done
    while [ "${right#0}" != "${right}" ]; do right="${right#0}"; done
    [ -n "${left}" ] || left=0
    [ -n "${right}" ] || right=0
    if [ "${#left}" -gt "${#right}" ]; then
        return 0
    fi
    if [ "${#left}" -lt "${#right}" ]; then
        return 1
    fi
    [[ "${left}" > "${right}" ]]
}

load_configuration()
{
    file_log_dir="$(read_environment_value EDGE_CONTROLLER_LOG_DIR)"
    file_max_bytes="$(read_environment_value EDGE_LOG_MAX_BYTES)"
    file_backups="$(read_environment_value EDGE_LOG_BACKUPS)"
    file_interval="$(read_environment_value EDGE_LOG_ROTATE_INTERVAL_SECONDS)"

    LOG_DIR="${EDGE_CONTROLLER_LOG_DIR:-${file_log_dir:-${DEFAULT_LOG_DIR}}}"
    MAX_BYTES="$(positive_integer_or_default \
        "${EDGE_LOG_MAX_BYTES:-${file_max_bytes:-}}" "${DEFAULT_MAX_BYTES}" EDGE_LOG_MAX_BYTES)"
    # 0 表示只截断当前日志、不保留副本；嵌入式设备可借此严格限制日志占用空间。
    BACKUPS="$(bounded_integer_or_default \
        "${EDGE_LOG_BACKUPS:-${file_backups:-}}" 0 10 "${DEFAULT_BACKUPS}" EDGE_LOG_BACKUPS)"
    INTERVAL_SECONDS="$(bounded_integer_or_default \
        "${EDGE_LOG_ROTATE_INTERVAL_SECONDS:-${file_interval:-}}" 10 3600 \
        "${DEFAULT_INTERVAL_SECONDS}" EDGE_LOG_ROTATE_INTERVAL_SECONDS)"

    case "${LOG_DIR}" in
        /*) ;;
        *) die "EDGE_CONTROLLER_LOG_DIR 必须是绝对路径：${LOG_DIR}" ;;
    esac
    [ -d "${LOG_DIR}" ] || die "日志目录不存在：${LOG_DIR}"
}

normalize_log_permissions()
{
    log_file="$1"
    [ -e "${log_file}" ] || return 0
    [ ! -L "${log_file}" ] && [ -f "${log_file}" ] || die "日志或备份不是常规文件：${log_file}"
    chown root:edge-controller "${log_file}"
    chmod 0640 "${log_file}"
}

validate_log_paths()
{
    current_log="$1"
    [ ! -L "${current_log}" ] || die "当前日志不能是符号链接：${current_log}"
    [ ! -e "${current_log}" ] || [ -f "${current_log}" ] || die "当前日志不是常规文件：${current_log}"
    for ((index = 1; index <= 10; ++index)); do
        backup="${current_log}.${index}"
        [ ! -L "${backup}" ] || die "日志备份不能是符号链接：${backup}"
        [ ! -e "${backup}" ] || [ -f "${backup}" ] || die "日志备份不是常规文件：${backup}"
    done
}

rotate_log_if_needed()
{
    current_log="$1"
    validate_log_paths "${current_log}"
    if [ ! -e "${current_log}" ]; then
        : > "${current_log}"
    fi

    current_size="$(stat -c '%s' -- "${current_log}")"
    for ((index = 10; index > BACKUPS; --index)); do
        rm -f -- "${current_log}.${index}"
    done
    if decimal_greater_than "${current_size}" "${MAX_BYTES}"; then
        if [ "${BACKUPS}" -gt 0 ]; then
            for ((index = BACKUPS; index >= 2; --index)); do
                previous=$((index - 1))
                if [ -f "${current_log}.${previous}" ]; then
                    cp -- "${current_log}.${previous}" "${current_log}.${index}"
                else
                    rm -f -- "${current_log}.${index}"
                fi
            done
            cp -- "${current_log}" "${current_log}.1"
        fi
        : > "${current_log}"
        log "已轮转 $(basename -- "${current_log}")：原大小=${current_size} bytes；保留=${BACKUPS}"
    fi

    normalize_log_permissions "${current_log}"
    for ((index = 1; index <= BACKUPS; ++index)); do
        normalize_log_permissions "${current_log}.${index}"
    done
}

rotate_once()
{
    rotate_log_if_needed "${LOG_DIR}/edge-controller.log"
    rotate_log_if_needed "${LOG_DIR}/edge-web.log"
    rotate_log_if_needed "${LOG_DIR}/update.log"
}

rotate_with_lock()
{
    flock -x 9
    rotate_once
    flock -u 9
}

handle_stop()
{
    STOP_REQUESTED=1
    if [ -n "${SLEEP_PID}" ]; then
        kill "${SLEEP_PID}" 2>/dev/null || true
    fi
}

main()
{
    case "${1:-}" in
        --once) mode=once ;;
        --loop) mode=loop ;;
        *) die "用法：rotate-logs.sh --once|--loop" ;;
    esac
    [ "$#" -eq 1 ] || die "用法：rotate-logs.sh --once|--loop"

    for command_name in sed tail flock stat cp rm chown chmod basename sleep kill; do
        command -v "${command_name}" >/dev/null 2>&1 || die "缺少命令：${command_name}"
    done
    load_configuration

    exec 9>"${LOG_DIR}/.rotate-logs.lock"
    chown root:edge-controller "${LOG_DIR}/.rotate-logs.lock"
    chmod 0660 "${LOG_DIR}/.rotate-logs.lock"

    trap handle_stop SIGTERM SIGINT
    if [ "${mode}" = "once" ]; then
        rotate_with_lock
        return
    fi

    while [ "${STOP_REQUESTED}" -eq 0 ]; do
        rotate_with_lock
        [ "${STOP_REQUESTED}" -eq 0 ] || break
        sleep "${INTERVAL_SECONDS}" &
        SLEEP_PID=$!
        wait "${SLEEP_PID}" || true
        SLEEP_PID=""
    done
    log "收到停止信号，日志轮转服务退出"
}

main "$@"
