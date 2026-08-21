#!/usr/bin/env bash

# 仅为 RK3562 / AArch64 交叉编译量产 edge-controller 目标。
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"

PACKAGE_VERSION="${PACKAGE_VERSION:-}"
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build-arm64-edge}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-${PROJECT_ROOT}/backend/toolchain-arm64-rk3562.cmake}"
ARM64_DEPS_ROOT="${ARM64_DEPS_ROOT:-/home/wu/dl/arm64-deps}"
RK3562_TOOLCHAIN_ROOT="${RK3562_TOOLCHAIN_ROOT:-/home/wu/sdk/kickpi-rk356x/download/rk356x-linux/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu}"
BUILD_JOBS="${BUILD_JOBS:-1}"
READELF_BIN="${READELF_BIN:-}"
RUNTIME_RPATH='$ORIGIN/../lib'
LOG_FILE=""

log()
{
    message="[build-arm64-rk3562] $*"
    printf '%s\n' "${message}"
    if [ -n "${LOG_FILE}" ]; then
        printf '%s\n' "${message}" >> "${LOG_FILE}"
    fi
}

die()
{
    message="[build-arm64-rk3562] ERROR: $*"
    printf '%s\n' "${message}" >&2
    if [ -n "${LOG_FILE}" ]; then
        printf '%s\n' "${message}" >> "${LOG_FILE}"
    fi
    exit 1
}

