#!/usr/bin/env bash

# 构建、校验并打包一份完整的 RK3562 / AArch64 发布物。
set -Eeuo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd -P)"
BUILD_DIR="${PROJECT_ROOT}/build-arm64-edge"
BUILD_LOG_DIR="${BUILD_DIR}/logs"
RELEASE_LOG="${BUILD_LOG_DIR}/release-arm64-rk3562.log"
WEB_BUILD_LOG="${BUILD_LOG_DIR}/edge-web-build.log"
CONTROLLER_BUILD_LOG="${BUILD_DIR}/build-arm64-rk3562.log"
CONTROLLER_BUILD_SCRIPT="${SCRIPT_DIR}/build-arm64-rk3562-release.sh"
PACKAGE_SCRIPT="${SCRIPT_DIR}/package-arm64-rk3562.sh"
DEPLOY_SCRIPT="${SCRIPT_DIR}/deploy-rk3562.sh"
DISPLAY_DEPLOY_SCRIPT="${SCRIPT_DIR}/deploy-rk3562-display.sh"
DISPLAY_ASSET_DIR="${SCRIPT_DIR}/rk3562/display"
VERSION_RESOLVER="${SCRIPT_DIR}/product-version.py"

PACKAGE_VERSION_OVERRIDE="${PACKAGE_VERSION:-}"
PACKAGE_VERSION=""
PACKAGE_VERSION_SOURCE=""
BUILD_JOBS="${BUILD_JOBS:-1}"
CLEAN_BUILD="${CLEAN_BUILD:-0}"
ARM64_DEPS_ROOT="${ARM64_DEPS_ROOT:-/home/wu/dl/arm64-deps}"
RK3562_TOOLCHAIN_ROOT="${RK3562_TOOLCHAIN_ROOT:-/home/wu/sdk/kickpi-rk356x/download/rk356x-linux/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu}"
DIST_DIR="${DIST_DIR:-${PROJECT_ROOT}/dist}"
READELF_BIN="${READELF_BIN:-}"
EDGE_RELEASE_SIGNING_KEY="${EDGE_RELEASE_SIGNING_KEY:-}"
EDGE_RELEASE_SIGNING_KEY_ID="${EDGE_RELEASE_SIGNING_KEY_ID:-}"

CURRENT_STAGE="初始化"
VERIFY_DIR=""
ARCHIVE_PATH=""
RELEASE_DIR=""

log()
{
    message="[release-arm64-rk3562] $*"
    printf '%s\n' "${message}"
    if [ -d "${BUILD_LOG_DIR}" ]; then
        printf '%s\n' "${message}" >> "${RELEASE_LOG}"
    fi
}

die()
{
    log "ERROR: $*" >&2
    return 1
}

cleanup()
{
    if [ -n "${VERIFY_DIR}" ] && [ -d "${VERIFY_DIR}" ]; then
        case "${VERIFY_DIR}" in
            "${BUILD_DIR}"/.release-verify.*) rm -rf -- "${VERIFY_DIR}" ;;
            *) printf '[release-arm64-rk3562] WARNING: 拒绝清理非预期验证目录：%s\n' "${VERIFY_DIR}" >&2 ;;
        esac
    fi
}

on_error()
{
    status="$?"
    error_line="$1"
    error_command="$2"
    trap - ERR
    set +e
    mkdir -p "${BUILD_LOG_DIR}"
    {
        printf '\n[release-arm64-rk3562] ERROR: 发布失败\n'
        printf '[release-arm64-rk3562] 失败阶段：%s\n' "${CURRENT_STAGE}"
        printf '[release-arm64-rk3562] 失败命令：%s\n' "${error_command}"
        printf '[release-arm64-rk3562] 失败行号：%s\n' "${error_line}"
        printf '[release-arm64-rk3562] 发布日志：%s\n' "${RELEASE_LOG}"
        printf '[release-arm64-rk3562] C++ 构建日志：%s\n' "${CONTROLLER_BUILD_LOG}"
        printf '[release-arm64-rk3562] edge-web 构建日志：%s\n' "${WEB_BUILD_LOG}"
    } | tee -a "${RELEASE_LOG}" >&2
    if [ -n "${RELEASE_DIR}" ] && [ -d "${RELEASE_DIR}" ]; then
        case "${RELEASE_DIR}" in
            "${DIST_DIR}"/edge-controller-rk3562-*) rm -rf -- "${RELEASE_DIR}" ;;
            *) printf '[release-arm64-rk3562] WARNING: 拒绝清理非预期发布目录：%s\n' "${RELEASE_DIR}" >&2 ;;
        esac
        printf '[release-arm64-rk3562] 已删除未通过最终验证的发布目录：%s\n' \
            "${RELEASE_DIR}" | tee -a "${RELEASE_LOG}" >&2
    fi
    exit "${status}"
}

