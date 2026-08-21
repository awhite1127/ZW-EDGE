#!/usr/bin/env python3
"""RK3562 应用升级校验器、任务创建器和独立恢复运行器。

运行器会把自身复制到任务和稳定恢复目录，升级中断时不能依赖正在切换的程序树，
因此保持单文件自包含；状态迁移、原子切换和回滚语义集中在本文件内审阅。
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import hashlib
import http.client
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid
from pathlib import Path, PurePosixPath
from typing import Any, Iterator, Optional

try:
    import fcntl
    import grp
    import pwd
except ImportError:  # pragma: no cover - only permits host-side contract tests on Windows.
    fcntl = None  # type: ignore[assignment]
    grp = None  # type: ignore[assignment]
    pwd = None  # type: ignore[assignment]


PRODUCT_ROOT = Path("/opt/edge-controller")
UPDATE_STATES = {
    "IDLE", "VALIDATING", "READY", "BACKING_UP", "STOPPING", "INSTALLING",
    "STARTING", "VERIFYING", "SUCCESS", "FAILED", "ROLLING_BACK", "ROLLED_BACK",
}
TERMINAL_STATES = {"IDLE", "SUCCESS", "FAILED", "ROLLED_BACK"}
ACTIVE_STATES = {
    "VALIDATING", "BACKING_UP", "STOPPING", "INSTALLING", "STARTING",
    "VERIFYING", "ROLLING_BACK",
}
RECOVERY_ROLLBACK_STATES = {
    "STOPPING", "INSTALLING", "STARTING", "VERIFYING", "ROLLING_BACK",
}
ALLOWED_TRANSITIONS = {
    "IDLE": {"VALIDATING"},
    "VALIDATING": {"READY", "FAILED"},
    "READY": {"BACKING_UP", "FAILED"},
    "BACKING_UP": {"STOPPING", "FAILED", "ROLLING_BACK"},
    "STOPPING": {"INSTALLING", "FAILED", "ROLLING_BACK"},
    "INSTALLING": {"STARTING", "ROLLING_BACK"},
    "STARTING": {"VERIFYING", "ROLLING_BACK"},
    "VERIFYING": {"SUCCESS", "ROLLING_BACK"},
    "ROLLING_BACK": {"ROLLED_BACK", "FAILED"},
    "SUCCESS": {"VALIDATING"},
    "FAILED": {"VALIDATING"},
    "ROLLED_BACK": {"VALIDATING"},
}
PACKAGE_NAME_RE = re.compile(r"^edge-controller-rk3562-([A-Za-z0-9._+-]+)\.tar\.gz$")
JOB_ID_RE = re.compile(r"^[0-9]{8}T[0-9]{6}Z-[0-9a-f]{8}$")
UPLOAD_ID_RE = re.compile(r"^[0-9a-f]{32}$")
MANIFEST_RE = re.compile(r"^([0-9a-fA-F]{64})  (\./[^\r\n]+)$")
SIGNATURE_FILE = "MANIFEST.sha256.sig"
REQUIRED_PATHS = (
    "bin/edge-controller",
    "bin/edge-web",
    "config/edge-controller.env.default",
    "systemd/edge-controller.service",
    "systemd/edge-web.service",
    "systemd/edge-log-rotator.service",
    "systemd/edge-upgrade@.service",
    "systemd/edge-upgrade-recovery.service",
    "systemd/edge-upgrade-recovery-verify.service",
    "scripts/install.sh",
    "scripts/verify-install.sh",
    "scripts/rotate-logs.sh",
    "scripts/update-manager.py",
    "VERSION",
    "package-info.json",
    "MANIFEST.sha256",
    "README.md",
)
# 回滚快照可能来自尚未包含 recovery systemd 单元的旧安装。它的完整性契约必须与
# 新升级包要求解耦，避免新增恢复资产后无法为仍受支持的旧安装创建可用备份。
ROLLBACK_REQUIRED_PATHS = (
    "bin/edge-controller",
    "bin/edge-web",
    "config/edge-controller.env.default",
    "systemd/edge-controller.service",
    "systemd/edge-web.service",
    "systemd/edge-log-rotator.service",
    "systemd/edge-upgrade@.service",
    "scripts/install.sh",
    "scripts/verify-install.sh",
    "scripts/rotate-logs.sh",
    "scripts/update-manager.py",
    "VERSION",
    "package-info.json",
    "MANIFEST.sha256",
    "README.md",
)
PROGRAM_DIRS = ("bin", "lib", "systemd", "scripts")
PROGRAM_FILES = ("VERSION", "package-info.json", "MANIFEST.sha256", "README.md")
PROGRAM_OPTIONAL_FILES = (SIGNATURE_FILE,)
SERVICES = ("edge-controller.service", "edge-web.service", "edge-log-rotator.service")
STOP_ORDER = ("edge-web.service", "edge-controller.service", "edge-log-rotator.service")
STABLE_SYSTEMD_UNITS = (
    "edge-upgrade@.service",
    "edge-upgrade-recovery.service",
    "edge-upgrade-recovery-verify.service",
)
SERVICE_GATE_DROPIN_NAME = "edge-upgrade-recovery-gate.conf"
KEEP_JOBS = 2
KEEP_BACKUPS = 2
KEEP_INCOMING = 3
KEEP_UPLOADS = 4
UPLOAD_TTL_SECONDS = 24 * 60 * 60
MAX_STAGED_UPLOAD_BYTES = 512 * 1024 * 1024
MAX_RECOVERY_ATTEMPTS = 3
SAFETY_MARGIN_BYTES = 256 * 1024 * 1024
MAX_UPLOAD_BYTES = 256 * 1024 * 1024
BACKUP_METADATA_FILE = "backup.json"
RECOVERY_MANAGER_FILE = "update-manager.py"
SIGNATURE_POLICIES = {"optional", "required"}
DEFAULT_SIGNATURE_POLICY = "optional"


class UpdateError(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def read_version(root: Path) -> str:
    try:
        return (root / "VERSION").read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def workspace_paths(root: Path) -> dict[str, Path]:
    update = root / "update"
    return {
        "update": update,
        "incoming": update / "incoming",
        "upload": update / "upload",
        "jobs": update / "jobs",
        "backup": update / "backup",
        "recovery": update / "recovery",
        "status": update / "status.json",
        "lock": update / ".update.lock",
    }


def ensure_workspace(root: Path) -> dict[str, Path]:
    paths = workspace_paths(root)
    root.mkdir(mode=0o750, parents=True, exist_ok=True)
    if root.is_symlink() or not root.is_dir():
        raise UpdateError("unsafe_workspace", f"产品根目录不能是链接且必须为目录：{root}")
    paths["update"].mkdir(mode=0o750, exist_ok=True)
    paths["incoming"].mkdir(mode=0o750, exist_ok=True)
    paths["upload"].mkdir(mode=0o770, exist_ok=True)
    paths["jobs"].mkdir(mode=0o700, exist_ok=True)
    paths["backup"].mkdir(mode=0o700, exist_ok=True)
    paths["recovery"].mkdir(mode=0o700, exist_ok=True)
    for key in ("update", "incoming", "upload", "jobs", "backup", "recovery"):
        if paths[key].is_symlink() or not paths[key].is_dir():
            raise UpdateError("unsafe_workspace", f"升级工作区不能是链接且必须为目录：{paths[key]}")
    for key in ("update", "incoming"):
        os.chmod(paths[key], 0o750)
    os.chmod(paths["upload"], 0o770)
    for key in ("jobs", "backup", "recovery"):
        os.chmod(paths[key], 0o700)
    if getattr(os, "geteuid", lambda: -1)() == 0 and grp is not None:
        try:
            group_id = grp.getgrnam("edge-controller").gr_gid
            os.chown(paths["update"], 0, group_id)
            os.chown(paths["incoming"], 0, group_id)
            os.chown(paths["upload"], 0, group_id)
        except KeyError:
            pass
    return paths


def default_status(root: Path) -> dict[str, Any]:
    return {
        "job_id": "",
        "state": "IDLE",
        "current_version": read_version(root),
        "target_version": "",
        "package_path": "",
        "created_at": "",
        "started_at": "",
        "finished_at": "",
        "progress": {"percent": 0, "stage": "idle"},
        "message": "暂无升级任务",
        "error_code": "",
        "error_message": "",
        "rollback_performed": False,
        "upgrade_error": "",
        "rollback_error": "",
        "runner_requested": False,
        "runner_pid": 0,
        "runner_unit": "",
        "last_heartbeat": "",
        "interrupted": False,
        "recovery_required": False,
        "recovery_pending_health": False,
        "recovery_started_at": "",
        "recovery_finished_at": "",
        "recovery_reason": "",
        "recovery_from_state": "",
        "recovery_attempt_count": 0,
        "recovery_error": "",
        "backup_id": "",
        "backup_complete": False,
        "previous_version": "",
        "signature": {
            "required": False, "status": "unsigned", "scheme": "none",
            "key_id": "", "signature_file": "", "signed": False,
            "signature_verified": False, "signature_algorithm": "none",
        },
        "package_info": {
            "product": "", "platform": "", "arch": "", "version": "",
            "build_time": "", "size_bytes": 0,
        },
    }


def read_status(root: Path) -> dict[str, Any]:
    status_path = workspace_paths(root)["status"]
    if not status_path.exists():
        return default_status(root)
    if status_path.is_symlink() or not status_path.is_file():
        raise UpdateError("status_corrupt", "升级状态文件不能是链接且必须为常规文件")
    try:
        value = json.loads(status_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise UpdateError("status_corrupt", f"升级状态文件不可读：{error}") from error
    if not isinstance(value, dict) or value.get("state") not in UPDATE_STATES:
        raise UpdateError("status_corrupt", "升级状态文件结构或 state 非法")
    normalized = default_status(root)
    normalized.update(value)
    for key in ("signature", "package_info"):
        nested = default_status(root)[key]
        if isinstance(value.get(key), dict):
            nested.update(value[key])
        normalized[key] = nested
    return normalized


def atomic_write_json(path: Path, value: dict[str, Any], mode: int = 0o640) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        if getattr(os, "geteuid", lambda: -1)() == 0 and path.name == "status.json" and grp is not None:
            try:
                os.chown(temporary, 0, grp.getgrnam("edge-controller").gr_gid)
            except KeyError:
                pass
        os.replace(temporary, path)
        try:
            directory_fd = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        except OSError:
            directory_fd = -1
        if directory_fd >= 0:
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
    finally:
        with contextlib.suppress(FileNotFoundError):
            temporary.unlink()


def write_status(root: Path, value: dict[str, Any]) -> None:
    if value.get("state") not in UPDATE_STATES:
        raise UpdateError("invalid_state", f"不支持的升级状态：{value.get('state')}")
    atomic_write_json(workspace_paths(root)["status"], value)


def transition(root: Path, state: str, percent: int, stage: str, message: str, **fields: Any) -> dict[str, Any]:
    status = read_status(root)
    previous = str(status.get("state", "IDLE"))
    if state != previous and state not in ALLOWED_TRANSITIONS.get(previous, set()):
        raise UpdateError("invalid_transition", f"升级状态不能从 {previous} 转换到 {state}")
    status.update(fields)
    status["state"] = state
    status["progress"] = {"percent": max(0, min(100, int(percent))), "stage": stage}
    status["message"] = message
    if state in ACTIVE_STATES or (state == "READY" and status.get("runner_requested")):
        status["last_heartbeat"] = utc_now()
    write_status(root, status)
    return status


@contextlib.contextmanager
def update_lock(root: Path, blocking: bool = False) -> Iterator[None]:
    if fcntl is None:
        raise UpdateError("unsupported_platform", "升级锁仅支持 Linux")
    paths = ensure_workspace(root)
    descriptor = os.open(paths["lock"], os.O_CREAT | os.O_RDWR, 0o600)
    try:
        operation = fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB)
        try:
            fcntl.flock(descriptor, operation)
        except BlockingIOError as error:
            raise UpdateError("update_busy", "另一个升级任务正在执行") from error
        yield
    finally:
        os.close(descriptor)


def normalized_archive_path(name: str) -> PurePosixPath:
    path = PurePosixPath(name)
    if not name or path.is_absolute() or ".." in path.parts or path.parts[0] != "edge-controller":
        raise UpdateError("unsafe_archive_path", f"升级包包含不安全路径：{name}")
    return path


def safe_link_target(member: tarfile.TarInfo, members: set[str]) -> None:
    if not (member.issym() or member.islnk()):
        return
    link = PurePosixPath(member.linkname)
    if link.is_absolute():
        raise UpdateError("unsafe_archive_link", f"升级包链接使用绝对目标：{member.name} -> {member.linkname}")
    if member.issym():
        combined = PurePosixPath(member.name).parent / link
    else:
        combined = link
    parts: list[str] = []
    for part in combined.parts:
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                raise UpdateError("unsafe_archive_link", f"升级包链接逃逸：{member.name} -> {member.linkname}")
            parts.pop()
        else:
            parts.append(part)
    if not parts or parts[0] != "edge-controller":
        raise UpdateError("unsafe_archive_link", f"升级包链接逃逸：{member.name} -> {member.linkname}")
    if member.islnk() and "/".join(parts) not in members:
        raise UpdateError("unsafe_archive_link", f"升级包硬链接目标不存在：{member.name} -> {member.linkname}")


def inspect_archive(archive: Path) -> tuple[list[tarfile.TarInfo], int]:
    try:
        with tarfile.open(archive, "r:gz") as bundle:
            members = bundle.getmembers()
    except (tarfile.TarError, OSError) as error:
        raise UpdateError("invalid_archive", f"升级包格式错误：{error}") from error
    if not members:
        raise UpdateError("empty_archive", "升级包为空")
    names: set[str] = set()
    link_names: set[str] = set()
    total_size = 0
    for member in members:
        normalized_archive_path(member.name)
        if member.name in names:
            raise UpdateError("duplicate_archive_path", f"升级包包含重复路径：{member.name}")
        names.add(member.name)
        if not (member.isdir() or member.isfile() or member.issym() or member.islnk()):
            raise UpdateError("unsupported_archive_entry", f"升级包包含不支持的特殊文件：{member.name}")
        if member.issym() or member.islnk():
            link_names.add(member.name.rstrip("/"))
        if member.isfile():
            total_size += member.size
    for member in members:
        safe_link_target(member, names)
        parts = PurePosixPath(member.name).parts
        for index in range(1, len(parts)):
            if "/".join(parts[:index]) in link_names:
                raise UpdateError("unsafe_archive_link", f"升级包路径位于链接目录下：{member.name}")
    return members, total_size


def extract_archive(archive: Path, destination: Path) -> Path:
    members, _ = inspect_archive(archive)
    destination.mkdir(mode=0o700, parents=True, exist_ok=False)
    try:
        with tarfile.open(archive, "r:gz") as bundle:
            bundle.extractall(destination, members=members, filter="data")
    except (tarfile.TarError, OSError) as error:
        raise UpdateError("extract_failed", f"安全解压升级包失败：{error}") from error
    extracted = destination / "edge-controller"
    if not extracted.is_dir() or extracted.is_symlink():
        raise UpdateError("invalid_top_level", "升级包必须且只能包含 edge-controller/ 顶层目录")
    return extracted


def load_package_info(package_root: Path) -> dict[str, Any]:
    try:
        value = json.loads((package_root / "package-info.json").read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise UpdateError("missing_package_info", "升级包缺少 package-info.json") from error
    except (OSError, json.JSONDecodeError) as error:
        raise UpdateError("invalid_package_info", f"package-info.json 无法解析：{error}") from error
    if not isinstance(value, dict):
        raise UpdateError("invalid_package_info", "package-info.json 顶层必须为对象")
    expected = {
        "product": "edge-controller",
        "platform": "rk3562",
        "arch": "aarch64",
    }
    if type(value.get("package_format_version")) is not int or value["package_format_version"] != 1:
        raise UpdateError("package_mismatch", "package-info.json package_format_version 必须为整数 1")
    for key, expected_value in expected.items():
        if value.get(key) != expected_value:
            raise UpdateError("package_mismatch", f"package-info.json {key} 必须为 {expected_value}")
    version = value.get("version")
    build_time = value.get("build_time")
    if not isinstance(version, str) or not version.strip():
        raise UpdateError("invalid_target_version", "package-info.json version 不能为空")
    if not isinstance(build_time, str) or not build_time.strip():
        raise UpdateError("invalid_build_time", "package-info.json build_time 不能为空")
    signature_format = value.get("signature_format_version", 1)
    signature_algorithm = value.get("signature_algorithm", "none")
    if type(signature_format) is not int or signature_format != 1:
        raise UpdateError("invalid_signature_metadata", "signature_format_version 必须为整数 1")
    if signature_algorithm not in {"none", "ed25519"}:
        raise UpdateError("invalid_signature_metadata", "signature_algorithm 仅支持 none 或 ed25519")
    return value


def read_environment_setting(root: Path, key: str, default: str) -> str:
    environment_value = os.environ.get(key)
    if environment_value is not None and environment_value.strip():
        return environment_value.strip()
    environment_file = root / "config/edge-controller.env"
    configured_value = ""
    with contextlib.suppress(OSError):
        for raw_line in environment_file.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, value = line.split("=", 1)
            if name.strip() == key:
                configured_value = value.strip()
    return configured_value or default


def signature_policy(root: Path = PRODUCT_ROOT) -> str:
    policy = read_environment_setting(root, "EDGE_UPGRADE_SIGNATURE_POLICY", DEFAULT_SIGNATURE_POLICY).lower()
    if policy not in SIGNATURE_POLICIES:
        raise UpdateError("invalid_signature_policy", "EDGE_UPGRADE_SIGNATURE_POLICY 仅支持 optional 或 required")
    return policy


def verify_package_signature(
    package_root: Path,
    info: dict[str, Any],
    trust_root: Path = PRODUCT_ROOT,
) -> dict[str, Any]:
    policy = signature_policy(trust_root)
    required = policy == "required"
    algorithm = str(info.get("signature_algorithm", "none")).lower()
    signing = info.get("signing") if isinstance(info.get("signing"), dict) else {}
    signature_name = str(signing.get("signature_file", ""))
    key_id = str(signing.get("key_id", ""))
    signature_path = package_root / SIGNATURE_FILE
    signature_exists = signature_path.exists() or signature_path.is_symlink()

    if algorithm == "none":
        if signature_exists or signature_name:
            raise UpdateError("invalid_signature_metadata", "未签名包不能携带签名文件或签名文件声明")
        if required:
            raise UpdateError("signature_required", "当前设备只接受带 Ed25519 签名的正式升级包")
        return {
            "required": False, "status": "unsigned", "scheme": "none", "key_id": "",
            "signature_file": "", "signed": False, "signature_verified": False,
            "signature_algorithm": "none",
        }

    if algorithm != "ed25519" or signing.get("scheme") != "ed25519" or signature_name != SIGNATURE_FILE:
        raise UpdateError("invalid_signature_metadata", "Ed25519 签名 metadata 不完整或签名文件名不受支持")
    try:
        signature_metadata = signature_path.lstat()
    except FileNotFoundError as error:
        raise UpdateError("signature_missing", "升级包声明了 Ed25519 签名但缺少签名文件") from error
    if not stat.S_ISREG(signature_metadata.st_mode) or stat.S_ISLNK(signature_metadata.st_mode) or signature_metadata.st_size != 64:
        raise UpdateError("invalid_signature_file", "Ed25519 签名必须是 64 字节常规文件且不能是符号链接")

    public_key_value = read_environment_setting(
        trust_root,
        "EDGE_UPGRADE_PUBLIC_KEY",
        str(trust_root / "security/upgrade-ed25519.pub"),
    )
    public_key = Path(public_key_value)
    try:
        public_key_metadata = public_key.lstat()
    except FileNotFoundError as error:
        raise UpdateError("trusted_key_missing", f"设备升级信任公钥不存在：{public_key}") from error
    if not stat.S_ISREG(public_key_metadata.st_mode) or stat.S_ISLNK(public_key_metadata.st_mode):
        raise UpdateError("trusted_key_invalid", "设备升级信任公钥必须是常规文件且不能是符号链接")
    if stat.S_IMODE(public_key_metadata.st_mode) & 0o022:
        raise UpdateError("trusted_key_permissions", "设备升级信任公钥不能由组或其他用户写入")
    if getattr(os, "geteuid", lambda: -1)() == 0 and public_key_metadata.st_uid != 0:
        raise UpdateError("trusted_key_owner", "设备升级信任公钥必须由 root 持有")
    configured_key_id = read_environment_setting(trust_root, "EDGE_UPGRADE_PUBLIC_KEY_ID", "")
    if configured_key_id and key_id != configured_key_id:
        raise UpdateError("signature_key_mismatch", "升级包签名 key_id 与设备信任根不匹配")

    try:
        run_checked([
            "openssl", "pkeyutl", "-verify", "-rawin", "-pubin", "-inkey", str(public_key),
            "-sigfile", str(signature_path), "-in", str(package_root / "MANIFEST.sha256"),
        ], timeout=30)
    except UpdateError as error:
        raise UpdateError("signature_verification_failed", "升级包 Ed25519 签名验证失败") from error
    return {
        "required": required, "status": "verified", "scheme": "ed25519", "key_id": key_id,
        "signature_file": SIGNATURE_FILE, "signed": True, "signature_verified": True,
        "signature_algorithm": "ed25519",
    }


def ensure_relative_manifest_path(value: str) -> str:
    if not value.startswith("./"):
        raise UpdateError("invalid_manifest", f"MANIFEST 路径必须以 ./ 开头：{value}")
    path = PurePosixPath(value[2:])
    if not path.parts or path.is_absolute() or ".." in path.parts:
        raise UpdateError("invalid_manifest", f"MANIFEST 包含不安全路径：{value}")
    return path.as_posix()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_manifest(package_root: Path) -> None:
    manifest_path = package_root / "MANIFEST.sha256"
    try:
        lines = manifest_path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise UpdateError("missing_manifest", f"升级包缺少 MANIFEST.sha256：{error}") from error
    expected: dict[str, str] = {}
    for line in lines:
        match = MANIFEST_RE.fullmatch(line)
        if match is None:
            raise UpdateError("invalid_manifest", f"MANIFEST.sha256 行格式错误：{line[:160]}")
        relative = ensure_relative_manifest_path(match.group(2))
        if relative == "MANIFEST.sha256" or relative in expected:
            raise UpdateError("invalid_manifest", f"MANIFEST.sha256 路径重复或自引用：{relative}")
        expected[relative] = match.group(1).lower()
    actual: set[str] = set()
    for path in package_root.rglob("*"):
        if path.is_dir() and not path.is_symlink():
            continue
        relative = path.relative_to(package_root).as_posix()
        if relative in {"MANIFEST.sha256", SIGNATURE_FILE}:
            continue
        actual.add(relative)
    if actual != set(expected):
        missing = sorted(actual - set(expected))
        extra = sorted(set(expected) - actual)
        raise UpdateError("manifest_coverage", f"MANIFEST 覆盖不完整：未列出={missing[:5]}；不存在={extra[:5]}")
    root_resolved = package_root.resolve()
    for relative, digest in expected.items():
        path = package_root / relative
        try:
            resolved = path.resolve(strict=True)
            resolved.relative_to(root_resolved)
        except (OSError, ValueError) as error:
            raise UpdateError("unsafe_package_link", f"MANIFEST 文件链接逃逸或无效：{relative}") from error
        if not resolved.is_file() or sha256_file(resolved) != digest:
            raise UpdateError("manifest_mismatch", f"MANIFEST 完整性校验失败：{relative}")


def run_checked(
    arguments: list[str],
    timeout: int = 30,
    environment_overrides: Optional[dict[str, str]] = None,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    if environment_overrides:
        environment.update(environment_overrides)
    try:
        return subprocess.run(
            arguments, text=True, capture_output=True, timeout=timeout, check=True, env=environment
        )
    except FileNotFoundError as error:
        raise UpdateError("missing_command", f"缺少系统命令：{arguments[0]}") from error
    except subprocess.TimeoutExpired as error:
        raise UpdateError("command_timeout", f"命令执行超时：{arguments[0]}") from error
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or error.stdout or "").strip()
        raise UpdateError("command_failed", f"命令失败：{' '.join(arguments[:3])}：{detail[:1000]}") from error


def validate_aarch64_header(path: Path, description: str) -> None:
    header = run_checked(["readelf", "-hW", str(path)]).stdout
    if "Class:                             ELF64" not in header or "Machine:                           AArch64" not in header:
        raise UpdateError("elf_arch_mismatch", f"{description} 不是 ELF64/AArch64")


def validate_elf(path: Path, description: str, static_binary: bool = False) -> None:
    validate_aarch64_header(path, description)
    program = run_checked(["readelf", "-lW", str(path)]).stdout
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    dynamic = subprocess.run(
        ["readelf", "-dW", str(path)], text=True, capture_output=True, timeout=30, env=environment
    )
    dynamic_text = dynamic.stdout
    if static_binary:
        if "INTERP" in program or "(NEEDED)" in dynamic_text:
            raise UpdateError("invalid_static_binary", f"{description} 必须为静态 ELF")
        return
    if "/lib/ld-linux-aarch64.so.1" not in program:
        raise UpdateError("invalid_elf_interpreter", f"{description} 动态解释器错误")
    if "Shared library: [libmosquitto.so.1]" not in dynamic_text:
        raise UpdateError("missing_mosquitto_dependency", f"{description} 缺少 libmosquitto.so.1 NEEDED")
    if "$ORIGIN/../lib" not in dynamic_text:
        raise UpdateError("invalid_rpath", f"{description} RPATH/RUNPATH 不包含 $ORIGIN/../lib")
    if "/usr/lib/x86_64-linux-gnu" in dynamic_text or "x86_64" in dynamic_text:
        raise UpdateError("host_path_in_elf", f"{description} 动态信息包含主机 x86_64 路径")


def validate_sonames(package_root: Path) -> None:
    libraries = {
        "lib/libmosquitto.so.1": "libmosquitto.so.1",
        "lib/libssl.so.3": "libssl.so.3",
        "lib/libcrypto.so.3": "libcrypto.so.3",
        "lib/libstdc++.so.6": "libstdc++.so.6",
        "lib/libgcc_s.so.1": "libgcc_s.so.1",
    }
    dynamic_outputs: dict[str, str] = {}
    for relative, soname in libraries.items():
        output = run_checked(["readelf", "-dW", str(package_root / relative)]).stdout
        dynamic_outputs[relative] = output
        if f"Library soname: [{soname}]" not in output:
            raise UpdateError("invalid_soname", f"{relative} SONAME 不是 {soname}")
    mosquitto_dynamic = dynamic_outputs["lib/libmosquitto.so.1"]
    if "Shared library: [libssl.so.3]" not in mosquitto_dynamic or \
       "Shared library: [libcrypto.so.3]" not in mosquitto_dynamic:
        raise UpdateError("mosquitto_tls_missing", "libmosquitto 缺少 OpenSSL TLS 动态依赖")


def validate_package_root(package_root: Path) -> dict[str, Any]:
    root = package_root.resolve(strict=True)
    for relative in REQUIRED_PATHS:
        path = root / relative
        if not path.exists() and not path.is_symlink():
            raise UpdateError("missing_required_file", f"升级包缺少必需文件：{relative}")
    for relative in (
        "bin/edge-controller", "bin/edge-web", "scripts/install.sh",
        "scripts/verify-install.sh", "scripts/rotate-logs.sh", "scripts/update-manager.py",
    ):
        if not os.access(root / relative, os.X_OK):
            raise UpdateError("invalid_file_mode", f"升级包必需程序不可执行：{relative}")
    info = load_package_info(root)
    version = read_version(root)
    if not version or version != info["version"]:
        raise UpdateError("version_mismatch", "VERSION 与 package-info.json version 不一致")
    verify_manifest(root)
    signature = verify_package_signature(root, info)
    checked_elfs: set[Path] = set()
    for directory_name in ("bin", "lib"):
        for candidate in (root / directory_name).rglob("*"):
            if not candidate.is_file():
                continue
            resolved = candidate.resolve(strict=True)
            if resolved in checked_elfs:
                continue
            checked_elfs.add(resolved)
            validate_aarch64_header(resolved, candidate.relative_to(root).as_posix())
    validate_elf(root / "bin/edge-controller", "edge-controller")
    validate_elf(root / "bin/edge-web", "edge-web", static_binary=True)
    validate_sonames(root)
    default_env = (root / "config/edge-controller.env.default").read_text(encoding="utf-8")
    if "EDGE_CONTROLLER_REQUIRE_EXPLICIT_NETWORK_CONFIG=1" not in default_env.splitlines():
        raise UpdateError("network_guard_missing", "升级包默认环境未启用首次网络保护")
    return {
        "product": info["product"],
        "platform": info["platform"],
        "arch": info["arch"],
        "version": version,
        "build_time": info["build_time"],
        "package_format_version": info["package_format_version"],
        "signature": signature,
    }


def directory_size(path: Path) -> int:
    total = 0
    if not path.exists():
        return 0
    for entry in path.rglob("*"):
        with contextlib.suppress(OSError):
            if entry.is_file() and not entry.is_symlink():
                total += entry.stat().st_size
    return total


def validate_current_install(root: Path) -> None:
    if root.is_symlink() or not root.is_dir():
        raise UpdateError("invalid_install_root", f"当前产品根目录不可用：{root}")
    required = [*PROGRAM_DIRS, "config", "data", "log", "VERSION", "package-info.json", "MANIFEST.sha256"]
    for relative in required:
        if not (root / relative).exists():
            raise UpdateError("invalid_current_install", f"当前安装缺少：{relative}")
    for relative in (*PROGRAM_DIRS, "config", "data", "log"):
        path = root / relative
        if path.is_symlink() or not path.is_dir():
            raise UpdateError("unsafe_current_install", f"当前安装目录不能是链接：{relative}")
    environment_path = root / "config/edge-controller.env"
    if environment_path.is_symlink() or not environment_path.is_file():
        raise UpdateError("missing_live_config", "当前现场配置 config/edge-controller.env 不存在")


def current_install_fingerprint(root: Path) -> dict[str, str]:
    version_path = root / "VERSION"
    manifest_path = root / "MANIFEST.sha256"
    for path, description in (
        (version_path, "VERSION"), (manifest_path, "MANIFEST.sha256"),
    ):
        try:
            metadata = path.lstat()
        except FileNotFoundError as error:
            raise UpdateError("current_install_changed", f"当前安装缺少 {description}") from error
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            raise UpdateError("current_install_changed", f"当前安装的 {description} 不是安全的常规文件")
    version = read_version(root)
    if not version:
        raise UpdateError("current_install_changed", "当前安装 VERSION 为空")
    return {
        "current_version": version,
        "manifest_sha256": sha256_file(manifest_path),
    }


def ensure_current_install_unchanged(root: Path, expected: Any) -> None:
    if not isinstance(expected, dict):
        raise UpdateError("current_install_changed", "READY 任务缺少当前安装指纹，请重新上传并校验升级包")
    expected_version = expected.get("current_version")
    expected_manifest = expected.get("manifest_sha256")
    if not isinstance(expected_version, str) or not isinstance(expected_manifest, str):
        raise UpdateError("current_install_changed", "READY 任务的当前安装指纹无效，请重新上传并校验升级包")
    current = current_install_fingerprint(root)
    if current["current_version"] != expected_version or current["manifest_sha256"] != expected_manifest:
        raise UpdateError("current_install_changed", "当前安装在 READY 后已发生变化，请重新上传并校验升级包")


def validate_disk_space(root: Path, archive: Path, unpacked_size: int) -> dict[str, int]:
    managed_size = sum(directory_size(root / item) for item in PROGRAM_DIRS)
    managed_size += sum((root / item).stat().st_size for item in PROGRAM_FILES if (root / item).is_file())
    data_size = directory_size(root / "data")
    package_size = archive.stat().st_size
    working_bytes = package_size + unpacked_size + managed_size + data_size
    safety = max(SAFETY_MARGIN_BYTES, working_bytes // 5)
    required = working_bytes + safety
    free = shutil.disk_usage(root).free
    if free < required:
        raise UpdateError("insufficient_disk_space", f"升级空间不足：需要至少 {required} bytes，当前可用 {free} bytes")
    return {"required_bytes": required, "available_bytes": free, "safety_margin_bytes": safety}


def incoming_package(root: Path, package_identifier: str) -> Path:
    if Path(package_identifier).name != package_identifier or PACKAGE_NAME_RE.fullmatch(package_identifier) is None:
        raise UpdateError("invalid_package_identifier", "package 只能是 incoming 中的安全升级包文件名")
    incoming = workspace_paths(root)["incoming"].resolve()
    package = incoming / package_identifier
    try:
        metadata = package.lstat()
    except FileNotFoundError as error:
        raise UpdateError("package_not_found", f"待升级包不存在：{package_identifier}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise UpdateError("invalid_package_file", "待升级包必须是 incoming 中的常规文件，不能是链接")
    if not os.access(package, os.R_OK):
        raise UpdateError("package_unreadable", f"待升级包不可读：{package_identifier}")
    return package


def new_job_id() -> str:
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return f"{timestamp}-{uuid.uuid4().hex[:8]}"


def active_or_requested(status: dict[str, Any]) -> bool:
    return status.get("state") in ACTIVE_STATES or bool(status.get("runner_requested"))


def validate_package_job(root: Path, package_identifier: str) -> dict[str, Any]:
    if getattr(os, "geteuid", lambda: -1)() != 0:
        raise UpdateError("root_required", "升级包校验和任务创建必须由 root 执行")
    with update_lock(root):
        previous = read_status(root)
        if active_or_requested(previous):
            raise UpdateError("update_busy", f"升级任务 {previous.get('job_id', '')} 正在执行")
        previous_job_for_cleanup = str(previous.get("job_id", ""))
        previous_backup_for_cleanup = str(previous.get("backup_id", ""))
        cleanup_history(
            root,
            previous_job_for_cleanup if JOB_ID_RE.fullmatch(previous_job_for_cleanup) else "",
            package_identifier,
            previous_backup_for_cleanup if JOB_ID_RE.fullmatch(previous_backup_for_cleanup) else "",
        )
        cleanup_stale_uploads(root)
        previous_job_id = str(previous.get("job_id", ""))
        if previous.get("state") == "READY" and JOB_ID_RE.fullmatch(previous_job_id):
            shutil.rmtree(workspace_paths(root)["jobs"] / previous_job_id, ignore_errors=True)
        package = incoming_package(root, package_identifier)
        validate_current_install(root)
        install_fingerprint = current_install_fingerprint(root)
        members, unpacked_size = inspect_archive(package)
        del members
        job_id = new_job_id()
        paths = workspace_paths(root)
        job_dir = paths["jobs"] / job_id
        status = default_status(root)
        status.update({
            "job_id": job_id,
            "state": "VALIDATING",
            "current_version": install_fingerprint["current_version"],
            "package_path": package.name,
            "created_at": utc_now(),
            "progress": {"percent": 5, "stage": "validating"},
            "message": "正在校验升级包",
        })
        write_status(root, status)
        try:
            space = validate_disk_space(root, package, unpacked_size)
            job_dir.mkdir(mode=0o700)
            extracted_root = extract_archive(package, job_dir / "extracted")
            validation = validate_package_root(extracted_root)
            filename_version = PACKAGE_NAME_RE.fullmatch(package.name).group(1)  # type: ignore[union-attr]
            if filename_version != validation["version"]:
                raise UpdateError("filename_version_mismatch", "升级包文件名版本与 package-info.json 不一致")
            ensure_current_install_unchanged(root, install_fingerprint)
            runner = job_dir / "runner.py"
            shutil.copy2(Path(__file__).resolve(), runner)
            os.chmod(runner, 0o700)
            descriptor = {
                "job_id": job_id,
                "package_file": package.name,
                "package_sha256": sha256_file(package),
                "current_version": status["current_version"],
                "current_install_fingerprint": install_fingerprint,
                "target_version": validation["version"],
                "created_at": status["created_at"],
                "validation": validation,
                "disk": space,
            }
            atomic_write_json(job_dir / "job.json", descriptor, 0o600)
            status.update({
                "state": "READY",
                "target_version": validation["version"],
                "progress": {"percent": 15, "stage": "ready"},
                "message": "升级包校验通过，等待独立升级任务启动",
                "signature": validation["signature"],
                "package_info": {
                    "product": validation["product"],
                    "platform": validation["platform"],
                    "arch": validation["arch"],
                    "version": validation["version"],
                    "build_time": validation["build_time"],
                    "size_bytes": package.stat().st_size,
                },
            })
            write_status(root, status)
            cleanup_history(root, job_id, package.name)
            return status
        except Exception as error:
            failure = error if isinstance(error, UpdateError) else UpdateError("validation_failed", str(error))
            status.update({
                "state": "FAILED",
                "finished_at": utc_now(),
                "progress": {"percent": 0, "stage": "validation_failed"},
                "message": "升级包校验失败，未停止或修改业务服务",
                "error_code": failure.code,
                "error_message": failure.message,
            })
            write_status(root, status)
            shutil.rmtree(job_dir, ignore_errors=True)
            cleanup_history(root, "", package.name)
            raise failure


def validate_upload_id(upload_id: str) -> None:
    if UPLOAD_ID_RE.fullmatch(upload_id) is None:
        raise UpdateError("invalid_upload_id", "upload_id 格式非法")


def validate_uploaded_file_metadata(metadata: os.stat_result) -> None:
    if pwd is None or grp is None:
        raise UpdateError("unsupported_platform", "上传包接管仅支持 Linux")
    try:
        expected_uid = pwd.getpwnam("edge-web").pw_uid
        expected_gid = grp.getgrnam("edge-controller").gr_gid
    except KeyError as error:
        raise UpdateError("upload_identity_missing", "升级上传专用用户或组不存在") from error
    if not stat.S_ISREG(metadata.st_mode):
        raise UpdateError("invalid_upload_file", "上传暂存项必须是常规文件")
    if metadata.st_uid != expected_uid or metadata.st_gid != expected_gid:
        raise UpdateError("invalid_upload_owner", "上传暂存项的 owner/group 不符合安全策略")
    if stat.S_IMODE(metadata.st_mode) != 0o600:
        raise UpdateError("invalid_upload_mode", "上传暂存项权限必须为 0600")
    if metadata.st_size <= 0:
        raise UpdateError("empty_upload", "升级包不能为空")
    if metadata.st_size > MAX_UPLOAD_BYTES:
        raise UpdateError("upload_too_large", "升级包不能超过 256 MiB")


def import_uploaded_package(root: Path, upload_id: str, package_identifier: str) -> dict[str, Any]:
    if getattr(os, "geteuid", lambda: -1)() != 0:
        raise UpdateError("root_required", "上传包接管必须由 root 执行")
    validate_upload_id(upload_id)
    if PACKAGE_NAME_RE.fullmatch(package_identifier) is None:
        raise UpdateError("invalid_package_identifier", "升级包名称不符合 RK3562 正式发布包规则")

    with update_lock(root):
        status = read_status(root)
        if active_or_requested(status):
            raise UpdateError("update_busy", "已有升级任务正在执行")
        paths = workspace_paths(root)
        upload_root = paths["upload"].resolve(strict=True)
        source = paths["upload"] / f"{upload_id}.upload"
        if source.parent.resolve(strict=True) != upload_root:
            raise UpdateError("unsafe_upload_path", "上传暂存路径越界")
        try:
            source_metadata = source.lstat()
        except FileNotFoundError as error:
            raise UpdateError("upload_not_found", "待接管的上传包不存在或已过期") from error
        if stat.S_ISLNK(source_metadata.st_mode):
            raise UpdateError("invalid_upload_file", "上传暂存项不能是符号链接")
        validate_uploaded_file_metadata(source_metadata)

        source_flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
        source_fd = os.open(source, source_flags)
        temporary = paths["incoming"] / f".import-{upload_id}"
        destination = paths["incoming"] / package_identifier
        destination_fd = -1
        try:
            opened_metadata = os.fstat(source_fd)
            validate_uploaded_file_metadata(opened_metadata)
            if (opened_metadata.st_dev, opened_metadata.st_ino) != (source_metadata.st_dev, source_metadata.st_ino):
                raise UpdateError("upload_changed", "上传暂存项在接管时发生变化")
            if destination.exists() or destination.is_symlink():
                destination_metadata = destination.lstat()
                if stat.S_ISLNK(destination_metadata.st_mode) or not stat.S_ISREG(destination_metadata.st_mode):
                    raise UpdateError("invalid_incoming_target", "incoming 中的同名目标不安全")
            destination_fd = os.open(
                temporary,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
                0o640,
            )
            while True:
                block = os.read(source_fd, 1024 * 1024)
                if not block:
                    break
                offset = 0
                while offset < len(block):
                    written = os.write(destination_fd, block[offset:])
                    if written <= 0:
                        raise UpdateError("upload_copy_failed", "接管上传包时写入中断")
                    offset += written
            os.fsync(destination_fd)
            os.fchmod(destination_fd, 0o640)
            if grp is not None:
                os.fchown(destination_fd, 0, grp.getgrnam("edge-controller").gr_gid)
            current_metadata = source.lstat()
            if (current_metadata.st_dev, current_metadata.st_ino) != (opened_metadata.st_dev, opened_metadata.st_ino):
                raise UpdateError("upload_changed", "上传暂存项在接管时发生变化")
            os.close(destination_fd)
            destination_fd = -1
            os.replace(temporary, destination)
            directory_fd = os.open(paths["incoming"], os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
            source.unlink()
        finally:
            os.close(source_fd)
            if destination_fd >= 0:
                os.close(destination_fd)
            with contextlib.suppress(FileNotFoundError):
                temporary.unlink()

    return validate_package_job(root, package_identifier)


def load_job(root: Path, job_id: str) -> tuple[Path, dict[str, Any]]:
    if JOB_ID_RE.fullmatch(job_id) is None:
        raise UpdateError("invalid_job_id", "job_id 格式非法")
    job_dir = workspace_paths(root)["jobs"] / job_id
    try:
        descriptor = json.loads((job_dir / "job.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise UpdateError("job_not_found", f"升级任务不存在或描述损坏：{job_id}") from error
    if not isinstance(descriptor, dict) or descriptor.get("job_id") != job_id:
        raise UpdateError("invalid_job", "升级任务描述与 job_id 不一致")
    return job_dir, descriptor


def fail_current_install_changed(root: Path, status: dict[str, Any], error: UpdateError) -> None:
    status.update({
        "state": "FAILED",
        "current_version": read_version(root),
        "finished_at": utc_now(),
        "runner_requested": False,
        "runner_pid": 0,
        "progress": {"percent": 0, "stage": "current_install_changed"},
        "error_code": error.code,
        "error_message": error.message,
        "message": "当前安装在升级包校验后发生变化，未停止业务服务；请重新上传并校验升级包",
        "recovery_required": False,
        "recovery_pending_health": False,
    })
    write_status(root, status)


def request_start(root: Path, job_id: str) -> dict[str, Any]:
    if getattr(os, "geteuid", lambda: -1)() != 0:
        raise UpdateError("root_required", "启动升级任务必须由 root 执行")
    with update_lock(root):
        status = read_status(root)
        if status.get("state") != "READY" or status.get("job_id") != job_id:
            raise UpdateError("job_not_ready", "指定升级任务不是当前 READY 任务")
        if status.get("runner_requested"):
            raise UpdateError("update_busy", "独立升级任务已经启动")
        job_dir, descriptor = load_job(root, job_id)
        package = incoming_package(root, str(descriptor.get("package_file", "")))
        if sha256_file(package) != descriptor.get("package_sha256"):
            raise UpdateError("package_changed", "staging 升级包在校验后发生变化")
        try:
            ensure_current_install_unchanged(root, descriptor.get("current_install_fingerprint"))
        except UpdateError as error:
            fail_current_install_changed(root, status, error)
            raise
        if not (job_dir / "runner.py").is_file():
            raise UpdateError("runner_missing", "job 目录缺少独立 runner 副本")
        unit = Path("/etc/systemd/system/edge-upgrade@.service")
        if not unit.is_file() or unit.is_symlink():
            raise UpdateError("upgrade_unit_missing", "独立升级 systemd unit 未正确安装")
        for recovery_unit_name in (
            "edge-upgrade-recovery.service", "edge-upgrade-recovery-verify.service",
        ):
            recovery_unit = Path("/etc/systemd/system") / recovery_unit_name
            if not recovery_unit.is_file() or recovery_unit.is_symlink():
                raise UpdateError("upgrade_recovery_unit_missing", f"升级恢复 unit 未正确安装：{recovery_unit_name}")
        install_recovery_manager(root)
        status.update({
            "started_at": utc_now(),
            "runner_requested": True,
            "runner_unit": f"edge-upgrade@{job_id}.service",
            "last_heartbeat": utc_now(),
            "message": "已请求 systemd 独立 root 任务接管升级",
            "progress": {"percent": 16, "stage": "runner_requested"},
        })
        write_status(root, status)
        try:
            run_checked(["systemctl", "start", "--no-block", f"edge-upgrade@{job_id}.service"], timeout=15)
        except UpdateError as error:
            status.update({
                "state": "FAILED",
                "finished_at": utc_now(),
                "runner_requested": False,
                "error_code": error.code,
                "error_message": error.message,
                "message": "无法启动独立升级任务，业务服务未停止",
            })
            write_status(root, status)
            raise
        return status


def copy_entry(source: Path, destination: Path) -> None:
    if source.is_dir() and not source.is_symlink():
        shutil.copytree(source, destination, symlinks=True)
    elif source.exists() or source.is_symlink():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination, follow_symlinks=False)


def fsync_tree(path: Path) -> None:
    if not path.is_dir() or path.is_symlink():
        raise UpdateError("backup_path_invalid", f"需要持久化的备份目录无效：{path}")
    directories = [path]
    for entry in path.rglob("*"):
        if entry.is_symlink():
            continue
        if entry.is_dir():
            directories.append(entry)
        elif entry.is_file():
            descriptor = os.open(entry, os.O_RDONLY)
            try:
                try:
                    os.fsync(descriptor)
                except OSError:
                    if os.name != "nt":
                        raise
            finally:
                os.close(descriptor)
    for directory in reversed(directories):
        try:
            descriptor = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        except OSError:
            if os.name == "nt":
                continue
            raise
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)


def fsync_regular_file(path: Path) -> None:
    try:
        metadata = path.lstat()
    except FileNotFoundError as error:
        raise UpdateError("installed_state_durability_failed", f"持久化提交缺少文件：{path}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise UpdateError("installed_state_durability_failed", f"持久化提交目标不是安全常规文件：{path}")
    descriptor = os.open(path, os.O_RDONLY)
    try:
        try:
            os.fsync(descriptor)
        except OSError:
            if os.name != "nt":
                raise
    finally:
        os.close(descriptor)


def fsync_directory(path: Path) -> None:
    if path.is_symlink() or not path.is_dir():
        raise UpdateError("installed_state_durability_failed", f"持久化提交缺少安全目录：{path}")
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    except OSError:
        if os.name == "nt":
            return
        raise
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def ensure_installed_state_durable(
    root: Path,
    include_persistent: bool = False,
    systemd_root: Path = Path("/etc/systemd/system"),
) -> None:
    try:
        for directory_name in PROGRAM_DIRS:
            fsync_tree(root / directory_name)
        required_files = [
            *(root / name for name in PROGRAM_FILES),
            root / "config/edge-controller.env.default",
            root / "update/recovery" / RECOVERY_MANAGER_FILE,
        ]
        for path in required_files:
            fsync_regular_file(path)
        for optional_name in PROGRAM_OPTIONAL_FILES:
            optional_path = root / optional_name
            if optional_path.exists() or optional_path.is_symlink():
                fsync_regular_file(optional_path)
        for unit_name in STABLE_SYSTEMD_UNITS:
            fsync_regular_file(systemd_root / unit_name)
        gate_directories: list[Path] = []
        for service_name in SERVICES:
            gate_directory = systemd_root / f"{service_name}.d"
            fsync_regular_file(gate_directory / SERVICE_GATE_DROPIN_NAME)
            gate_directories.append(gate_directory)
        if include_persistent:
            fsync_regular_file(root / "config/edge-controller.env")
            fsync_tree(root / "data")
        for directory in (
            root,
            root / "config",
            root / "update",
            root / "update/recovery",
            systemd_root,
            *gate_directories,
        ):
            fsync_directory(directory)
        wants_directory = systemd_root / "multi-user.target.wants"
        if wants_directory.exists() or wants_directory.is_symlink():
            fsync_directory(wants_directory)
    except UpdateError as error:
        if error.code == "installed_state_durability_failed":
            raise
        raise UpdateError("installed_state_durability_failed", f"安装状态持久化提交失败：{error.message}") from error
    except OSError as error:
        raise UpdateError("installed_state_durability_failed", f"安装状态持久化提交失败：{error}") from error


def install_recovery_manager(root: Path, source: Optional[Path] = None) -> Path:
    paths = ensure_workspace(root)
    source_path = source or Path(__file__).resolve()
    if source_path.is_symlink() or not source_path.is_file():
        raise UpdateError("recovery_manager_invalid", "恢复 manager 来源必须是常规文件且不能是符号链接")
    target = paths["recovery"] / RECOVERY_MANAGER_FILE
    descriptor, temporary_name = tempfile.mkstemp(prefix=".update-manager.", dir=paths["recovery"])
    temporary = Path(temporary_name)
    try:
        with source_path.open("rb") as input_stream, os.fdopen(descriptor, "wb") as output_stream:
            shutil.copyfileobj(input_stream, output_stream, length=1024 * 1024)
            output_stream.flush()
            os.fsync(output_stream.fileno())
        os.chmod(temporary, 0o700)
        if getattr(os, "geteuid", lambda: -1)() == 0:
            os.chown(temporary, 0, 0)
        os.replace(temporary, target)
        directory_fd = os.open(paths["recovery"], os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        with contextlib.suppress(FileNotFoundError):
            temporary.unlink()
    return target


def create_program_backup(root: Path, backup_dir: Path) -> Path:
    program = backup_dir / "program"
    program.mkdir(mode=0o700, parents=True)
    for item in PROGRAM_DIRS:
        copy_entry(root / item, program / item)
    for item in PROGRAM_FILES:
        copy_entry(root / item, program / item)
    for item in PROGRAM_OPTIONAL_FILES:
        copy_entry(root / item, program / item)
    (program / "config").mkdir(mode=0o700)
    copy_entry(root / "config/edge-controller.env.default", program / "config/edge-controller.env.default")
    shutil.copy2(program / "config/edge-controller.env.default", program / "config/edge-controller.env")
    shutil.copy2(root / "config/edge-controller.env", backup_dir / "edge-controller.env")
    return program


def backup_checksums(backup_dir: Path) -> dict[str, str]:
    checksums: dict[str, str] = {}
    program = backup_dir / "program"
    for directory_name in ("bin", "lib", "systemd", "scripts", "config"):
        directory = program / directory_name
        if directory.is_symlink() or not directory.is_dir():
            raise UpdateError("backup_incomplete", f"回滚备份缺少程序目录：{directory_name}")
    for required_relative in ROLLBACK_REQUIRED_PATHS:
        candidate = program / required_relative
        if not candidate.exists() and not candidate.is_symlink():
            raise UpdateError("backup_incomplete", f"回滚备份缺少旧版程序资产：{required_relative}")
    for candidate in sorted(program.rglob("*")):
        if candidate.is_file() and not candidate.is_symlink():
            relative = candidate.relative_to(backup_dir).as_posix()
            checksums[relative] = sha256_file(candidate)
    for relative in ("program/VERSION", "program/package-info.json", "program/MANIFEST.sha256", "edge-controller.env"):
        candidate = backup_dir / relative
        if candidate.is_symlink() or not candidate.is_file():
            raise UpdateError("backup_incomplete", f"回滚备份缺少常规文件：{relative}")
        checksums[relative] = sha256_file(candidate)
    data = backup_dir / "data"
    if data.is_symlink() or not data.is_dir():
        raise UpdateError("backup_incomplete", "回滚备份缺少一致性 data 快照")
    for candidate in sorted(data.rglob("*")):
        if candidate.is_symlink():
            raise UpdateError("backup_incomplete", f"data 快照不能包含符号链接：{candidate.relative_to(data)}")
        if candidate.is_file():
            relative = candidate.relative_to(backup_dir).as_posix()
            checksums[relative] = sha256_file(candidate)
    for database in ("edge-config.db", "edge-history.db", "edge-events.db"):
        if f"data/{database}" not in checksums:
            raise UpdateError("backup_incomplete", f"一致性 data 快照缺少数据库：{database}")
    return checksums


def commit_backup(root: Path, backup_dir: Path, job_id: str, previous_version: str) -> dict[str, Any]:
    if backup_dir.name != job_id or JOB_ID_RE.fullmatch(job_id) is None:
        raise UpdateError("backup_id_invalid", "backup 目录与 job_id 不一致")
    checksums = backup_checksums(backup_dir)
    fsync_tree(backup_dir / "program")
    fsync_tree(backup_dir / "data")
    environment_fd = os.open(backup_dir / "edge-controller.env", os.O_RDONLY)
    try:
        try:
            os.fsync(environment_fd)
        except OSError:
            if os.name != "nt":
                raise
    finally:
        os.close(environment_fd)
    metadata = {
        "format_version": 1,
        "backup_id": job_id,
        "job_id": job_id,
        "previous_version": previous_version,
        "complete": True,
        "completed_at": utc_now(),
        "checksums": checksums,
    }
    atomic_write_json(backup_dir / BACKUP_METADATA_FILE, metadata, 0o600)
    try:
        parent_fd = os.open(backup_dir.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    except OSError:
        parent_fd = -1
    if parent_fd >= 0:
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
    return metadata


def load_complete_backup(backup_dir: Path, expected_backup_id: str = "") -> dict[str, Any]:
    marker = backup_dir / BACKUP_METADATA_FILE
    if marker.is_symlink() or not marker.is_file():
        raise UpdateError("rollback_backup_incomplete", "回滚 backup 尚未到达完整提交点")
    try:
        metadata = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise UpdateError("rollback_backup_incomplete", "回滚 backup 完整标识不可读") from error
    if not isinstance(metadata, dict) or metadata.get("format_version") != 1 or metadata.get("complete") is not True:
        raise UpdateError("rollback_backup_incomplete", "回滚 backup 完整标识无效")
    backup_id = str(metadata.get("backup_id", ""))
    if JOB_ID_RE.fullmatch(backup_id) is None or backup_id != backup_dir.name or (expected_backup_id and backup_id != expected_backup_id):
        raise UpdateError("rollback_backup_incomplete", "回滚 backup 标识与目录或事务不一致")
    checksums = metadata.get("checksums")
    if not isinstance(checksums, dict) or not checksums:
        raise UpdateError("rollback_backup_incomplete", "回滚 backup 缺少完整性摘要")
    for relative, expected_digest in checksums.items():
        if not isinstance(relative, str) or not isinstance(expected_digest, str):
            raise UpdateError("rollback_backup_incomplete", "回滚 backup 完整性摘要格式无效")
        candidate = backup_dir / relative
        try:
            candidate.resolve(strict=True).relative_to(backup_dir.resolve(strict=True))
        except (OSError, ValueError) as error:
            raise UpdateError("rollback_backup_incomplete", f"回滚 backup 文件越界：{relative}") from error
        if candidate.is_symlink() or not candidate.is_file() or sha256_file(candidate) != expected_digest:
            raise UpdateError("rollback_backup_corrupt", f"回滚 backup 完整性校验失败：{relative}")
    return metadata


def stop_services() -> None:
    for service in STOP_ORDER:
        subprocess.run(["systemctl", "stop", service], text=True, capture_output=True, timeout=45)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        active = [service for service in SERVICES if subprocess.run(
            ["systemctl", "is-active", "--quiet", service], timeout=5
        ).returncode == 0]
        if not active:
            return
        time.sleep(1)
    raise UpdateError("service_stop_timeout", "业务服务未在 30 秒内完全停止")


def stop_active_services() -> None:
    for service in STOP_ORDER:
        active = subprocess.run(
            ["systemctl", "is-active", "--quiet", service], timeout=5,
        ).returncode == 0
        if active:
            run_checked(["systemctl", "stop", service], timeout=45)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        active_services = [service for service in SERVICES if subprocess.run(
            ["systemctl", "is-active", "--quiet", service], timeout=5,
        ).returncode == 0]
        if not active_services:
            return
        time.sleep(1)
    raise UpdateError("service_stop_timeout", "恢复前仍有业务服务未停止")


def start_services() -> None:
    run_checked(["systemctl", "daemon-reload"], timeout=30)
    for service in SERVICES:
        run_checked(["systemctl", "start", service], timeout=30)


def recover_services_best_effort() -> str:
    errors: list[str] = []
    try:
        run_checked(["systemctl", "daemon-reload"], timeout=30)
    except Exception as error:
        errors.append(f"daemon-reload: {error}")
    for service in SERVICES:
        try:
            run_checked(["systemctl", "start", service], timeout=30)
        except Exception as error:
            errors.append(f"{service}: {error}")
    return "; ".join(errors)


def http_responds() -> bool:
    connection = http.client.HTTPConnection("127.0.0.1", 8080, timeout=3)
    try:
        connection.request("GET", "/")
        response = connection.getresponse()
        response.read(1024)
        return 200 <= response.status < 400
    except OSError:
        return False
    finally:
        connection.close()


def services_healthy(root: Path, expected_version: str, timeout: int = 60, full_verify: bool = True) -> None:
    deadline = time.monotonic() + timeout
    last_missing = ""
    while time.monotonic() < deadline:
        active = all(subprocess.run(["systemctl", "is-active", "--quiet", service], timeout=5).returncode == 0 for service in SERVICES)
        socket_ok = (root / "run/edge-controller.sock").is_socket()
        databases_ok = all((root / "data" / name).is_file() for name in ("edge-config.db", "edge-history.db", "edge-events.db"))
        config_ok = (root / "config/edge-controller.env").is_file()
        if active and socket_ok and databases_ok and config_ok and read_version(root) == expected_version and http_responds():
            if full_verify:
                run_checked([str(root / "scripts/verify-install.sh")], timeout=120)
            return
        last_missing = f"active={active}, socket={socket_ok}, db={databases_ok}, config={config_ok}, version={read_version(root)!r}"
        time.sleep(2)
    raise UpdateError("health_check_failed", f"升级后健康检查超时：{last_missing}")


def restore_backup(root: Path, backup_dir: Path, old_version: str, start_after_restore: bool = True) -> None:
    metadata = load_complete_backup(backup_dir, backup_dir.name)
    if str(metadata.get("previous_version", "")) != old_version:
        raise UpdateError("rollback_backup_invalid", "回滚 backup 的 previous_version 与事务不一致")
    program = backup_dir / "program"
    install_script = program / "scripts/install.sh"
    if not install_script.is_file():
        raise UpdateError("rollback_backup_invalid", "回滚快照缺少旧版 install.sh")
    run_checked(
        ["bash", str(install_script), "--no-start"], timeout=180,
        environment_overrides={
            "EDGE_UPGRADE_RECOVERY_MODE": "1",
            # root 专用的已提交备份可能早于强制验签策略，其完整性以 backup.json 为信任锚。
            "EDGE_UPGRADE_SIGNATURE_POLICY": "optional",
        },
    )
    environment_backup = backup_dir / "edge-controller.env"
    data_backup = backup_dir / "data"
    if not environment_backup.is_file() or not data_backup.is_dir():
        raise UpdateError("rollback_backup_invalid", "回滚快照缺少现场配置或一致性 data 快照")
    shutil.copy2(environment_backup, root / "config/edge-controller.env")
    os.chmod(root / "config/edge-controller.env", 0o640)
    if getattr(os, "geteuid", lambda: -1)() == 0 and grp is not None:
        with contextlib.suppress(KeyError):
            os.chown(root / "config/edge-controller.env", 0, grp.getgrnam("edge-controller").gr_gid)
    current_data = root / "data"
    if current_data.exists() or current_data.is_symlink():
        if current_data.is_symlink():
            current_data.unlink()
        else:
            shutil.rmtree(current_data)
    shutil.copytree(data_backup, current_data, symlinks=True)
    os.chmod(current_data, 0o750)
    if getattr(os, "geteuid", lambda: -1)() == 0:
        os.chown(current_data, 0, 0)
    for stale_runtime in (
        root / "run/edge-controller.sock", root / "run/edge-controller.sock.lock",
    ):
        with contextlib.suppress(FileNotFoundError):
            stale_runtime.unlink()
    run_checked(["systemctl", "daemon-reload"], timeout=30)
    ensure_installed_state_durable(root, include_persistent=True)
    if start_after_restore:
        start_services()
        services_healthy(root, old_version, timeout=60, full_verify=False)


def cleanup_history(
    root: Path,
    current_job_id: str,
    current_package: str = "",
    current_backup_id: str = "",
) -> None:
    paths = workspace_paths(root)
    policies = (
        (paths["jobs"], KEEP_JOBS, {current_job_id} if current_job_id else set()),
        (paths["backup"], KEEP_BACKUPS, {value for value in (current_job_id, current_backup_id) if value}),
    )
    for directory, keep, protected_names in policies:
        protected_existing = {
            name for name in protected_names
            if JOB_ID_RE.fullmatch(name) is not None and (directory / name).is_dir()
        }
        entries = sorted(
            (entry for entry in directory.iterdir() if entry.is_dir() and entry.name not in protected_existing),
            key=lambda entry: entry.stat().st_mtime,
            reverse=True,
        )
        retain_others = max(0, keep - len(protected_existing))
        for entry in entries[retain_others:]:
            shutil.rmtree(entry, ignore_errors=True)
    for entry in paths["incoming"].iterdir():
        if re.fullmatch(r"\.import-[0-9a-f]{32}", entry.name):
            with contextlib.suppress(OSError):
                if entry.is_symlink() or entry.is_file():
                    entry.unlink()
    incoming = sorted(
        (entry for entry in paths["incoming"].iterdir()
         if entry.is_file() and not entry.is_symlink() and entry.name != current_package),
        key=lambda entry: entry.stat().st_mtime,
        reverse=True,
    )
    retain_incoming = max(0, KEEP_INCOMING - (1 if current_package else 0))
    for entry in incoming[retain_incoming:]:
        with contextlib.suppress(OSError):
            entry.unlink()


def cleanup_stale_uploads(root: Path) -> None:
    upload = workspace_paths(root)["upload"]
    now = time.time()
    candidates: list[tuple[Path, os.stat_result]] = []
    for entry in upload.iterdir():
        if re.fullmatch(r"[0-9a-f]{64}\.watch", entry.name):
            try:
                metadata = entry.lstat()
            except FileNotFoundError:
                continue
            if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode) or now - metadata.st_mtime > 60 * 60:
                with contextlib.suppress(OSError):
                    entry.unlink()
            continue
        if not re.fullmatch(r"[0-9a-f]{32}\.(?:part|upload)", entry.name):
            continue
        try:
            metadata = entry.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            with contextlib.suppress(OSError):
                entry.unlink()
            continue
        if now - metadata.st_mtime > UPLOAD_TTL_SECONDS:
            with contextlib.suppress(OSError):
                entry.unlink()
            continue
        candidates.append((entry, metadata))
    candidates.sort(key=lambda item: item[1].st_mtime, reverse=True)
    retained_bytes = 0
    for index, (entry, metadata) in enumerate(candidates):
        retained_bytes += metadata.st_size
        if index >= KEEP_UPLOADS or retained_bytes > MAX_STAGED_UPLOAD_BYTES:
            with contextlib.suppress(OSError):
                entry.unlink()


def cleanup_workspace(root: Path, status: Optional[dict[str, Any]] = None) -> None:
    current = status or read_status(root)
    current_job_id = str(current.get("job_id", ""))
    current_backup_id = str(current.get("backup_id", ""))
    current_package = str(current.get("package_path", ""))
    cleanup_history(
        root,
        current_job_id if JOB_ID_RE.fullmatch(current_job_id) else "",
        current_package,
        current_backup_id if JOB_ID_RE.fullmatch(current_backup_id) else "",
    )
    cleanup_stale_uploads(root)
    paths = workspace_paths(root)
    for entry in paths["update"].glob(".status.json.*"):
        with contextlib.suppress(OSError):
            if entry.is_symlink() or entry.is_file():
                entry.unlink()
    for entry in paths["recovery"].glob(".update-manager.*"):
        with contextlib.suppress(OSError):
            if entry.is_symlink() or entry.is_file():
                entry.unlink()


def service_state_snapshot() -> dict[str, str]:
    snapshot: dict[str, str] = {}
    for service in SERVICES:
        try:
            result = subprocess.run(
                ["systemctl", "is-active", service], text=True, capture_output=True, timeout=5,
            )
            snapshot[service] = (result.stdout or result.stderr or f"exit={result.returncode}").strip()[:120]
        except Exception as error:
            snapshot[service] = f"unknown: {error}"[:120]
    return snapshot


def service_start_allowed(root: Path) -> dict[str, Any]:
    status = read_status(root)
    state = str(status.get("state", "IDLE"))
    recovery_required = bool(status.get("recovery_required"))
    recovery_pending_health = bool(status.get("recovery_pending_health"))
    allowed = True
    reason = "upgrade_state_safe"
    if state == "FAILED" and recovery_required:
        allowed = False
        reason = "recovery_required"
    elif state == "ROLLING_BACK":
        allowed = recovery_pending_health
        reason = "recovery_health_check" if allowed else "rollback_not_committed"
    elif state in {"STOPPING", "INSTALLING"}:
        allowed = False
        reason = "upgrade_files_may_be_inconsistent"
    return {
        "allowed": allowed,
        "state": state,
        "recovery_required": recovery_required,
        "recovery_pending_health": recovery_pending_health,
        "reason": reason,
    }


def recover_transaction(root: Path, reason: str) -> dict[str, Any]:
    if getattr(os, "geteuid", lambda: -1)() != 0:
        raise UpdateError("root_required", "升级事务恢复必须由 root 执行")
    with update_lock(root, blocking=True):
        try:
            status = read_status(root)
        except UpdateError as error:
            if error.code != "status_corrupt":
                raise
            status = default_status(root)
            status.update({
                "state": "FAILED", "finished_at": utc_now(),
                "error_code": "status_corrupt", "error_message": error.message,
                "message": "升级状态文件损坏，无法自动判断未完成事务；将尝试启动当前可识别版本",
                "recovery_reason": reason, "recovery_required": False,
                "service_status": service_state_snapshot(),
            })
            write_status(root, status)
            return status
        state = str(status.get("state", "IDLE"))
        retry_failed_recovery = state == "FAILED" and bool(status.get("recovery_required"))
        if state in TERMINAL_STATES and not retry_failed_recovery:
            cleanup_workspace(root, status)
            return status
        if state == "READY":
            if status.get("runner_requested"):
                status.update({
                    "runner_requested": False,
                    "runner_pid": 0,
                    "runner_unit": "",
                    "last_heartbeat": "",
                    "message": "升级任务已校验；系统重启后已清除未生效的 runner 请求，可重新开始升级",
                    "recovery_reason": reason,
                })
                write_status(root, status)
            cleanup_workspace(root, status)
            return status
        if state in {"VALIDATING", "BACKING_UP"}:
            job_id = str(status.get("job_id", ""))
            if JOB_ID_RE.fullmatch(job_id):
                shutil.rmtree(workspace_paths(root)["jobs"] / job_id, ignore_errors=True)
            if state == "BACKING_UP":
                backup_id = str(status.get("backup_id") or job_id)
                if JOB_ID_RE.fullmatch(backup_id):
                    shutil.rmtree(workspace_paths(root)["backup"] / backup_id, ignore_errors=True)
            status.update({
                "state": "FAILED", "finished_at": utc_now(), "interrupted": True,
                "recovery_required": False, "recovery_pending_health": False,
                "recovery_reason": reason, "recovery_from_state": state,
                "recovery_finished_at": utc_now(), "runner_requested": False, "runner_pid": 0,
                "progress": {"percent": 0, "stage": "interrupted_before_install"},
                "error_code": "upgrade_interrupted",
                "error_message": "升级校验或备份阶段被系统中断，当前程序未进入安装",
                "message": "升级在修改当前程序前中断，任务已终止，可重新上传并校验",
                "backup_id": "" if state == "BACKING_UP" else status.get("backup_id", ""),
                "backup_complete": False,
            })
            write_status(root, status)
            cleanup_workspace(root, status)
            return status
        if state not in RECOVERY_ROLLBACK_STATES and not retry_failed_recovery:
            status.update({
                "state": "FAILED", "finished_at": utc_now(), "interrupted": True,
                "recovery_required": False, "runner_requested": False,
                "error_code": "recovery_state_invalid", "error_message": f"无法识别的恢复状态：{state}",
                "message": "升级事务状态异常，已停止自动恢复，需要人工检查",
            })
            write_status(root, status)
            return status

        attempts = int(status.get("recovery_attempt_count", 0) or 0)
        if attempts >= MAX_RECOVERY_ATTEMPTS:
            status.update({
                "state": "FAILED", "finished_at": utc_now(), "recovery_required": False,
                "recovery_pending_health": False, "runner_requested": False, "runner_pid": 0,
                "error_code": "recovery_attempts_exhausted",
                "error_message": "自动恢复已达到最大尝试次数，需要人工处理",
                "message": "升级恢复未完成，已停止重复自动恢复并尝试启动当前可用版本",
                "current_version": read_version(root),
                "service_status": service_state_snapshot(),
            })
            write_status(root, status)
            with contextlib.suppress(Exception):
                run_checked(["systemctl", "daemon-reload"], timeout=30)
            return status

        from_state = str(status.get("recovery_from_state") or state)
        backup_id = str(status.get("backup_id") or status.get("job_id") or "")
        previous_version = str(status.get("previous_version") or "")
        attempts += 1
        status.update({
            "state": "ROLLING_BACK", "interrupted": True, "recovery_required": True,
            "recovery_pending_health": False, "recovery_started_at": utc_now(),
            "recovery_finished_at": "", "recovery_reason": reason,
            "recovery_from_state": from_state, "recovery_attempt_count": attempts,
            "recovery_error": "", "backup_id": backup_id, "runner_requested": False,
            "runner_pid": 0, "runner_unit": "", "last_heartbeat": utc_now(),
            "rollback_performed": True,
            "progress": {"percent": 88, "stage": "recovering_interrupted_upgrade"},
            "message": "检测到升级过程异常中断，正在从完整 backup 恢复原版本",
        })
        write_status(root, status)
        try:
            if JOB_ID_RE.fullmatch(backup_id) is None:
                raise UpdateError("recovery_metadata_missing", "升级状态缺少可恢复的 backup_id")
            backup_dir = workspace_paths(root)["backup"] / backup_id
            backup_metadata = load_complete_backup(backup_dir, backup_id)
            committed_previous_version = str(backup_metadata.get("previous_version") or "")
            if not previous_version:
                previous_version = committed_previous_version
                status["previous_version"] = previous_version
                write_status(root, status)
            if not previous_version or previous_version != committed_previous_version:
                raise UpdateError("recovery_metadata_missing", "升级状态与完整 backup 的 previous_version 不一致")
            stop_active_services()
            restore_backup(root, backup_dir, previous_version, start_after_restore=False)
            status.update({
                "state": "ROLLING_BACK", "current_version": previous_version,
                "backup_complete": True, "recovery_pending_health": True,
                "progress": {"percent": 94, "stage": "recovery_waiting_for_health"},
                "message": "原版本和升级前数据已恢复，等待业务服务启动并完成健康确认",
            })
            write_status(root, status)
            return status
        except Exception as error:
            recovery_error = error.message if isinstance(error, UpdateError) else str(error)
            stopped_before_install_without_backup = (
                from_state == "STOPPING" and isinstance(error, UpdateError) and
                error.code in {"rollback_backup_incomplete", "recovery_metadata_missing"}
            )
            if stopped_before_install_without_backup:
                if JOB_ID_RE.fullmatch(backup_id):
                    shutil.rmtree(workspace_paths(root)["backup"] / backup_id, ignore_errors=True)
            status.update({
                "state": "FAILED", "finished_at": utc_now(),
                "recovery_required": attempts < MAX_RECOVERY_ATTEMPTS and not stopped_before_install_without_backup,
                "recovery_pending_health": False, "recovery_finished_at": utc_now(),
                "progress": {"percent": 0, "stage": "recovery_failed"},
                "error_code": "recovery_failed",
                "error_message": "升级在安装前中断且 backup 未完整提交" if stopped_before_install_without_backup else
                                 "升级中断后的自动恢复失败",
                "recovery_error": recovery_error, "rollback_error": recovery_error,
                "message": "升级在安装前中断，半完成 backup 已删除；当前程序保持不变并将重新启动服务"
                           if stopped_before_install_without_backup else
                           "升级失败，系统恢复未完成，需要维护人员检查" if attempts >= MAX_RECOVERY_ATTEMPTS else
                           "升级恢复本次未完成；后续启动仍会在次数上限内重试",
                "current_version": read_version(root),
                "service_status": service_state_snapshot(),
                "backup_id": "" if stopped_before_install_without_backup else backup_id,
                "backup_complete": False if stopped_before_install_without_backup else bool(status.get("backup_complete")),
            })
            write_status(root, status)
            return status


def finalize_recovery(root: Path) -> dict[str, Any]:
    if getattr(os, "geteuid", lambda: -1)() != 0:
        raise UpdateError("root_required", "升级恢复健康确认必须由 root 执行")
    with update_lock(root, blocking=True):
        status = read_status(root)
        if status.get("state") != "ROLLING_BACK" or not status.get("recovery_pending_health"):
            if status.get("state") == "FAILED" and (status.get("interrupted") or status.get("recovery_error")):
                status["current_version"] = read_version(root)
                status["service_status"] = service_state_snapshot()
                write_status(root, status)
            return status
        previous_version = str(status.get("previous_version") or status.get("current_version") or "")
        try:
            services_healthy(root, previous_version, timeout=90, full_verify=False)
            status.update({
                "state": "ROLLED_BACK", "current_version": previous_version,
                "finished_at": utc_now(), "recovery_required": False,
                "recovery_pending_health": False, "recovery_finished_at": utc_now(),
                "progress": {"percent": 100, "stage": "recovery_complete"},
                "message": "升级过程中系统意外中断，已自动恢复至原版本",
                "runner_requested": False, "runner_pid": 0, "recovery_error": "",
            })
            write_status(root, status)
            cleanup_workspace(root, status)
            return status
        except Exception as error:
            recovery_error = error.message if isinstance(error, UpdateError) else str(error)
            attempts = int(status.get("recovery_attempt_count", 0) or 0)
            status.update({
                "state": "FAILED", "finished_at": utc_now(),
                "recovery_required": attempts < MAX_RECOVERY_ATTEMPTS,
                "recovery_pending_health": False, "recovery_finished_at": utc_now(),
                "progress": {"percent": 0, "stage": "recovery_health_failed"},
                "error_code": "recovery_health_failed",
                "error_message": "原版本文件已恢复，但业务服务健康检查失败",
                "recovery_error": recovery_error, "rollback_error": recovery_error,
                "message": "升级失败，系统恢复后的服务健康检查未通过，需要维护人员检查",
                "service_status": service_state_snapshot(),
            })
            write_status(root, status)
            return status


def run_job(root: Path, job_id: str) -> dict[str, Any]:
    if getattr(os, "geteuid", lambda: -1)() != 0:
        raise UpdateError("root_required", "独立升级 runner 必须由 root 执行")
    with update_lock(root):
        status = read_status(root)
        if status.get("state") != "READY" or status.get("job_id") != job_id or not status.get("runner_requested"):
            raise UpdateError("job_not_ready", "独立 runner 对应的任务状态无效")
        job_dir, descriptor = load_job(root, job_id)
        extracted_root = job_dir / "extracted/edge-controller"
        target_version = str(descriptor["target_version"])
        old_version = str(descriptor["current_version"])
        backup_dir = workspace_paths(root)["backup"] / job_id
        install_started = False
        services_may_be_affected = False
        try:
            ensure_current_install_unchanged(root, descriptor.get("current_install_fingerprint"))
            package = incoming_package(root, str(descriptor.get("package_file", "")))
            if sha256_file(package) != descriptor.get("package_sha256"):
                raise UpdateError("package_changed", "staging 升级包在任务启动前发生变化")
            validation = validate_package_root(extracted_root)
            if validation["version"] != target_version:
                raise UpdateError("job_target_mismatch", "job 解压结果与目标版本不一致")
            transition(
                root, "BACKING_UP", 25, "backing_up_program", "正在备份当前程序资产",
                previous_version=old_version, backup_id=job_id, backup_complete=False,
                recovery_required=False, recovery_pending_health=False, interrupted=False,
                runner_pid=os.getpid(), runner_unit=f"edge-upgrade@{job_id}.service",
            )
            create_program_backup(root, backup_dir)
            transition(
                root, "STOPPING", 35, "stopping_services", "正在停止业务服务并准备一致性数据快照",
                backup_complete=False,
            )
            services_may_be_affected = True
            stop_services()
            shutil.copytree(root / "data", backup_dir / "data", symlinks=False)
            commit_backup(root, backup_dir, job_id, old_version)
            transition(
                root, "STOPPING", 45, "backup_committed", "升级前程序和一致性数据 backup 已完整提交",
                backup_complete=True, last_heartbeat=utc_now(),
            )
            transition(root, "INSTALLING", 55, "installing", "正在通过共享 install.sh 安装新版本")
            install_started = True
            run_checked(["bash", str(extracted_root / "scripts/install.sh"), "--no-start"], timeout=240)
            ensure_installed_state_durable(root)
            transition(root, "STARTING", 72, "starting_services", "正在启动新版本服务")
            start_services()
            transition(root, "VERIFYING", 82, "health_check", "正在执行升级后健康检查")
            services_healthy(root, target_version, timeout=60, full_verify=True)
            result = transition(
                root, "SUCCESS", 100, "complete", "应用升级成功",
                current_version=target_version, finished_at=utc_now(), runner_requested=False,
                error_code="", error_message="", rollback_performed=False,
                recovery_required=False, recovery_pending_health=False,
                recovery_finished_at="", runner_pid=0,
            )
            cleanup_history(root, job_id)
            return result
        except Exception as error:
            upgrade_error = error if isinstance(error, UpdateError) else UpdateError("upgrade_failed", str(error))
            if not install_started:
                if upgrade_error.code == "current_install_changed":
                    fail_current_install_changed(root, read_status(root), upgrade_error)
                    raise upgrade_error
                failed = read_status(root)
                failed.update({
                    "state": "FAILED",
                    "finished_at": utc_now(),
                    "progress": {"percent": 0, "stage": "failed_before_install"},
                    "message": "升级在安装前失败，当前程序未被替换",
                    "error_code": upgrade_error.code,
                    "error_message": upgrade_error.message,
                    "upgrade_error": upgrade_error.message,
                    "rollback_error": "",
                    "runner_requested": False,
                    "runner_pid": 0,
                    "recovery_required": False,
                    "recovery_pending_health": False,
                })
                # 请求 systemd 启动前先持久化安全的 FAILED 状态，让服务启动门禁放行原版本服务。
                write_status(root, failed)
                restart_error = ""
                if services_may_be_affected:
                    restart_error = recover_services_best_effort()
                if restart_error:
                    failed["message"] += "；原业务服务恢复也出现错误，需要人工检查"
                    failed["rollback_error"] = restart_error
                    write_status(root, failed)
                raise upgrade_error
            rolling = transition(
                root, "ROLLING_BACK", 88, "rolling_back", "新版本安装或验证失败，正在恢复旧版本和升级前数据",
                error_code=upgrade_error.code, error_message=upgrade_error.message,
                upgrade_error=upgrade_error.message, rollback_performed=True,
                recovery_required=False, recovery_pending_health=False,
            )
            try:
                stop_services()
                restore_backup(root, backup_dir, old_version, start_after_restore=False)
                rolling.update({
                    "recovery_pending_health": True,
                    "progress": {"percent": 94, "stage": "rollback_waiting_for_health"},
                    "message": "旧版本和升级前数据已恢复，正在启动旧版本服务并执行健康检查",
                })
                write_status(root, rolling)
                start_services()
                services_healthy(root, old_version, timeout=60, full_verify=False)
                rolling.update({
                    "state": "ROLLED_BACK",
                    "current_version": old_version,
                    "finished_at": utc_now(),
                    "progress": {"percent": 100, "stage": "rolled_back"},
                    "message": "新版本升级失败，旧版本和升级前数据已恢复",
                    "runner_requested": False,
                    "runner_pid": 0,
                    "recovery_required": False,
                    "recovery_pending_health": False,
                    "recovery_finished_at": utc_now(),
                })
                write_status(root, rolling)
                cleanup_history(root, job_id)
                return rolling
            except Exception as rollback_exception:
                rollback_error = rollback_exception.message if isinstance(rollback_exception, UpdateError) else str(rollback_exception)
                rolling.update({
                    "state": "FAILED",
                    "finished_at": utc_now(),
                    "progress": {"percent": 0, "stage": "rollback_failed"},
                    "message": "新版本升级失败，自动回滚也失败，需要人工处理",
                    "rollback_error": rollback_error,
                    "runner_requested": False,
                    "runner_pid": 0,
                    "recovery_required": True,
                    "recovery_from_state": "ROLLING_BACK",
                    "recovery_reason": "runner-rollback-failed",
                    "recovery_finished_at": utc_now(),
                })
                write_status(root, rolling)
                raise UpdateError("rollback_failed", f"upgrade={upgrade_error.message}; rollback={rollback_error}") from rollback_exception


def print_json(value: dict[str, Any]) -> None:
    print(json.dumps(value, ensure_ascii=False, separators=(",", ":")))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Edge Controller RK3562 application update manager")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    commands.add_parser("current-version")
    validate = commands.add_parser("validate")
    validate.add_argument("--package", required=True, dest="package_identifier")
    start = commands.add_parser("start")
    start.add_argument("--job-id", required=True)
    run = commands.add_parser("run")
    run.add_argument("--job-id", required=True)
    import_upload = commands.add_parser("import-upload")
    import_upload.add_argument("--upload-id", required=True)
    import_upload.add_argument("--package", required=True, dest="package_identifier")
    validate_root = commands.add_parser("validate-root")
    validate_root.add_argument("--root", required=True, dest="package_root")
    recover = commands.add_parser("recover")
    recover.add_argument("--reason", default="startup")
    commands.add_parser("recover-finalize")
    commands.add_parser("service-start-allowed")
    return parser


def main() -> int:
    arguments = build_parser().parse_args()
    root = PRODUCT_ROOT
    try:
        if arguments.command == "status":
            paths = ensure_workspace(root)
            status = read_status(root)
            if not paths["status"].exists():
                write_status(root, status)
            print_json(status)
        elif arguments.command == "current-version":
            print_json({"version": read_version(root)})
        elif arguments.command == "validate":
            print_json(validate_package_job(root, arguments.package_identifier))
        elif arguments.command == "start":
            print_json(request_start(root, arguments.job_id))
        elif arguments.command == "run":
            print_json(run_job(root, arguments.job_id))
        elif arguments.command == "import-upload":
            print_json(import_uploaded_package(root, arguments.upload_id, arguments.package_identifier))
        elif arguments.command == "validate-root":
            print_json(validate_package_root(Path(arguments.package_root)))
        elif arguments.command == "recover":
            reason = str(arguments.reason).strip()
            if not reason or len(reason) > 120 or re.fullmatch(r"[A-Za-z0-9._:-]+", reason) is None:
                raise UpdateError("invalid_recovery_reason", "恢复原因标识格式无效")
            print_json(recover_transaction(root, reason))
        elif arguments.command == "recover-finalize":
            print_json(finalize_recovery(root))
        elif arguments.command == "service-start-allowed":
            decision = service_start_allowed(root)
            print_json(decision)
            return 0 if decision["allowed"] else 1
        return 0
    except UpdateError as error:
        print(json.dumps({"success": False, "error_code": error.code, "error_message": error.message}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