need_cmd()
{
    command -v "$1" >/dev/null 2>&1 || die "缺少命令：$1"
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

resolve_regular_file()
{
    source_path="$1"
    description="$2"
    [ -e "${source_path}" ] || [ -L "${source_path}" ] ||
        die "${description}不存在：${source_path}"
    resolved_path="$(readlink -f -- "${source_path}" 2>/dev/null || true)"
    [ -n "${resolved_path}" ] && [ -f "${resolved_path}" ] ||
        die "${description}不是有效的常规文件（可能是失效符号链接）：${source_path}"
    printf '%s\n' "${resolved_path}"
}

detect_readelf()
{
    if [ -n "${READELF_BIN}" ]; then
        command -v "${READELF_BIN}" >/dev/null 2>&1 ||
            die "找不到指定的 READELF_BIN：${READELF_BIN}"
        return
    fi

    toolchain_readelf="${RK3562_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-readelf"
    if [ -x "${toolchain_readelf}" ]; then
        READELF_BIN="${toolchain_readelf}"
    elif command -v aarch64-none-linux-gnu-readelf >/dev/null 2>&1; then
        READELF_BIN="aarch64-none-linux-gnu-readelf"
    elif command -v readelf >/dev/null 2>&1; then
        READELF_BIN="readelf"
    else
        die "缺少 readelf，无法校验 AArch64 依赖和构建产物"
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
    elf_path="$1"
    description="$2"

    file_output="$(LC_ALL=C file -L -- "${elf_path}" 2>&1 || true)"
    if ! header_output="$(LC_ALL=C "${READELF_BIN}" -hW "${elf_path}" 2>&1)"; then
        die "${description}不是可读取的 ELF 文件：${elf_path}；file=${file_output}；readelf=${header_output}"
    fi

    elf_class="$(elf_header_value "${header_output}" Class)"
    elf_machine="$(elf_header_value "${header_output}" Machine)"
    [ "${elf_class}" = "ELF64" ] ||
        die "${description}位数错误：要求 ELF64，检测到 ${elf_class:-Unknown}；${file_output}"
    [ "${elf_machine}" = "AArch64" ] ||
        die "${description}架构错误：要求 AArch64，检测到 ${elf_machine:-Unknown}；${file_output}"

    log "${description}架构校验通过：Class=${elf_class}；Machine=${elf_machine}；file=${file_output}"
}

validate_soname()
{
    library_path="$1"
    expected_soname="$2"
    description="$3"
    library_dynamic="$(LC_ALL=C "${READELF_BIN}" -dW "${library_path}" 2>&1)" ||
        die "无法读取 ${description} 动态段：${library_path}"
    printf '%s\n' "${library_dynamic}" |
        LC_ALL=C grep -F "Library soname: [${expected_soname}]" >/dev/null ||
        die "${description} SONAME 不符合要求：预期 ${expected_soname}；${library_path}"
}

validate_toolchain_files()
{
    [ -d "${RK3562_TOOLCHAIN_ROOT}" ] ||
        die "RK3562 工具链目录不存在：${RK3562_TOOLCHAIN_ROOT}"
    TOOLCHAIN_ROOT_REAL="$(readlink -f -- "${RK3562_TOOLCHAIN_ROOT}" 2>/dev/null || true)"
    [ -n "${TOOLCHAIN_ROOT_REAL}" ] && [ -d "${TOOLCHAIN_ROOT_REAL}" ] ||
        die "无法解析 RK3562 工具链目录：${RK3562_TOOLCHAIN_ROOT}"

    EXPECTED_C_COMPILER="$(resolve_regular_file "${TOOLCHAIN_ROOT_REAL}/bin/aarch64-none-linux-gnu-gcc" "AArch64 C 编译器")"
    EXPECTED_CXX_COMPILER="$(resolve_regular_file "${TOOLCHAIN_ROOT_REAL}/bin/aarch64-none-linux-gnu-g++" "AArch64 C++ 编译器")"
    RK3562_TOOLCHAIN_ROOT="${TOOLCHAIN_ROOT_REAL}"
    export RK3562_TOOLCHAIN_ROOT
    log "工具链已锁定：root=${TOOLCHAIN_ROOT_REAL}；C=${EXPECTED_C_COMPILER}；CXX=${EXPECTED_CXX_COMPILER}"
}

validate_private_dependencies()
{
    [ -d "${ARM64_DEPS_ROOT}" ] || die "ARM64 私有依赖目录不存在：${ARM64_DEPS_ROOT}"
    DEPS_ROOT_REAL="$(readlink -f -- "${ARM64_DEPS_ROOT}" 2>/dev/null || true)"
    [ -n "${DEPS_ROOT_REAL}" ] && [ -d "${DEPS_ROOT_REAL}" ] ||
        die "无法解析 ARM64 私有依赖目录：${ARM64_DEPS_ROOT}"

    MOSQUITTO_HEADER="$(resolve_regular_file "${ARM64_DEPS_ROOT}/include/mosquitto.h" "Mosquitto 头文件")"
    MOSQUITTO_LIBRARY="$(resolve_regular_file "${ARM64_DEPS_ROOT}/lib/libmosquitto.so.1" "Mosquitto ARM64 库")"
    SSL_LIBRARY="$(resolve_regular_file "${ARM64_DEPS_ROOT}/lib/libssl.so.3" "OpenSSL SSL ARM64 库")"
    CRYPTO_LIBRARY="$(resolve_regular_file "${ARM64_DEPS_ROOT}/lib/libcrypto.so.3" "OpenSSL Crypto ARM64 库")"

    path_is_within "${MOSQUITTO_HEADER}" "${DEPS_ROOT_REAL}" ||
        die "Mosquitto 头文件解析后越出 ARM64_DEPS_ROOT：${MOSQUITTO_HEADER}"
    path_is_within "${MOSQUITTO_LIBRARY}" "${DEPS_ROOT_REAL}" ||
        die "Mosquitto 库解析后越出 ARM64_DEPS_ROOT：${MOSQUITTO_LIBRARY}"
    path_is_within "${SSL_LIBRARY}" "${DEPS_ROOT_REAL}" ||
        die "OpenSSL SSL 库解析后越出 ARM64_DEPS_ROOT：${SSL_LIBRARY}"
    path_is_within "${CRYPTO_LIBRARY}" "${DEPS_ROOT_REAL}" ||
        die "OpenSSL Crypto 库解析后越出 ARM64_DEPS_ROOT：${CRYPTO_LIBRARY}"

    MOSQUITTO_INCLUDE_DIR="$(dirname -- "${MOSQUITTO_HEADER}")"
    validate_aarch64_elf "${MOSQUITTO_LIBRARY}" "libmosquitto"
    validate_aarch64_elf "${SSL_LIBRARY}" "libssl"
    validate_aarch64_elf "${CRYPTO_LIBRARY}" "libcrypto"
    validate_soname "${MOSQUITTO_LIBRARY}" "libmosquitto.so.1" "libmosquitto"
    validate_soname "${SSL_LIBRARY}" "libssl.so.3" "libssl"
    validate_soname "${CRYPTO_LIBRARY}" "libcrypto.so.3" "libcrypto"

    mosquitto_dynamic="$(LC_ALL=C "${READELF_BIN}" -dW "${MOSQUITTO_LIBRARY}" 2>&1)" ||
        die "无法读取 libmosquitto 动态段：${MOSQUITTO_LIBRARY}"
    printf '%s\n' "${mosquitto_dynamic}" |
        LC_ALL=C grep -F 'Shared library: [libssl.so.3]' >/dev/null ||
        die "libmosquitto 未依赖 libssl.so.3；当前 libmosquitto 可能是 WITH_TLS=OFF 构建，不符合产品 MQTT TLS 要求"
    printf '%s\n' "${mosquitto_dynamic}" |
        LC_ALL=C grep -F 'Shared library: [libcrypto.so.3]' >/dev/null ||
        die "libmosquitto 未依赖 libcrypto.so.3；当前 libmosquitto 可能是 WITH_TLS=OFF 构建，不符合产品 MQTT TLS 要求"

    log "私有依赖已锁定：include=${MOSQUITTO_INCLUDE_DIR}；mosquitto=${MOSQUITTO_LIBRARY}；ssl=${SSL_LIBRARY}；crypto=${CRYPTO_LIBRARY}"
    log "MQTT TLS 校验通过：libmosquitto.so.1 NEEDED 包含 libssl.so.3 和 libcrypto.so.3"
}

cache_value()
{
    cache_key="$1"
    [ -f "${BUILD_DIR}/CMakeCache.txt" ] || return 0
    LC_ALL=C sed -n "s|^${cache_key}:[^=]*=||p" "${BUILD_DIR}/CMakeCache.txt" | head -n 1
}

read_cmake_compiler()
{
    compiler_variable="$1"
    compiler_info_name="$2"
    cached_compiler="$(cache_value "${compiler_variable}")"
    if [ -n "${cached_compiler}" ]; then
        printf '%s\n' "${cached_compiler}"
        return 0
    fi

    cache_path="${BUILD_DIR}/CMakeCache.txt"
    c_info_pattern="${BUILD_DIR}/CMakeFiles/*/CMakeCCompiler.cmake"
    cxx_info_pattern="${BUILD_DIR}/CMakeFiles/*/CMakeCXXCompiler.cmake"
    search_hint="已检查 CMakeCache.txt=${cache_path}；C 编译器文件=${c_info_pattern}；C++ 编译器文件=${cxx_info_pattern}。请检查 CMake 配置日志：${LOG_FILE}"

    shopt -s nullglob
    compiler_info_files=("${BUILD_DIR}"/CMakeFiles/*/"${compiler_info_name}")
    shopt -u nullglob
    if [ "${#compiler_info_files[@]}" -eq 0 ]; then
        die "无法读取 ${compiler_variable}：未找到 ${compiler_info_name}。${search_hint}"
    fi
    if [ "${#compiler_info_files[@]}" -ne 1 ]; then
        compiler_info_matches="$(printf '%s；' "${compiler_info_files[@]}")"
        die "无法明确选择 ${compiler_variable}：发现多个 ${compiler_info_name}：${compiler_info_matches}。${search_hint}"
    fi

    compiler_info_file="${compiler_info_files[0]}"
    mapfile -t parsed_compilers < <(
        LC_ALL=C sed -n \
            "s|^[[:space:]]*set([[:space:]]*${compiler_variable}[[:space:]]*\"\([^\"]*\)\"[[:space:]]*)[[:space:]]*$|\1|p" \
            "${compiler_info_file}"
    )
    if [ "${#parsed_compilers[@]}" -ne 1 ] || [ -z "${parsed_compilers[0]}" ]; then
        die "无法从 ${compiler_info_file} 唯一解析 ${compiler_variable}。${search_hint}"
    fi
    printf '%s\n' "${parsed_compilers[0]}"
}

validate_cmake_selection()
{
    selected_c_compiler="$(read_cmake_compiler CMAKE_C_COMPILER CMakeCCompiler.cmake)"
    selected_cxx_compiler="$(read_cmake_compiler CMAKE_CXX_COMPILER CMakeCXXCompiler.cmake)"
    selected_include="$(cache_value MOSQUITTO_INCLUDE_DIR)"
    selected_library="$(cache_value MOSQUITTO_LIBRARY)"
    selected_package_version="$(cache_value EDGE_CONTROLLER_PACKAGE_VERSION)"
    selected_c_compiler_real="$(readlink -f -- "${selected_c_compiler}" 2>/dev/null || true)"
    selected_cxx_compiler_real="$(readlink -f -- "${selected_cxx_compiler}" 2>/dev/null || true)"
    selected_include_real="$(readlink -f -- "${selected_include}" 2>/dev/null || true)"
    selected_library_real="$(readlink -f -- "${selected_library}" 2>/dev/null || true)"

    [ "${selected_c_compiler_real}" = "${EXPECTED_C_COMPILER}" ] ||
        die "CMake 实际使用的 C 编译器不匹配：实际=${selected_c_compiler_real:-${selected_c_compiler:-<空>}}；期望=${EXPECTED_C_COMPILER}。请删除 ${BUILD_DIR} 后重新配置"
    [ "${selected_cxx_compiler_real}" = "${EXPECTED_CXX_COMPILER}" ] ||
        die "CMake 实际使用的 C++ 编译器不匹配：实际=${selected_cxx_compiler_real:-${selected_cxx_compiler:-<空>}}；期望=${EXPECTED_CXX_COMPILER}。请删除 ${BUILD_DIR} 后重新配置"

    [ -n "${selected_include_real}" ] && path_is_within "${selected_include_real}" "${DEPS_ROOT_REAL}" ||
        die "CMake 选中了 ARM64_DEPS_ROOT 之外的 Mosquitto 头文件：${selected_include:-<空>}"
    [ -n "${selected_library_real}" ] && path_is_within "${selected_library_real}" "${DEPS_ROOT_REAL}" ||
        die "CMake 选中了 ARM64_DEPS_ROOT 之外的 Mosquitto 库：${selected_library:-<空>}"
    [ "${selected_library_real}" = "${MOSQUITTO_LIBRARY}" ] ||
        die "CMake 选中的 Mosquitto 库与已校验库不一致：${selected_library_real}"
    [ -n "${selected_package_version}" ] ||
        die "CMake 未生成 EDGE_CONTROLLER_PACKAGE_VERSION"
    if [ -n "${PACKAGE_VERSION}" ] && [ "${selected_package_version}" != "${PACKAGE_VERSION}" ]; then
        die "controller 编译版本与发布版本不一致：编译=${selected_package_version}；发布=${PACKAGE_VERSION}"
    fi
    validate_aarch64_elf "${selected_library_real}" "CMake 选中的 libmosquitto"

    case "${selected_include_real}:${selected_library_real}" in
        *'/usr/lib/x86_64-linux-gnu/'*|*x86_64*)
            die "CMake 误选了主机 x86_64 Mosquitto：${selected_include_real}；${selected_library_real}"
            ;;
    esac
    log "CMake 工具链缓存校验通过：C=${selected_c_compiler_real}；CXX=${selected_cxx_compiler_real}"
    log "CMake Mosquitto 选择校验通过"
    log "controller 编译版本校验通过：${selected_package_version}"
}

validate_thread_link()
{
    forbidden_link_workaround="--no-as""-needed"
    link_evidence_file=""

    if [ -f "${BUILD_DIR}/CMakeFiles/edge-controller.dir/link.txt" ]; then
        link_evidence_file="${BUILD_DIR}/CMakeFiles/edge-controller.dir/link.txt"
    else
        for reply_file in "${BUILD_DIR}"/.cmake/api/v1/reply/target-edge-controller-*.json; do
            if [ -f "${reply_file}" ] && LC_ALL=C grep -F -- '-pthread' "${reply_file}" >/dev/null; then
                link_evidence_file="${reply_file}"
                break
            fi
        done
    fi

    [ -n "${link_evidence_file}" ] ||
        die "无法从 link.txt 或 CMake File API 找到 edge-controller 最终链接信息"
    LC_ALL=C grep -F -- '-pthread' "${link_evidence_file}" >/dev/null ||
        die "edge-controller 最终链接信息未包含由 Threads::Threads 产生的 -pthread：${link_evidence_file}"
    if LC_ALL=C grep -F -- "${forbidden_link_workaround}" "${link_evidence_file}" >/dev/null; then
        die "edge-controller 最终链接命令仍包含禁止的临时链接绕过参数"
    fi
    if LC_ALL=C grep -R -F -- "${forbidden_link_workaround}" \
        "${PROJECT_ROOT}/backend" "${PROJECT_ROOT}/scripts" >/dev/null; then
        die "仓库 backend 或 scripts 中仍包含禁止的临时链接绕过参数"
    fi
    log "线程链接校验通过：${link_evidence_file} 包含 -pthread，且未使用临时链接绕过参数"
}

validate_controller()
{
    controller_bin="${BUILD_DIR}/edge-controller"
    [ -f "${controller_bin}" ] || die "构建完成但产物不存在：${controller_bin}"
    [ -x "${controller_bin}" ] || die "构建产物不可执行：${controller_bin}"
    validate_aarch64_elf "${controller_bin}" "edge-controller"

    program_output="$(LC_ALL=C "${READELF_BIN}" -lW "${controller_bin}" 2>&1)" ||
        die "无法读取 edge-controller 程序头：${controller_bin}"
    printf '%s\n' "${program_output}" |
        LC_ALL=C grep -F 'Requesting program interpreter: /lib/ld-linux-aarch64.so.1' >/dev/null ||
        die "edge-controller 动态解释器不是 /lib/ld-linux-aarch64.so.1"

    dynamic_output="$(LC_ALL=C "${READELF_BIN}" -dW "${controller_bin}" 2>&1)" ||
        die "无法读取 edge-controller 动态段：${controller_bin}"
    printf '%s\n' "${dynamic_output}" |
        LC_ALL=C grep -F 'Shared library: [libmosquitto.so.1]' >/dev/null ||
        die "edge-controller 未声明 libmosquitto.so.1 动态依赖"
    detected_rpath="$(printf '%s\n' "${dynamic_output}" |
        LC_ALL=C sed -n \
            -e 's/.*(RPATH).*Library rpath: \[\([^]]*\)\].*/\1/p' \
            -e 's/.*(RUNPATH).*Library runpath: \[\([^]]*\)\].*/\1/p' |
        head -n 1)"
    [ "${detected_rpath}" = "${RUNTIME_RPATH}" ] ||
        die "edge-controller RPATH/RUNPATH 必须固定为 ${RUNTIME_RPATH}，检测到 ${detected_rpath:-<空>}"
    if printf '%s\n' "${dynamic_output}" |
        LC_ALL=C grep -E '/usr/lib/x86_64-linux-gnu|(^|[^[:alnum:]_])x86_64([^[:alnum:]_]|$)' >/dev/null; then
        die "edge-controller 动态依赖包含主机 x86_64 路径"
    fi

    artifact_size="$(stat -c '%s' -- "${controller_bin}")"
    artifact_sha256="$(sha256sum -- "${controller_bin}" | awk '{print $1}')"
    log "产物校验通过：${controller_bin}"
    log "动态解释器：/lib/ld-linux-aarch64.so.1；依赖：libmosquitto.so.1；RPATH/RUNPATH=${RUNTIME_RPATH}"
    log "产物大小：${artifact_size} bytes；SHA-256：${artifact_sha256}"
}