stage()
{
    CURRENT_STAGE="$2"
    log "[$1/6] $2"
}

need_cmd()
{
    command -v "$1" >/dev/null 2>&1 || die "缺少命令：$1"
}

normalize_configuration()
{
    [ -f "${VERSION_RESOLVER}" ] || die "产品版本解析器不存在：${VERSION_RESOLVER}"
    PACKAGE_VERSION="$(python3 "${VERSION_RESOLVER}" \
        --repository-root "${PROJECT_ROOT}" \
        --override "${PACKAGE_VERSION_OVERRIDE}")" || return 1
    if [ -n "${PACKAGE_VERSION_OVERRIDE}" ]; then
        PACKAGE_VERSION_SOURCE="显式 PACKAGE_VERSION"
    else
        PACKAGE_VERSION_SOURCE="${PROJECT_ROOT}/VERSION"
    fi
    case "${BUILD_JOBS}" in
        ''|*[!0-9]*|0) die "BUILD_JOBS 必须是正整数：${BUILD_JOBS}" ;;
    esac
    case "${CLEAN_BUILD,,}" in
        1|true|yes) CLEAN_BUILD_ENABLED=1 ;;
        ''|0|false|no) CLEAN_BUILD_ENABLED=0 ;;
        *) die "CLEAN_BUILD 仅支持 1/true/yes 或 0/false/no：${CLEAN_BUILD}" ;;
    esac
    case "${DIST_DIR}" in
        /*) ;;
        *) DIST_DIR="${PROJECT_ROOT}/${DIST_DIR}" ;;
    esac
    [ -n "${DIST_DIR}" ] && [ "${DIST_DIR}" != "/" ] || die "DIST_DIR 不能为根目录"
    if [ -n "${EDGE_RELEASE_SIGNING_KEY}" ]; then
        signing_key_real="$(readlink -f -- "${EDGE_RELEASE_SIGNING_KEY}" 2>/dev/null || true)"
        [ -n "${signing_key_real}" ] && [ -f "${signing_key_real}" ] && [ ! -L "${EDGE_RELEASE_SIGNING_KEY}" ] ||
            die "EDGE_RELEASE_SIGNING_KEY 必须是常规私钥文件且不能是符号链接"
        EDGE_RELEASE_SIGNING_KEY="${signing_key_real}"
    fi
    RELEASE_DIR="${DIST_DIR}/edge-controller-rk3562-${PACKAGE_VERSION}"
    ARCHIVE_PATH="${RELEASE_DIR}/edge-controller-rk3562-${PACKAGE_VERSION}.tar.gz"
}

prepare_logs()
{
    if [ "${CLEAN_BUILD_ENABLED}" -eq 1 ]; then
        [ "${BUILD_DIR}" = "${PROJECT_ROOT}/build-arm64-edge" ] ||
            die "拒绝清理非标准构建目录：${BUILD_DIR}"
        rm -rf -- "${BUILD_DIR}"
    fi
    mkdir -p "${BUILD_LOG_DIR}"
    : > "${RELEASE_LOG}"
    : > "${WEB_BUILD_LOG}"
}

prepare_release_directory()
{
    [ "${RELEASE_DIR}" != "${DIST_DIR}" ] && [ "${RELEASE_DIR}" != "/" ] ||
        die "发布目录不安全：${RELEASE_DIR}"
    case "${RELEASE_DIR}" in
        "${DIST_DIR}"/edge-controller-rk3562-*) ;;
        *) die "发布目录未位于 DIST_DIR 的版本子目录：${RELEASE_DIR}" ;;
    esac
    rm -rf -- "${RELEASE_DIR}"
    mkdir -p "${RELEASE_DIR}"
}

detect_readelf()
{
    if [ -n "${READELF_BIN}" ]; then
        command -v "${READELF_BIN}" >/dev/null 2>&1 || [ -x "${READELF_BIN}" ] ||
            die "READELF_BIN 不可执行：${READELF_BIN}"
        return
    fi
    toolchain_readelf="${RK3562_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-readelf"
    if [ -x "${toolchain_readelf}" ]; then
        READELF_BIN="${toolchain_readelf}"
    elif command -v aarch64-none-linux-gnu-readelf >/dev/null 2>&1; then
        READELF_BIN="$(command -v aarch64-none-linux-gnu-readelf)"
    elif command -v readelf >/dev/null 2>&1; then
        READELF_BIN="$(command -v readelf)"
    else
        die "缺少可读取 AArch64 ELF 的 readelf"
    fi
}

elf_header_value()
{
    header_text="$1"
    field_name="$2"
    printf '%s\n' "${header_text}" |
        LC_ALL=C sed -n "s/^[[:space:]]*${field_name}:[[:space:]]*//p" |
        head -n 1
}

