#!/usr/bin/env bash

# 组装一份以 edge-controller/ 为根目录的完整 RK3562 发布物。
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build-arm64-edge}"
DIST_DIR="${DIST_DIR:-${PROJECT_ROOT}/dist}"
ARM64_DEPS_ROOT="${ARM64_DEPS_ROOT:-/home/wu/dl/arm64-deps}"
RK3562_TOOLCHAIN_ROOT="${RK3562_TOOLCHAIN_ROOT:-/home/wu/sdk/kickpi-rk356x/download/rk356x-linux/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu}"
VERSION_RESOLVER="${SCRIPT_DIR}/product-version.py"
PACKAGE_VERSION_OVERRIDE="${PACKAGE_VERSION:-}"
PACKAGE_VERSION=""
READELF_BIN="${READELF_BIN:-}"
EDGE_RELEASE_SIGNING_KEY="${EDGE_RELEASE_SIGNING_KEY:-}"
EDGE_RELEASE_SIGNING_KEY_ID="${EDGE_RELEASE_SIGNING_KEY_ID:-}"
WORK_DIR=""
SIGNING_PUBLIC_KEY=""
SIGNING_KEY_ID=""
SIGNATURE_ALGORITHM="none"

log()
{
    printf '[package-arm64-rk3562] %s\n' "$*"
}

die()
{
    printf '[package-arm64-rk3562] ERROR: %s\n' "$*" >&2
    exit 1
}

