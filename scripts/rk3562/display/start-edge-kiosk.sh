#!/usr/bin/env bash

# kickpi 图形会话中的本地 Chromium Kiosk 启动器。
set -eu

KIOSK_URL="http://127.0.0.1:8080"
PROFILE_DIR="${HOME}/.config/edge-kiosk-chromium"
LOG_DIR="${HOME}/.local/share"
LOG_FILE="${LOG_DIR}/edge-kiosk.log"
CURRENT_UID="$(id -u)"

mkdir -p "${PROFILE_DIR}" "${LOG_DIR}"
chmod 0700 "${PROFILE_DIR}"
touch "${LOG_FILE}"
chmod 0640 "${LOG_FILE}"
exec >>"${LOG_FILE}" 2>&1

log()
{
    printf '[%s] [edge-kiosk] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"
}

if command -v chromium >/dev/null 2>&1; then
    CHROMIUM="$(command -v chromium)"
elif command -v chromium-browser >/dev/null 2>&1; then
    CHROMIUM="$(command -v chromium-browser)"
else
    log "ERROR: 未找到 chromium 或 chromium-browser"
    exit 1
fi

log "清理当前用户的旧 Chromium 进程"
pkill -TERM -u "${CURRENT_UID}" -f '(^|/)(chromium|chromium-browser)( |$)' 2>/dev/null || true
sleep 1
pkill -KILL -u "${CURRENT_UID}" -f '(^|/)(chromium|chromium-browser)( |$)' 2>/dev/null || true

rm -f -- \
    "${PROFILE_DIR}/SingletonLock" \
    "${PROFILE_DIR}/SingletonSocket" \
    "${PROFILE_DIR}/SingletonCookie"

log "等待 edge-web：${KIOSK_URL}"
until curl -fsS --max-time 3 -o /dev/null "${KIOSK_URL}"; do
    sleep 2
done

log "edge-web 已可访问，启动 Chromium Kiosk：${CHROMIUM}"
exec "${CHROMIUM}" \
    --user-data-dir="${PROFILE_DIR}" \
    --kiosk \
    --no-first-run \
    --no-default-browser-check \
    --password-store=basic \
    --noerrdialogs \
    --disable-session-crashed-bubble \
    --hide-crash-restore-bubble \
    --disable-infobars \
    --disable-translate \
    --disable-features=Translate,TranslateUI,OverscrollHistoryNavigation \
    --overscroll-history-navigation=0 \
    --disable-pinch \
    "${KIOSK_URL}"