validate_aarch64_elf()
{
    binary="$1"
    description="$2"
    [ -f "${binary}" ] && [ -x "${binary}" ] ||
        die "${description} 不存在或不可执行：${binary}"
    header="$(LC_ALL=C "${READELF_BIN}" -hW "${binary}" 2>&1)" ||
        die "${description} 不是有效 ELF：${binary}"
    elf_class="$(elf_header_value "${header}" Class)"
    elf_machine="$(elf_header_value "${header}" Machine)"
    [ "${elf_class}" = ELF64 ] && [ "${elf_machine}" = AArch64 ] ||
        die "${description} 架构错误：Class=${elf_class:-Unknown}；Machine=${elf_machine:-Unknown}"
}

print_binary_summary()
{
    binary="$1"
    description="$2"
    binary_size="$(stat -c '%s' -- "${binary}")"
    binary_sha256="$(sha256sum -- "${binary}" | awk '{print $1}')"
    binary_file="$(LC_ALL=C file -L -- "${binary}")"
    log "${description}：${binary}"
    log "${description} 大小：${binary_size} bytes"
    log "${description} SHA-256：${binary_sha256}"
    log "${description} file：${binary_file}"
}

validate_controller()
{
    controller="${BUILD_DIR}/edge-controller"
    validate_aarch64_elf "${controller}" edge-controller
    program="$(LC_ALL=C "${READELF_BIN}" -lW "${controller}" 2>&1)" ||
        die "无法读取 edge-controller 程序头"
    printf '%s\n' "${program}" |
        grep -F 'Requesting program interpreter: /lib/ld-linux-aarch64.so.1' >/dev/null ||
        die "edge-controller 动态解释器不是 /lib/ld-linux-aarch64.so.1"
    dynamic="$(LC_ALL=C "${READELF_BIN}" -dW "${controller}" 2>&1)" ||
        die "无法读取 edge-controller 动态段"
    printf '%s\n' "${dynamic}" | grep -F 'Shared library: [libmosquitto.so.1]' >/dev/null ||
        die "edge-controller 未依赖 libmosquitto.so.1"
    printf '%s\n' "${dynamic}" | grep -F '$ORIGIN/../lib' >/dev/null ||
        die "edge-controller RPATH/RUNPATH 不包含 \$ORIGIN/../lib"
    if printf '%s\n%s\n' "${program}" "${dynamic}" |
        grep -E '/usr/lib/x86_64-linux-gnu|(^|[^[:alnum:]_])x86_64([^[:alnum:]_]|$)' >/dev/null; then
        die "edge-controller ELF 信息包含主机 x86_64 路径"
    fi
    print_binary_summary "${controller}" edge-controller
}