on_error()
{
    status="$?"
    printf '[build-arm64-rk3562] ERROR: 构建失败（退出码 %s）；构建目录和日志已保留：%s；%s\n' \
        "${status}" "${BUILD_DIR}" "${LOG_FILE:-<尚未创建>}" >&2
    exit "${status}"
}

main()
{
    trap on_error ERR

    need_cmd cmake
    need_cmd file
    need_cmd readlink
    need_cmd sed
    need_cmd grep
    need_cmd head
    need_cmd awk
    need_cmd tee
    need_cmd stat
    need_cmd sha256sum

    case "${BUILD_JOBS}" in
        ''|*[!0-9]*|0) die "BUILD_JOBS 必须是正整数，当前值：${BUILD_JOBS}" ;;
    esac
    [ -f "${TOOLCHAIN_FILE}" ] || die "找不到 RK3562 ARM64 toolchain 文件：${TOOLCHAIN_FILE}"
    [ -n "${BUILD_DIR}" ] && [ "${BUILD_DIR}" != "/" ] || die "BUILD_DIR 不能为根目录"

    mkdir -p "${BUILD_DIR}"
    LOG_FILE="${BUILD_DIR}/build-arm64-rk3562.log"
    : > "${LOG_FILE}"
    mkdir -p "${BUILD_DIR}/.cmake/api/v1/query"
    : > "${BUILD_DIR}/.cmake/api/v1/query/codemodel-v2"

    validate_toolchain_files
    detect_readelf
    validate_private_dependencies

    log "配置 Release 构建：build=${BUILD_DIR}；jobs=${BUILD_JOBS}；toolchain=${TOOLCHAIN_FILE}"
    log "默认关闭 IPO/LTO；Release flags=-O2 -DNDEBUG；RPATH/RUNPATH=${RUNTIME_RPATH}"
    cmake \
        -S "${PROJECT_ROOT}/backend" \
        -B "${BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE:FILEPATH="${TOOLCHAIN_FILE}" \
        -DCMAKE_PREFIX_PATH:PATH="${DEPS_ROOT_REAL}" \
        -DMOSQUITTO_INCLUDE_DIR:PATH="${MOSQUITTO_INCLUDE_DIR}" \
        -DMOSQUITTO_LIBRARY:FILEPATH="${MOSQUITTO_LIBRARY}" \
        "-DPACKAGE_VERSION:STRING=${PACKAGE_VERSION}" \
        -DCMAKE_BUILD_TYPE:STRING=Release \
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION:BOOL=OFF \
        -DCMAKE_C_FLAGS_RELEASE:STRING='-O2 -DNDEBUG' \
        -DCMAKE_CXX_FLAGS_RELEASE:STRING='-O2 -DNDEBUG' \
        "-DCMAKE_EXE_LINKER_FLAGS:STRING=-Wl,-rpath-link,${DEPS_ROOT_REAL}/lib" \
        "-DCMAKE_BUILD_RPATH:STRING=${RUNTIME_RPATH}" \
        "-DCMAKE_INSTALL_RPATH:STRING=${RUNTIME_RPATH}" \
        -DCMAKE_BUILD_WITH_INSTALL_RPATH:BOOL=ON \
        -DCMAKE_INSTALL_RPATH_USE_LINK_PATH:BOOL=OFF \
        2>&1 | tee -a "${LOG_FILE}"

    validate_cmake_selection

    log "编译正式目标 edge-controller（并发数 ${BUILD_JOBS}）"
    cmake --build "${BUILD_DIR}" --target edge-controller --parallel "${BUILD_JOBS}" --verbose \
        2>&1 | tee -a "${LOG_FILE}"

    validate_thread_link
    validate_controller
    log "完成；完整日志：${LOG_FILE}"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    main "$@"
fi