need_cmd()
{
    command -v "$1" >/dev/null 2>&1 || die "缺少打包命令：$1"
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

set_package_permissions()
{
    find "${PACKAGE_ROOT}" -type f -exec chmod 0644 {} +
    while IFS= read -r -d '' directory; do
        set_exact_directory_mode "${directory}" 0755
    done < <(find \
        "${PACKAGE_ROOT}/bin" "${PACKAGE_ROOT}/lib" \
        "${PACKAGE_ROOT}/systemd" "${PACKAGE_ROOT}/scripts" \
        -type d -print0)
    set_exact_directory_mode "${PACKAGE_ROOT}" 0750
    set_exact_directory_mode "${PACKAGE_ROOT}/config" 0750
    set_exact_directory_mode "${PACKAGE_ROOT}/data" 0750
    set_exact_directory_mode "${PACKAGE_ROOT}/log" 0750
    set_exact_directory_mode "${PACKAGE_ROOT}/run" 0770

    chmod 0755 \
        "${PACKAGE_ROOT}/bin/edge-controller" \
        "${PACKAGE_ROOT}/bin/edge-web" \
        "${PACKAGE_ROOT}/scripts/install.sh" \
        "${PACKAGE_ROOT}/scripts/verify-install.sh" \
        "${PACKAGE_ROOT}/scripts/rotate-logs.sh" \
        "${PACKAGE_ROOT}/scripts/update-manager.py"
    chmod 0640 \
        "${PACKAGE_ROOT}/config/edge-controller.env" \
        "${PACKAGE_ROOT}/config/edge-controller.env.default"
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

resolve_file_within()
{
    source_path="$1"
    allowed_root="$2"
    description="$3"
    [ -e "${source_path}" ] || [ -L "${source_path}" ] || die "${description}不存在：${source_path}"
    resolved="$(readlink -f -- "${source_path}" 2>/dev/null || true)"
    [ -n "${resolved}" ] && [ -f "${resolved}" ] || die "${description}不是有效文件或符号链接：${source_path}"
    path_is_within "${resolved}" "${allowed_root}" || die "${description}解析后越出允许目录：${source_path} -> ${resolved}"
    printf '%s\n' "${resolved}"
}

detect_readelf()
{
    if [ -n "${READELF_BIN}" ]; then
        command -v "${READELF_BIN}" >/dev/null 2>&1 || die "找不到 READELF_BIN：${READELF_BIN}"
        return
    fi
    candidate="${TOOLCHAIN_ROOT_REAL}/bin/aarch64-none-linux-gnu-readelf"
    if [ -x "${candidate}" ]; then
        READELF_BIN="${candidate}"
    elif command -v readelf >/dev/null 2>&1; then
        READELF_BIN="readelf"
    else
        die "缺少 readelf，无法校验发布包 ELF"
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

validate_aarch64()
{
    elf_path="$1"
    description="$2"
    header="$(LC_ALL=C "${READELF_BIN}" -hW "${elf_path}" 2>&1)" || die "${description}不是有效 ELF：${elf_path}"
    elf_class="$(elf_header_value "${header}" Class)"
    elf_machine="$(elf_header_value "${header}" Machine)"
    [ "${elf_class}" = "ELF64" ] && [ "${elf_machine}" = "AArch64" ] ||
        die "${description}架构错误：Class=${elf_class:-Unknown}；Machine=${elf_machine:-Unknown}；${elf_path}"
}

validate_soname()
{
    library="$1"
    expected="$2"
    dynamic="$(LC_ALL=C "${READELF_BIN}" -dW "${library}" 2>&1)" || die "无法读取动态库：${library}"
    printf '%s\n' "${dynamic}" | grep -F "Library soname: [${expected}]" >/dev/null ||
        die "SONAME 错误：${library}；期望=${expected}"
}

copy_library_with_soname()
{
    source_path="$1"
    allowed_root="$2"
    soname="$3"
    description="$4"
    real_source="$(resolve_file_within "${source_path}" "${allowed_root}" "${description}")"
    real_name="$(basename -- "${real_source}")"
    cp -- "${real_source}" "${PACKAGE_ROOT}/lib/${real_name}"
    if [ "${real_name}" != "${soname}" ]; then
        ln -sfn "${real_name}" "${PACKAGE_ROOT}/lib/${soname}"
    fi
}

validate_controller()
{
    controller="${PACKAGE_ROOT}/bin/edge-controller"
    validate_aarch64 "${controller}" "edge-controller"
    program="$(LC_ALL=C "${READELF_BIN}" -lW "${controller}" 2>&1)" || die "无法读取 edge-controller 程序头"
    printf '%s\n' "${program}" | grep -F 'Requesting program interpreter: /lib/ld-linux-aarch64.so.1' >/dev/null ||
        die "edge-controller 动态解释器不是 /lib/ld-linux-aarch64.so.1"
    dynamic="$(LC_ALL=C "${READELF_BIN}" -dW "${controller}" 2>&1)" || die "无法读取 edge-controller 动态段"
    printf '%s\n' "${dynamic}" | grep -F 'Shared library: [libmosquitto.so.1]' >/dev/null ||
        die "edge-controller 未依赖 libmosquitto.so.1"
    printf '%s\n' "${dynamic}" | grep -F '$ORIGIN/../lib' >/dev/null ||
        die "edge-controller RPATH/RUNPATH 不包含 \$ORIGIN/../lib"
    if printf '%s\n' "${dynamic}" | grep -E '/usr/lib/x86_64-linux-gnu|(^|[^[:alnum:]_])x86_64([^[:alnum:]_]|$)' >/dev/null; then
        die "edge-controller 动态信息包含主机 x86_64 路径"
    fi
}

validate_static_web()
{
    web_binary="${PACKAGE_ROOT}/bin/edge-web"
    validate_aarch64 "${web_binary}" "edge-web"
    program="$(LC_ALL=C "${READELF_BIN}" -lW "${web_binary}" 2>&1)" || die "无法读取 edge-web 程序头"
    dynamic="$(LC_ALL=C "${READELF_BIN}" -dW "${web_binary}" 2>&1 || true)"
    ! printf '%s\n' "${program}" | grep -F 'INTERP' >/dev/null || die "edge-web 含动态解释器，不是静态 ELF"
    ! printf '%s\n' "${dynamic}" | grep -F '(NEEDED)' >/dev/null || die "edge-web 含动态依赖，不是静态 ELF"
    file_output="$(LC_ALL=C file -L -- "${web_binary}")"
    printf '%s\n' "${file_output}" | grep -F 'statically linked' >/dev/null ||
        die "file 未将 edge-web 识别为静态 ELF：${file_output}"
}

prepare_roots()
{
    DEPS_ROOT_REAL="$(readlink -f -- "${ARM64_DEPS_ROOT}" 2>/dev/null || true)"
    TOOLCHAIN_ROOT_REAL="$(readlink -f -- "${RK3562_TOOLCHAIN_ROOT}" 2>/dev/null || true)"
    [ -n "${DEPS_ROOT_REAL}" ] && [ -d "${DEPS_ROOT_REAL}" ] || die "ARM64_DEPS_ROOT 不存在：${ARM64_DEPS_ROOT}"
    [ -n "${TOOLCHAIN_ROOT_REAL}" ] && [ -d "${TOOLCHAIN_ROOT_REAL}" ] || die "RK3562_TOOLCHAIN_ROOT 不存在：${RK3562_TOOLCHAIN_ROOT}"
    AARCH64_CXX="$(resolve_file_within "${TOOLCHAIN_ROOT_REAL}/bin/aarch64-none-linux-gnu-g++" "${TOOLCHAIN_ROOT_REAL}" "AArch64 C++ 编译器")"
    detect_readelf
}

copy_runtime_libraries()
{
    mosquitto_real="$(resolve_file_within "${DEPS_ROOT_REAL}/lib/libmosquitto.so.2.0.18" "${DEPS_ROOT_REAL}" "Mosquitto 2.0.18")"
    mosquitto_soname_real="$(resolve_file_within "${DEPS_ROOT_REAL}/lib/libmosquitto.so.1" "${DEPS_ROOT_REAL}" "Mosquitto SONAME")"
    [ "${mosquitto_real}" = "${mosquitto_soname_real}" ] ||
        die "libmosquitto.so.1 未指向 libmosquitto.so.2.0.18"
    cp -- "${mosquitto_real}" "${PACKAGE_ROOT}/lib/libmosquitto.so.2.0.18"
    ln -s libmosquitto.so.2.0.18 "${PACKAGE_ROOT}/lib/libmosquitto.so.1"

    copy_library_with_soname "${DEPS_ROOT_REAL}/lib/libssl.so.3" "${DEPS_ROOT_REAL}" "libssl.so.3" "OpenSSL SSL"
    copy_library_with_soname "${DEPS_ROOT_REAL}/lib/libcrypto.so.3" "${DEPS_ROOT_REAL}" "libcrypto.so.3" "OpenSSL Crypto"

    libstdcpp_path="$("${AARCH64_CXX}" -print-file-name=libstdc++.so.6)"
    libgcc_path="$("${AARCH64_CXX}" -print-file-name=libgcc_s.so.1)"
    copy_library_with_soname "${libstdcpp_path}" "${TOOLCHAIN_ROOT_REAL}" "libstdc++.so.6" "工具链 libstdc++"
    copy_library_with_soname "${libgcc_path}" "${TOOLCHAIN_ROOT_REAL}" "libgcc_s.so.1" "工具链 libgcc_s"

    validate_soname "${PACKAGE_ROOT}/lib/libmosquitto.so.1" "libmosquitto.so.1"
    validate_soname "${PACKAGE_ROOT}/lib/libssl.so.3" "libssl.so.3"
    validate_soname "${PACKAGE_ROOT}/lib/libcrypto.so.3" "libcrypto.so.3"
    validate_soname "${PACKAGE_ROOT}/lib/libstdc++.so.6" "libstdc++.so.6"
    validate_soname "${PACKAGE_ROOT}/lib/libgcc_s.so.1" "libgcc_s.so.1"
    mosquitto_dynamic="$(LC_ALL=C "${READELF_BIN}" -dW "${PACKAGE_ROOT}/lib/libmosquitto.so.1")"
    printf '%s\n' "${mosquitto_dynamic}" | grep -F 'Shared library: [libssl.so.3]' >/dev/null ||
        die "打包的 libmosquitto 可能为 WITH_TLS=OFF：缺少 libssl.so.3 NEEDED"
    printf '%s\n' "${mosquitto_dynamic}" | grep -F 'Shared library: [libcrypto.so.3]' >/dev/null ||
        die "打包的 libmosquitto 可能为 WITH_TLS=OFF：缺少 libcrypto.so.3 NEEDED"
}

copy_web()
{
    [ -x "${BUILD_DIR}/edge-web" ] ||
        die "缺少 ARM64 edge-web：${BUILD_DIR}/edge-web；请通过 release-arm64-rk3562.sh 构建"
    log "复制已构建的 ARM64 edge-web：${BUILD_DIR}/edge-web"
    cp -- "${BUILD_DIR}/edge-web" "${PACKAGE_ROOT}/bin/edge-web"
}

write_metadata()
{
    built_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '%s\n' "${PACKAGE_VERSION}" > "${PACKAGE_ROOT}/VERSION"
    {
        printf '{\n'
        printf '  "package_format_version": 1,\n'
        printf '  "product": "edge-controller",\n'
        printf '  "platform": "rk3562",\n'
        printf '  "arch": "aarch64",\n'
        printf '  "version": "%s",\n' "${PACKAGE_VERSION}"
        printf '  "build_time": "%s",\n' "${built_at}"
        printf '  "signature_format_version": 1,\n'
        printf '  "signature_algorithm": "%s",\n' "${SIGNATURE_ALGORITHM}"
        if [ "${SIGNATURE_ALGORITHM}" = ed25519 ]; then
            printf '  "signing": {"scheme": "ed25519", "key_id": "%s", "signature_file": "MANIFEST.sha256.sig"}\n' "${SIGNING_KEY_ID}"
        else
            printf '  "signing": {"scheme": "none", "key_id": "", "signature_file": ""}\n'
        fi
        printf '}\n'
    } > "${PACKAGE_ROOT}/package-info.json"

    (
        cd "${PACKAGE_ROOT}"
        while IFS= read -r -d '' manifest_path; do
            sha256sum -- "${manifest_path}"
        done < <(find . \( -type f -o -type l \) \
            ! -name MANIFEST.sha256 ! -name MANIFEST.sha256.sig -print0 | LC_ALL=C sort -z)
    ) > "${PACKAGE_ROOT}/MANIFEST.sha256"
}

prepare_signing()
{
    [ -n "${EDGE_RELEASE_SIGNING_KEY}" ] || return 0
    need_cmd openssl
    signing_key_real="$(readlink -f -- "${EDGE_RELEASE_SIGNING_KEY}" 2>/dev/null || true)"
    [ -n "${signing_key_real}" ] && [ -f "${signing_key_real}" ] && [ ! -L "${EDGE_RELEASE_SIGNING_KEY}" ] ||
        die "EDGE_RELEASE_SIGNING_KEY 必须是发布机上的常规私钥文件且不能是符号链接"
    path_is_within "${signing_key_real}" "${PROJECT_ROOT}" &&
        die "发布私钥不能位于源码仓库内：${signing_key_real}"
    EDGE_RELEASE_SIGNING_KEY="${signing_key_real}"
    key_mode="$(stat -c '%a' -- "${signing_key_real}")"
    case "${key_mode}" in
        *00) ;;
        *) die "发布私钥不能向 group/other 开放权限：mode=${key_mode}" ;;
    esac
    SIGNING_PUBLIC_KEY="${WORK_DIR}/release-signing-ed25519.pub"
    openssl pkey -in "${signing_key_real}" -pubout -out "${SIGNING_PUBLIC_KEY}" >/dev/null 2>&1 ||
        die "无法从 EDGE_RELEASE_SIGNING_KEY 读取 Ed25519 公钥"
    public_description="$(openssl pkey -pubin -in "${SIGNING_PUBLIC_KEY}" -text_pub -noout 2>&1)"
    printf '%s\n' "${public_description}" | grep -i 'ED25519' >/dev/null ||
        die "EDGE_RELEASE_SIGNING_KEY 不是 Ed25519 私钥"
    if [ -n "${EDGE_RELEASE_SIGNING_KEY_ID}" ]; then
        case "${EDGE_RELEASE_SIGNING_KEY_ID}" in
            *[!A-Za-z0-9._+-]*|'') die "EDGE_RELEASE_SIGNING_KEY_ID 格式无效" ;;
        esac
        SIGNING_KEY_ID="${EDGE_RELEASE_SIGNING_KEY_ID}"
    else
        SIGNING_KEY_ID="ed25519-$(sha256sum -- "${SIGNING_PUBLIC_KEY}" | awk '{print substr($1,1,16)}')"
    fi
    SIGNATURE_ALGORITHM="ed25519"
}