validate_web()
{
    web_binary="${BUILD_DIR}/edge-web"
    validate_aarch64_elf "${web_binary}" edge-web
    program="$(LC_ALL=C "${READELF_BIN}" -lW "${web_binary}" 2>&1)" ||
        die "无法读取 edge-web 程序头"
    dynamic="$(LC_ALL=C "${READELF_BIN}" -dW "${web_binary}" 2>&1 || true)"
    ! printf '%s\n' "${program}" | grep -F 'INTERP' >/dev/null ||
        die "edge-web 含动态解释器，不是静态 ELF"
    ! printf '%s\n' "${dynamic}" | grep -F '(NEEDED)' >/dev/null ||
        die "edge-web 含动态依赖，不是静态 ELF"
    web_file="$(LC_ALL=C file -L -- "${web_binary}")"
    printf '%s\n' "${web_file}" | grep -F 'statically linked' >/dev/null ||
        die "file 未将 edge-web 识别为静态 ELF：${web_file}"
    print_binary_summary "${web_binary}" edge-web
}

build_controller()
{
    PACKAGE_VERSION="${PACKAGE_VERSION}" \
    BUILD_DIR="${BUILD_DIR}" \
    BUILD_JOBS="${BUILD_JOBS}" \
    ARM64_DEPS_ROOT="${ARM64_DEPS_ROOT}" \
    RK3562_TOOLCHAIN_ROOT="${RK3562_TOOLCHAIN_ROOT}" \
        "${CONTROLLER_BUILD_SCRIPT}" 2>&1 | tee -a "${RELEASE_LOG}"
}

build_web()
{
    printf '%s\n' \
        "[edge-web-build] CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -ldflags=-s\\ -w\\ -X\\ main.version=${PACKAGE_VERSION} edge-web/cmd/edge-web" |
        tee -a "${WEB_BUILD_LOG}" "${RELEASE_LOG}"
    (
        cd "${PROJECT_ROOT}/web"
        CGO_ENABLED=0 GOOS=linux GOARCH=arm64 \
            go build -trimpath -ldflags="-s -w -X main.version=${PACKAGE_VERSION}" \
                -o "${BUILD_DIR}/edge-web" edge-web/cmd/edge-web
    ) 2>&1 | tee -a "${WEB_BUILD_LOG}" "${RELEASE_LOG}"
    printf '%s\n' '[edge-web-build] build completed' |
        tee -a "${WEB_BUILD_LOG}" "${RELEASE_LOG}"
}

generate_package()
{
    rm -f -- "${ARCHIVE_PATH}"
    PACKAGE_VERSION="${PACKAGE_VERSION}" \
    BUILD_DIR="${BUILD_DIR}" \
    ARM64_DEPS_ROOT="${ARM64_DEPS_ROOT}" \
    RK3562_TOOLCHAIN_ROOT="${RK3562_TOOLCHAIN_ROOT}" \
    DIST_DIR="${RELEASE_DIR}" \
    EDGE_RELEASE_SIGNING_KEY="${EDGE_RELEASE_SIGNING_KEY}" \
    EDGE_RELEASE_SIGNING_KEY_ID="${EDGE_RELEASE_SIGNING_KEY_ID}" \
        "${PACKAGE_SCRIPT}" 2>&1 | tee -a "${RELEASE_LOG}"
    [ -f "${ARCHIVE_PATH}" ] || die "打包脚本未生成预期发布包：${ARCHIVE_PATH}"
}

generate_release_directory_files()
{
    cp -- "${DEPLOY_SCRIPT}" "${RELEASE_DIR}/deploy-rk3562.sh"
    cp -- "${DISPLAY_DEPLOY_SCRIPT}" "${RELEASE_DIR}/deploy-rk3562-display.sh"
    mkdir -p "${RELEASE_DIR}/rk3562/display"
    cp -- "${DISPLAY_ASSET_DIR}/start-edge-kiosk.sh" \
        "${RELEASE_DIR}/rk3562/display/start-edge-kiosk.sh"
    cp -- "${DISPLAY_ASSET_DIR}/edge-kiosk.desktop" \
        "${RELEASE_DIR}/rk3562/display/edge-kiosk.desktop"
    chmod 0755 \
        "${RELEASE_DIR}/deploy-rk3562.sh" \
        "${RELEASE_DIR}/deploy-rk3562-display.sh" \
        "${RELEASE_DIR}/rk3562/display/start-edge-kiosk.sh"
    chmod 0644 "${RELEASE_DIR}/rk3562/display/edge-kiosk.desktop"
    printf '%s\n' \
        '无屏边缘控制器：sudo bash ./deploy-rk3562.sh' \
        '带屏显示终端：sudo bash ./deploy-rk3562-display.sh' \
        > "${RELEASE_DIR}/README.txt"
    (
        cd "${RELEASE_DIR}"
        sha256sum -- \
            "$(basename -- "${ARCHIVE_PATH}")" \
            deploy-rk3562.sh \
            deploy-rk3562-display.sh \
            rk3562/display/start-edge-kiosk.sh \
            rk3562/display/edge-kiosk.desktop \
            > SHA256SUMS
    )
}

json_string_value()
{
    json_file="$1"
    json_key="$2"
    LC_ALL=C sed -n \
        "s/^[[:space:]]*\"${json_key}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\"[[:space:]]*,\{0,1\}[[:space:]]*$/\1/p" \
        "${json_file}" | head -n 1
}

verify_archive()
{
    tar -tzf "${ARCHIVE_PATH}" >/dev/null
    archive_listing="$(tar -tzf "${ARCHIVE_PATH}")"
    printf '%s\n' "${archive_listing}" | grep -Ev '^edge-controller(/|$)' >/dev/null &&
        die "发布包包含 edge-controller/ 之外的顶层路径"
    if printf '%s\n' "${archive_listing}" | grep -E '(^/|(^|/)\.\.(/|$))' >/dev/null; then
        die "发布包包含不安全路径"
    fi

    for required_path in \
        edge-controller/bin/edge-controller \
        edge-controller/bin/edge-web \
        edge-controller/lib/ \
        edge-controller/config/edge-controller.env.default \
        edge-controller/systemd/ \
        edge-controller/systemd/edge-log-rotator.service \
        edge-controller/systemd/edge-upgrade@.service \
        edge-controller/systemd/edge-upgrade-recovery.service \
        edge-controller/systemd/edge-upgrade-recovery-verify.service \
        edge-controller/scripts/install.sh \
        edge-controller/scripts/verify-install.sh \
        edge-controller/scripts/rotate-logs.sh \
        edge-controller/scripts/update-manager.py \
        edge-controller/VERSION \
        edge-controller/package-info.json \
        edge-controller/MANIFEST.sha256; do
        printf '%s\n' "${archive_listing}" | grep -F -x "${required_path}" >/dev/null ||
            die "发布包缺少：${required_path}"
    done

    VERIFY_DIR="$(mktemp -d "${BUILD_DIR}/.release-verify.XXXXXX")"
    tar -xzf "${ARCHIVE_PATH}" -C "${VERIFY_DIR}"
    extracted_root="${VERIFY_DIR}/edge-controller"
    extracted_version="$(sed -n '1p' "${extracted_root}/VERSION")"
    [ "${extracted_version}" = "${PACKAGE_VERSION}" ] ||
        die "VERSION 不匹配：实际=${extracted_version:-<空>}；期望=${PACKAGE_VERSION}"

    package_info="${extracted_root}/package-info.json"
    grep -E '^[[:space:]]*"package_format_version"[[:space:]]*:[[:space:]]*1,' \
        "${package_info}" >/dev/null || die "package-info.json package_format_version 不正确"
    [ "$(json_string_value "${package_info}" product)" = edge-controller ] ||
        die "package-info.json product 不正确"
    [ "$(json_string_value "${package_info}" platform)" = rk3562 ] ||
        die "package-info.json platform 不正确"
    [ "$(json_string_value "${package_info}" arch)" = aarch64 ] ||
        die "package-info.json arch 不正确"
    [ "$(json_string_value "${package_info}" version)" = "${PACKAGE_VERSION}" ] ||
        die "package-info.json version 不正确"
    [ -n "$(json_string_value "${package_info}" build_time)" ] ||
        die "package-info.json build_time 不能为空"
    grep -F -x 'EDGE_CONTROLLER_REQUIRE_EXPLICIT_NETWORK_CONFIG=1' \
        "${extracted_root}/config/edge-controller.env.default" >/dev/null ||
        die "发布包 RK3562 默认环境未启用首次网络保护"

    if [ -n "${EDGE_RELEASE_SIGNING_KEY}" ]; then
        printf '%s\n' "${archive_listing}" | grep -F -x edge-controller/MANIFEST.sha256.sig >/dev/null ||
            die "签名发布包缺少 MANIFEST.sha256.sig"
        signing_public_key="${VERIFY_DIR}/release-signing-ed25519.pub"
        openssl pkey -in "${EDGE_RELEASE_SIGNING_KEY}" -pubout -out "${signing_public_key}" >/dev/null 2>&1 ||
            die "无法从发布私钥导出验证公钥"
        signing_key_id="${EDGE_RELEASE_SIGNING_KEY_ID}"
        if [ -z "${signing_key_id}" ]; then
            signing_key_id="ed25519-$(sha256sum -- "${signing_public_key}" | awk '{print substr($1,1,16)}')"
        fi
        EDGE_UPGRADE_SIGNATURE_POLICY=required \
        EDGE_UPGRADE_PUBLIC_KEY="${signing_public_key}" \
        EDGE_UPGRADE_PUBLIC_KEY_ID="${signing_key_id}" \
            python3 "${extracted_root}/scripts/update-manager.py" validate-root --root "${extracted_root}" >/dev/null ||
            die "解压后的正式包 Ed25519 签名验证失败"
    else
        ! printf '%s\n' "${archive_listing}" | grep -F -x edge-controller/MANIFEST.sha256.sig >/dev/null ||
            die "未配置签名私钥时不应生成签名文件"
        EDGE_UPGRADE_SIGNATURE_POLICY=optional \
            python3 "${extracted_root}/scripts/update-manager.py" validate-root --root "${extracted_root}" >/dev/null ||
            die "解压后的 unsigned 发布包契约验证失败"
    fi

    (cd "${extracted_root}" && sha256sum -c MANIFEST.sha256) 2>&1 |
        tee -a "${RELEASE_LOG}"
    case "${VERIFY_DIR}" in
        "${BUILD_DIR}"/.release-verify.*) rm -rf -- "${VERIFY_DIR}" ;;
        *) die "拒绝清理非预期验证目录：${VERIFY_DIR}" ;;
    esac
    VERIFY_DIR=""
}