sign_manifest()
{
    [ "${SIGNATURE_ALGORITHM}" = ed25519 ] || return 0
    openssl pkeyutl -sign -rawin \
        -inkey "${EDGE_RELEASE_SIGNING_KEY}" \
        -in "${PACKAGE_ROOT}/MANIFEST.sha256" \
        -out "${PACKAGE_ROOT}/MANIFEST.sha256.sig" >/dev/null 2>&1 ||
        die "生成 MANIFEST.sha256 Ed25519 签名失败"
    [ "$(stat -c '%s' -- "${PACKAGE_ROOT}/MANIFEST.sha256.sig")" -eq 64 ] ||
        die "Ed25519 签名长度不是 64 字节"
    openssl pkeyutl -verify -rawin -pubin \
        -inkey "${SIGNING_PUBLIC_KEY}" \
        -sigfile "${PACKAGE_ROOT}/MANIFEST.sha256.sig" \
        -in "${PACKAGE_ROOT}/MANIFEST.sha256" >/dev/null 2>&1 ||
        die "发布端 Ed25519 签名自校验失败"
}

cleanup()
{
    if [ -n "${WORK_DIR}" ] && [ -d "${WORK_DIR}" ]; then
        rm -rf -- "${WORK_DIR}"
    fi
}

main()
{
    [ "$#" -eq 0 ] || die "用法：package-arm64-rk3562.sh（通过环境变量配置版本和路径）"
    for command_name in date file readlink sed head grep find sort sha256sum tar cp ln chmod mkdir basename rm awk stat python3; do
        need_cmd "${command_name}"
    done
    [ -f "${VERSION_RESOLVER}" ] || die "产品版本解析器不存在：${VERSION_RESOLVER}"
    PACKAGE_VERSION="$(python3 "${VERSION_RESOLVER}" \
        --repository-root "${PROJECT_ROOT}" \
        --override "${PACKAGE_VERSION_OVERRIDE}")" || exit 1
    prepare_roots

    controller_source="${BUILD_DIR}/edge-controller"
    [ -x "${controller_source}" ] || die "缺少 ARM64 edge-controller：${controller_source}；请先运行 build-arm64-rk3562-release.sh"

    [ "${DIST_DIR}" != "/" ] || die "DIST_DIR 不能是根目录"
    mkdir -p "${DIST_DIR}"
    WORK_DIR="${DIST_DIR}/.edge-controller-rk3562-${PACKAGE_VERSION}.work"
    [ "${WORK_DIR}" != "${DIST_DIR}" ] || die "非法临时目录"
    rm -rf -- "${WORK_DIR}"
    trap cleanup EXIT
    PACKAGE_ROOT="${WORK_DIR}/edge-controller"
    mkdir -p \
        "${PACKAGE_ROOT}/bin" "${PACKAGE_ROOT}/lib" "${PACKAGE_ROOT}/config" \
        "${PACKAGE_ROOT}/data" "${PACKAGE_ROOT}/log" "${PACKAGE_ROOT}/run" \
        "${PACKAGE_ROOT}/systemd" "${PACKAGE_ROOT}/scripts"

    prepare_signing

    log "复制正式运行文件"
    cp -- "${controller_source}" "${PACKAGE_ROOT}/bin/edge-controller"
    copy_web
    copy_runtime_libraries
    cp -- "${SCRIPT_DIR}/rk3562/edge-controller.env.default" "${PACKAGE_ROOT}/config/edge-controller.env.default"
    cp -- "${SCRIPT_DIR}/rk3562/edge-controller.env.default" "${PACKAGE_ROOT}/config/edge-controller.env"
    cp -- "${SCRIPT_DIR}/rk3562/edge-controller.service" "${PACKAGE_ROOT}/systemd/edge-controller.service"
    cp -- "${SCRIPT_DIR}/rk3562/edge-web.service" "${PACKAGE_ROOT}/systemd/edge-web.service"
    cp -- "${SCRIPT_DIR}/rk3562/edge-log-rotator.service" "${PACKAGE_ROOT}/systemd/edge-log-rotator.service"
    cp -- "${SCRIPT_DIR}/rk3562/edge-upgrade@.service" "${PACKAGE_ROOT}/systemd/edge-upgrade@.service"
    cp -- "${SCRIPT_DIR}/rk3562/edge-upgrade-recovery.service" "${PACKAGE_ROOT}/systemd/edge-upgrade-recovery.service"
    cp -- "${SCRIPT_DIR}/rk3562/edge-upgrade-recovery-verify.service" "${PACKAGE_ROOT}/systemd/edge-upgrade-recovery-verify.service"
    cp -- "${SCRIPT_DIR}/rk3562/install.sh" "${PACKAGE_ROOT}/scripts/install.sh"
    cp -- "${SCRIPT_DIR}/rk3562/verify-install.sh" "${PACKAGE_ROOT}/scripts/verify-install.sh"
    cp -- "${SCRIPT_DIR}/rk3562/rotate-logs.sh" "${PACKAGE_ROOT}/scripts/rotate-logs.sh"
    cp -- "${SCRIPT_DIR}/rk3562/update-manager.py" "${PACKAGE_ROOT}/scripts/update-manager.py"
    cp -- "${SCRIPT_DIR}/rk3562/README.md" "${PACKAGE_ROOT}/README.md"
    grep -F -x 'EDGE_CONTROLLER_REQUIRE_EXPLICIT_NETWORK_CONFIG=1' \
        "${PACKAGE_ROOT}/config/edge-controller.env.default" >/dev/null ||
        die "RK3562 默认环境未启用首次网络保护"

    log "校验 ARM64 ELF、私有依赖和运行路径"
    validate_controller
    validate_static_web
    while IFS= read -r elf_file; do
        validate_aarch64 "${elf_file}" "发布包 ELF"
    done < <(find "${PACKAGE_ROOT}/bin" "${PACKAGE_ROOT}/lib" -type f -print | LC_ALL=C sort)
    if find "${PACKAGE_ROOT}" -type f \
        \( -iname '*.pem' -o -iname '*.key' -o -iname '*.crt' -o -iname '*.p12' -o -iname '*.pfx' \) \
        -print -quit | grep . >/dev/null; then
        die "基础发布包中禁止包含 MQTT TLS 证书或私钥"
    fi

    write_metadata
    sign_manifest
    set_package_permissions
    if [ "${SIGNATURE_ALGORITHM}" = ed25519 ]; then
        EDGE_UPGRADE_SIGNATURE_POLICY=optional \
        EDGE_UPGRADE_PUBLIC_KEY="${SIGNING_PUBLIC_KEY}" \
        EDGE_UPGRADE_PUBLIC_KEY_ID="${SIGNING_KEY_ID}" \
            python3 "${PACKAGE_ROOT}/scripts/update-manager.py" validate-root --root "${PACKAGE_ROOT}" >/dev/null
    else
        EDGE_UPGRADE_SIGNATURE_POLICY=optional \
            python3 "${PACKAGE_ROOT}/scripts/update-manager.py" validate-root --root "${PACKAGE_ROOT}" >/dev/null
    fi
    output="${DIST_DIR}/edge-controller-rk3562-${PACKAGE_VERSION}.tar.gz"
    rm -f -- "${output}"
    tar -czf "${output}" -C "${WORK_DIR}" edge-controller
    if tar -tzf "${output}" | grep -Ev '^edge-controller(/|$)' >/dev/null; then
        die "压缩包出现 edge-controller 之外的顶层目录"
    fi
    archive_sha256="$(sha256sum -- "${output}" | awk '{print $1}')"
    archive_size="$(stat -c '%s' -- "${output}")"
    log "完成：${output}"
    log "大小：${archive_size} bytes；SHA-256：${archive_sha256}"
}

main "$@"