verify_release_directory()
{
    expected_archive="$(basename -- "${ARCHIVE_PATH}")"
    for required_file in \
        "${expected_archive}" deploy-rk3562.sh deploy-rk3562-display.sh SHA256SUMS README.txt; do
        [ -f "${RELEASE_DIR}/${required_file}" ] || die "发布目录缺少：${required_file}"
    done
    [ -f "${RELEASE_DIR}/rk3562/display/start-edge-kiosk.sh" ] ||
        die "发布目录缺少 Kiosk 启动脚本"
    grep -F -- '--password-store=basic' \
        "${RELEASE_DIR}/rk3562/display/start-edge-kiosk.sh" >/dev/null ||
        die "发布目录 Kiosk 启动脚本缺少 --password-store=basic"
    [ -f "${RELEASE_DIR}/rk3562/display/edge-kiosk.desktop" ] ||
        die "发布目录缺少 Kiosk autostart 文件"
    unexpected_file=""
    while IFS= read -r release_entry; do
        case "${release_entry}" in
            "${expected_archive}"|deploy-rk3562.sh|deploy-rk3562-display.sh|SHA256SUMS|README.txt|rk3562) ;;
            *) unexpected_file="${release_entry}"; break ;;
        esac
    done < <(find "${RELEASE_DIR}" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort)
    [ -z "${unexpected_file}" ] || die "发布目录包含非正式文件：${unexpected_file}"
    [ "$(find "${RELEASE_DIR}" -mindepth 1 -maxdepth 1 -type f | wc -l)" -eq 5 ] ||
        die "发布目录顶层文件数量不是 5"
    [ "$(find "${RELEASE_DIR}/rk3562/display" -mindepth 1 -maxdepth 1 -type f | wc -l)" -eq 2 ] ||
        die "发布目录 Kiosk 资产数量不是 2"
    expected_readme="$(printf '%s\n' \
        '无屏边缘控制器：sudo bash ./deploy-rk3562.sh' \
        '带屏显示终端：sudo bash ./deploy-rk3562-display.sh')"
    [ "$(cat "${RELEASE_DIR}/README.txt")" = "${expected_readme}" ] ||
        die "README.txt 不是最简部署说明"
    (cd "${RELEASE_DIR}" && sha256sum -c SHA256SUMS) 2>&1 | tee -a "${RELEASE_LOG}"
}

print_release_summary()
{
    archive_size="$(stat -c '%s' -- "${ARCHIVE_PATH}")"
    archive_sha256="$(sha256sum -- "${ARCHIVE_PATH}" | awk '{print $1}')"
    controller_sha256="$(sha256sum -- "${BUILD_DIR}/edge-controller" | awk '{print $1}')"
    web_sha256="$(sha256sum -- "${BUILD_DIR}/edge-web" | awk '{print $1}')"
    {
        printf '%s\n' '========================================'
        printf '%s\n' 'RK3562 ARM64 发布完成'
        printf '版本：%s\n' "${PACKAGE_VERSION}"
        printf 'edge-controller：%s (SHA-256: %s)\n' "${BUILD_DIR}/edge-controller" "${controller_sha256}"
        printf 'edge-web：%s (SHA-256: %s)\n' "${BUILD_DIR}/edge-web" "${web_sha256}"
        printf '发布目录：%s\n' "${RELEASE_DIR}"
        printf '发布包：%s\n' "${ARCHIVE_PATH}"
        printf '发布包大小：%s bytes\n' "${archive_size}"
        printf '发布包 SHA-256：%s\n' "${archive_sha256}"
        printf '构建日志：%s\n' "${RELEASE_LOG}"
        printf 'C++ 日志：%s\n' "${CONTROLLER_BUILD_LOG}"
        printf 'edge-web 日志：%s\n' "${WEB_BUILD_LOG}"
        printf '无屏板端执行：sudo bash ./deploy-rk3562.sh\n'
        printf '带屏板端执行：sudo bash ./deploy-rk3562-display.sh\n'
        printf '%s\n' '========================================'
    } | tee -a "${RELEASE_LOG}"
}

main()
{
    trap cleanup EXIT
    trap 'on_error "${LINENO}" "${BASH_COMMAND}"' ERR
    [ "$#" -eq 0 ] || die "用法：release-arm64-rk3562.sh（通过环境变量配置）"
    need_cmd python3
    normalize_configuration
    prepare_logs

    stage 1 环境检查
    [ -d "${PROJECT_ROOT}/backend" ] && [ -f "${PROJECT_ROOT}/web/go.mod" ] ||
        die "项目目录不完整：${PROJECT_ROOT}"
    [ -x "${CONTROLLER_BUILD_SCRIPT}" ] || die "构建脚本不存在或不可执行：${CONTROLLER_BUILD_SCRIPT}"
    [ -x "${PACKAGE_SCRIPT}" ] || die "打包脚本不存在或不可执行：${PACKAGE_SCRIPT}"
    [ -f "${DEPLOY_SCRIPT}" ] || die "板端部署脚本不存在：${DEPLOY_SCRIPT}"
    [ -f "${DISPLAY_DEPLOY_SCRIPT}" ] || die "显示版部署脚本不存在：${DISPLAY_DEPLOY_SCRIPT}"
    [ -f "${DISPLAY_ASSET_DIR}/start-edge-kiosk.sh" ] || die "Kiosk 启动脚本不存在"
    [ -f "${DISPLAY_ASSET_DIR}/edge-kiosk.desktop" ] || die "Kiosk autostart 文件不存在"
    for command_name in date go file readlink sed head grep awk stat sha256sum tar mktemp tee rm mkdir cp chmod basename find sort wc cat python3; do
        need_cmd "${command_name}"
    done
    if [ -n "${EDGE_RELEASE_SIGNING_KEY}" ]; then
        need_cmd openssl
    fi
    detect_readelf
    mkdir -p "${DIST_DIR}"
    prepare_release_directory
    log "版本=${PACKAGE_VERSION}；来源=${PACKAGE_VERSION_SOURCE}；jobs=${BUILD_JOBS}；clean=${CLEAN_BUILD_ENABLED}"
    log "依赖=${ARM64_DEPS_ROOT}；工具链=${RK3562_TOOLCHAIN_ROOT}；输出=${DIST_DIR}"

    stage 2 "编译 edge-controller"
    build_controller

    stage 3 "编译 edge-web"
    build_web

    stage 4 "校验 ARM64 产物"
    validate_controller
    validate_web

    stage 5 生成发布包
    generate_package

    stage 6 "验证发布包并生成发布目录"
    verify_archive
    generate_release_directory_files
    verify_release_directory
    print_release_summary
}

main "$@"
