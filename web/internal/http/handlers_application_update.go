package httpserver

// 应用升级 HTTP 层只负责权限、流式上传和受限 IPC 编排，不解析或安装升级包。

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"

	"edge-web/internal/model"
)

const (
	defaultApplicationUpdateUploadDir        = "/opt/edge-controller/update/upload"
	applicationUpdateMaxPackageBytes   int64 = 256 << 20
	applicationUpdateMultipartOverhead int64 = 1 << 20
	applicationUpdateUploadTTL               = 24 * time.Hour
	applicationUpdateMaxStagedFiles          = 4
	applicationUpdateMaxStagedBytes    int64 = 512 << 20
	applicationUpdateRequestTimeout          = 7 * time.Minute
	applicationUpdateWatchDuration           = time.Hour
	applicationUpdateWatchCookieName         = "edge_update_watch"
)

var (
	applicationUpdatePackageNamePattern = regexp.MustCompile(`^edge-controller-rk3562-[A-Za-z0-9._+\-]+\.tar\.gz$`)
	applicationUpdateUploadNamePattern  = regexp.MustCompile(`^[0-9a-f]{32}\.(?:part|upload)$`)
	applicationUpdateJobIDPattern       = regexp.MustCompile(`^[0-9]{8}T[0-9]{6}Z-[0-9a-f]{8}$`)
	applicationUpdateWatchTokenPattern  = regexp.MustCompile(`^[0-9a-f]{64}$`)
	applicationUpdateWatchNamePattern   = regexp.MustCompile(`^[0-9a-f]{64}\.watch$`)
)

type stagedApplicationUpdate struct {
	UploadID    string
	PackageName string
	Path        string
	Size        int64
}

type applicationUpdateUploadError struct {
	Status  int
	Code    string
	Message string
	Cause   error
}

type applicationUpdateWatchRecord struct {
	JobID     string    `json:"job_id"`
	ExpiresAt time.Time `json:"expires_at"`
}

func (e *applicationUpdateUploadError) Error() string {
	if e == nil {
		return ""
	}
	if e.Cause != nil {
		return e.Message + ": " + e.Cause.Error()
	}
	return e.Message
}

func (s *Server) handleGetApplicationUpdateVersion(w http.ResponseWriter, r *http.Request) {
	if !s.requirePermission(w, r, permissionManageApplicationUpdate) {
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 15*time.Second)
	defer cancel()
	result, err := s.readApplicationUpdateVersion(ctx)
	if err != nil {
		writeApplicationUpdateError(w, err, "版本信息暂时不可用")
		return
	}
	writeResult(w, result, nil)
}

func (s *Server) handleGetApplicationUpdateStatus(w http.ResponseWriter, r *http.Request) {
	if !s.requirePermission(w, r, permissionManageApplicationUpdate) {
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 15*time.Second)
	defer cancel()
	result, err := s.readApplicationUpdateStatus(ctx)
	if err != nil {
		writeApplicationUpdateError(w, err, "升级状态暂时不可用")
		return
	}
	writeResult(w, publicApplicationUpdateStatus(result), nil)
}

func (s *Server) handleUploadApplicationUpdatePackage(w http.ResponseWriter, r *http.Request) {
	if !s.requirePermission(w, r, permissionManageApplicationUpdate) {
		return
	}
	// 上传和正式校验可能持续数分钟。重复点击或第二个浏览器请求不应阻塞并堆积
	// HTTP goroutine；明确返回冲突后，前端可继续轮询当前任务状态。
	if !s.applicationUpdateMu.TryLock() {
		writeError(w, http.StatusConflict, "upgrade_upload_busy", "已有升级包正在上传或校验，请稍后再试")
		return
	}
	defer s.applicationUpdateMu.Unlock()

	controller := http.NewResponseController(w)
	_ = controller.SetReadDeadline(time.Now().Add(applicationUpdateRequestTimeout))
	_ = controller.SetWriteDeadline(time.Now().Add(applicationUpdateRequestTimeout))

	uploadDir := s.applicationUpdateUploadDir
	if strings.TrimSpace(uploadDir) == "" {
		uploadDir = defaultApplicationUpdateUploadDir
	}
	if err := cleanupApplicationUpdateUploads(uploadDir, "", applicationUpdateMaxStagedFiles-1); err != nil {
		log.Printf("清理升级上传暂存区失败: %v", err)
		writeError(w, http.StatusServiceUnavailable, "upload_staging_unavailable", "升级上传暂存区暂时不可用")
		return
	}

	staged, err := stageApplicationUpdateUpload(w, r, uploadDir, applicationUpdateMaxPackageBytes)
	if err != nil {
		var uploadError *applicationUpdateUploadError
		if errors.As(err, &uploadError) {
			if uploadError.Cause != nil {
				log.Printf("升级包流式上传失败: code=%s error=%v", uploadError.Code, uploadError.Cause)
			}
			writeError(w, uploadError.Status, uploadError.Code, uploadError.Message)
			return
		}
		log.Printf("升级包流式上传失败: %v", err)
		writeError(w, http.StatusBadRequest, "upgrade_upload_failed", "升级包上传失败")
		return
	}
	defer os.Remove(staged.Path)
	// 上传和正式包校验分别获得有界时间，避免慢速上传耗尽响应写期限。
	_ = controller.SetWriteDeadline(time.Now().Add(applicationUpdateRequestTimeout))
	if err := cleanupApplicationUpdateUploads(uploadDir, filepath.Base(staged.Path), applicationUpdateMaxStagedFiles); err != nil {
		log.Printf("上传完成后清理升级暂存区失败: %v", err)
	}

	ctx, cancel := context.WithTimeout(r.Context(), applicationUpdateRequestTimeout)
	defer cancel()
	result, err := s.importUploadedApplicationUpdate(ctx, staged.UploadID, staged.PackageName)
	if err != nil {
		log.Printf("升级包 root 接管或正式校验失败: upload_id=%s size=%d error=%v", staged.UploadID, staged.Size, err)
		writeApplicationUpdateError(w, err, "升级包校验失败")
		return
	}
	writeSuccess(w, publicApplicationUpdateStatus(result))
}

func (s *Server) handleStartApplicationUpdate(w http.ResponseWriter, r *http.Request) {
	if !s.requirePermission(w, r, permissionManageApplicationUpdate) {
		return
	}
	var request model.ApplicationUpdateStartRequest
	if !decodeJSONRequest(w, r, &request, maxStandardJSONRequestBodyBytes) {
		return
	}
	request.JobID = strings.TrimSpace(request.JobID)
	if !applicationUpdateJobIDPattern.MatchString(request.JobID) {
		writeError(w, http.StatusBadRequest, "invalid_job_id", "升级任务标识无效，请重新上传并校验升级包")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 45*time.Second)
	defer cancel()
	result, err := s.startUploadedApplicationUpdate(ctx, request.JobID)
	if err != nil {
		log.Printf("启动应用升级任务失败: job_id=%s error=%v", request.JobID, err)
		writeApplicationUpdateError(w, err, "升级任务启动失败")
		return
	}
	writeSuccess(w, publicApplicationUpdateStatus(result))
}

func (s *Server) handleCreateApplicationUpdateWatch(w http.ResponseWriter, r *http.Request) {
	if !s.requirePermission(w, r, permissionManageApplicationUpdate) {
		return
	}
	var request model.ApplicationUpdateStartRequest
	if !decodeJSONRequest(w, r, &request, maxStandardJSONRequestBodyBytes) {
		return
	}
	request.JobID = strings.TrimSpace(request.JobID)
	if !applicationUpdateJobIDPattern.MatchString(request.JobID) {
		writeError(w, http.StatusBadRequest, "invalid_job_id", "升级任务标识无效")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 15*time.Second)
	defer cancel()
	status, err := s.readApplicationUpdateStatus(ctx)
	if err != nil || status.State != "READY" || status.JobID != request.JobID {
		writeError(w, http.StatusConflict, "upgrade_job_not_ready", "升级任务尚未就绪，请重新校验升级包")
		return
	}
	uploadDir := s.applicationUpdateUploadDir
	if strings.TrimSpace(uploadDir) == "" {
		uploadDir = defaultApplicationUpdateUploadDir
	}
	token, err := createApplicationUpdateWatch(uploadDir, request.JobID, time.Now())
	if err != nil {
		log.Printf("创建升级断连观察凭证失败: job_id=%s error=%v", request.JobID, err)
		writeError(w, http.StatusServiceUnavailable, "upgrade_watch_unavailable", "无法建立升级断连等待状态，请稍后重试")
		return
	}
	http.SetCookie(w, &http.Cookie{
		Name: applicationUpdateWatchCookieName, Value: token, Path: "/",
		MaxAge: int(applicationUpdateWatchDuration.Seconds()), HttpOnly: true,
		Secure: shouldUseSecureCookie(r), SameSite: http.SameSiteStrictMode,
	})
	writeSuccess(w, map[string]bool{"ready": true})
}

func (s *Server) handleWatchApplicationUpdateStatus(w http.ResponseWriter, r *http.Request) {
	uploadDir := s.applicationUpdateUploadDir
	if strings.TrimSpace(uploadDir) == "" {
		uploadDir = defaultApplicationUpdateUploadDir
	}
	record, err := readApplicationUpdateWatch(r, uploadDir, time.Now())
	if err != nil {
		writeError(w, http.StatusUnauthorized, "upgrade_watch_invalid", "升级等待凭证不存在或已过期")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 15*time.Second)
	defer cancel()
	status, err := s.readApplicationUpdateStatus(ctx)
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, "upgrade_status_unavailable", "服务正在升级并重新启动，请稍候")
		return
	}
	if status.JobID != record.JobID {
		writeError(w, http.StatusConflict, "upgrade_watch_job_changed", "升级任务状态已变更，请重新登录后查看")
		return
	}
	writeResult(w, publicApplicationUpdateStatus(status), nil)
}

func (s *Server) handleApplicationUpdateWaitPage(w http.ResponseWriter, r *http.Request) {
	uploadDir := s.applicationUpdateUploadDir
	if strings.TrimSpace(uploadDir) == "" {
		uploadDir = defaultApplicationUpdateUploadDir
	}
	if _, err := readApplicationUpdateWatch(r, uploadDir, time.Now()); err != nil {
		http.Redirect(w, r, "/login?redirect=/operations", http.StatusSeeOther)
		return
	}
	data := model.OperationsPageData{BasePageData: s.basePageData(
		"应用升级", "operations", "服务升级期间会自动等待并恢复状态。", r,
	)}
	s.renderPage(w, "application_update_wait", data)
}

func stageApplicationUpdateUpload(
	w http.ResponseWriter,
	r *http.Request,
	uploadDir string,
	maxPackageBytes int64,
) (stagedApplicationUpdate, error) {
	if maxPackageBytes <= 0 {
		maxPackageBytes = applicationUpdateMaxPackageBytes
	}
	if r.ContentLength > maxPackageBytes+applicationUpdateMultipartOverhead {
		return stagedApplicationUpdate{}, &applicationUpdateUploadError{
			Status: http.StatusRequestEntityTooLarge, Code: "upgrade_package_too_large",
			Message: "升级包不能超过 256 MiB",
		}
	}
	directoryInfo, err := os.Lstat(uploadDir)
	if err != nil || !directoryInfo.IsDir() || directoryInfo.Mode()&os.ModeSymlink != 0 {
		return stagedApplicationUpdate{}, &applicationUpdateUploadError{
			Status: http.StatusServiceUnavailable, Code: "upload_staging_unavailable",
			Message: "升级上传暂存区暂时不可用", Cause: err,
		}
	}
	r.Body = http.MaxBytesReader(w, r.Body, maxPackageBytes+applicationUpdateMultipartOverhead)
	reader, err := r.MultipartReader()
	if err != nil {
		return stagedApplicationUpdate{}, &applicationUpdateUploadError{
			Status: http.StatusBadRequest, Code: "invalid_multipart_upload",
			Message: "升级包上传请求格式不正确", Cause: err,
		}
	}

	var staged stagedApplicationUpdate
	for {
		part, nextErr := reader.NextPart()
		if errors.Is(nextErr, io.EOF) {
			break
		}
		if nextErr != nil {
			if staged.Path != "" {
				_ = os.Remove(staged.Path)
			}
			return stagedApplicationUpdate{}, uploadReadError(nextErr)
		}
		if part.FormName() != "upgrade_file" || part.FileName() == "" || staged.Path != "" {
			_ = part.Close()
			if staged.Path != "" {
				_ = os.Remove(staged.Path)
			}
			return stagedApplicationUpdate{}, &applicationUpdateUploadError{
				Status: http.StatusBadRequest, Code: "invalid_upgrade_file_field",
				Message: "请仅选择一个正式 RK3562 升级包",
			}
		}

		packageName := filepath.Base(strings.ReplaceAll(part.FileName(), "\\", "/"))
		if !applicationUpdatePackageNamePattern.MatchString(packageName) {
			_ = part.Close()
			return stagedApplicationUpdate{}, &applicationUpdateUploadError{
				Status: http.StatusBadRequest, Code: "invalid_upgrade_package_name",
				Message: "只支持正式 edge-controller-rk3562-<version>.tar.gz 升级包",
			}
		}
		uploadID, idErr := newApplicationUpdateUploadID()
		if idErr != nil {
			_ = part.Close()
			return stagedApplicationUpdate{}, &applicationUpdateUploadError{
				Status: http.StatusInternalServerError, Code: "upload_id_failed",
				Message: "无法创建升级上传任务", Cause: idErr,
			}
		}
		partPath := filepath.Join(uploadDir, uploadID+".part")
		finalPath := filepath.Join(uploadDir, uploadID+".upload")
		file, createErr := os.OpenFile(partPath, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
		if createErr != nil {
			_ = part.Close()
			return stagedApplicationUpdate{}, &applicationUpdateUploadError{
				Status: http.StatusServiceUnavailable, Code: "upload_staging_unavailable",
				Message: "升级上传暂存区暂时不可用", Cause: createErr,
			}
		}
		copied, copyErr := io.CopyBuffer(file, io.LimitReader(part, maxPackageBytes+1), make([]byte, 64<<10))
		syncErr := file.Sync()
		closeErr := file.Close()
		_ = part.Close()
		if copyErr != nil || syncErr != nil || closeErr != nil || copied > maxPackageBytes || copied == 0 {
			_ = os.Remove(partPath)
			switch {
			case copied > maxPackageBytes:
				return stagedApplicationUpdate{}, &applicationUpdateUploadError{
					Status: http.StatusRequestEntityTooLarge, Code: "upgrade_package_too_large",
					Message: "升级包不能超过 256 MiB",
				}
			case copied == 0 && copyErr == nil:
				return stagedApplicationUpdate{}, &applicationUpdateUploadError{
					Status: http.StatusBadRequest, Code: "upgrade_package_empty", Message: "升级包不能为空",
				}
			default:
				firstErr := copyErr
				if firstErr == nil {
					firstErr = syncErr
				}
				if firstErr == nil {
					firstErr = closeErr
				}
				return stagedApplicationUpdate{}, uploadReadError(firstErr)
			}
		}
		if err := os.Chmod(partPath, 0o600); err != nil {
			_ = os.Remove(partPath)
			return stagedApplicationUpdate{}, &applicationUpdateUploadError{
				Status: http.StatusServiceUnavailable, Code: "upload_staging_unavailable",
				Message: "无法设置升级包暂存权限", Cause: err,
			}
		}
		if err := os.Rename(partPath, finalPath); err != nil {
			_ = os.Remove(partPath)
			return stagedApplicationUpdate{}, &applicationUpdateUploadError{
				Status: http.StatusServiceUnavailable, Code: "upload_staging_unavailable",
				Message: "无法提交完整升级包", Cause: err,
			}
		}
		staged = stagedApplicationUpdate{UploadID: uploadID, PackageName: packageName, Path: finalPath, Size: copied}
	}
	if staged.Path == "" {
		return stagedApplicationUpdate{}, &applicationUpdateUploadError{
			Status: http.StatusBadRequest, Code: "upgrade_package_required", Message: "请选择要上传的升级包",
		}
	}
	return staged, nil
}

func uploadReadError(err error) *applicationUpdateUploadError {
	var maxBytesError *http.MaxBytesError
	if errors.As(err, &maxBytesError) {
		return &applicationUpdateUploadError{
			Status: http.StatusRequestEntityTooLarge, Code: "upgrade_package_too_large",
			Message: "升级包不能超过 256 MiB", Cause: err,
		}
	}
	return &applicationUpdateUploadError{
		Status: http.StatusBadRequest, Code: "upgrade_upload_interrupted",
		Message: "升级包上传中断，请重新选择文件", Cause: err,
	}
}

func newApplicationUpdateUploadID() (string, error) {
	var value [16]byte
	if _, err := rand.Read(value[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(value[:]), nil
}

func createApplicationUpdateWatch(uploadDir string, jobID string, now time.Time) (string, error) {
	if !applicationUpdateJobIDPattern.MatchString(jobID) {
		return "", fmt.Errorf("非法升级任务标识")
	}
	if err := cleanupApplicationUpdateWatchFiles(uploadDir, now); err != nil {
		return "", err
	}
	var tokenBytes [32]byte
	if _, err := rand.Read(tokenBytes[:]); err != nil {
		return "", err
	}
	token := hex.EncodeToString(tokenBytes[:])
	digest := sha256.Sum256([]byte(token))
	path := filepath.Join(uploadDir, hex.EncodeToString(digest[:])+".watch")
	payload, err := json.Marshal(applicationUpdateWatchRecord{JobID: jobID, ExpiresAt: now.Add(applicationUpdateWatchDuration)})
	if err != nil {
		return "", err
	}
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return "", err
	}
	removeOnFailure := true
	defer func() {
		_ = file.Close()
		if removeOnFailure {
			_ = os.Remove(path)
		}
	}()
	if _, err := file.Write(append(payload, '\n')); err != nil {
		return "", err
	}
	if err := file.Sync(); err != nil {
		return "", err
	}
	if err := file.Close(); err != nil {
		return "", err
	}
	removeOnFailure = false
	return token, nil
}

func readApplicationUpdateWatch(r *http.Request, uploadDir string, now time.Time) (applicationUpdateWatchRecord, error) {
	cookie, err := r.Cookie(applicationUpdateWatchCookieName)
	if err != nil || !applicationUpdateWatchTokenPattern.MatchString(cookie.Value) {
		return applicationUpdateWatchRecord{}, fmt.Errorf("观察凭证缺失")
	}
	digest := sha256.Sum256([]byte(cookie.Value))
	path := filepath.Join(uploadDir, hex.EncodeToString(digest[:])+".watch")
	metadata, err := os.Lstat(path)
	if err != nil || !metadata.Mode().IsRegular() || metadata.Mode()&os.ModeSymlink != 0 || metadata.Mode().Perm() != 0o600 || metadata.Size() > 4096 {
		return applicationUpdateWatchRecord{}, fmt.Errorf("观察凭证文件无效")
	}
	content, err := os.ReadFile(path)
	if err != nil {
		return applicationUpdateWatchRecord{}, err
	}
	var record applicationUpdateWatchRecord
	if err := json.Unmarshal(content, &record); err != nil || !applicationUpdateJobIDPattern.MatchString(record.JobID) || !now.Before(record.ExpiresAt) {
		return applicationUpdateWatchRecord{}, fmt.Errorf("观察凭证已过期或内容无效")
	}
	return record, nil
}

func cleanupApplicationUpdateWatchFiles(uploadDir string, now time.Time) error {
	entries, err := os.ReadDir(uploadDir)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if !applicationUpdateWatchNamePattern.MatchString(entry.Name()) {
			continue
		}
		path := filepath.Join(uploadDir, entry.Name())
		metadata, statErr := os.Lstat(path)
		if statErr != nil {
			continue
		}
		if metadata.Mode()&os.ModeSymlink != 0 || !metadata.Mode().IsRegular() || now.Sub(metadata.ModTime()) > applicationUpdateWatchDuration {
			_ = os.Remove(path)
		}
	}
	return nil
}

type stagedUploadEntry struct {
	path    string
	name    string
	size    int64
	modTime time.Time
}

func cleanupApplicationUpdateUploads(uploadDir string, protectedName string, maxFiles int) error {
	directoryInfo, err := os.Lstat(uploadDir)
	if err != nil {
		return err
	}
	if !directoryInfo.IsDir() || directoryInfo.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("升级上传暂存区不是安全目录")
	}
	entries, err := os.ReadDir(uploadDir)
	if err != nil {
		return err
	}
	now := time.Now()
	kept := make([]stagedUploadEntry, 0, len(entries))
	for _, entry := range entries {
		name := entry.Name()
		if !applicationUpdateUploadNamePattern.MatchString(name) {
			continue
		}
		path := filepath.Join(uploadDir, name)
		metadata, statErr := os.Lstat(path)
		if statErr != nil {
			if errors.Is(statErr, os.ErrNotExist) {
				continue
			}
			return statErr
		}
		if metadata.Mode()&os.ModeSymlink != 0 || !metadata.Mode().IsRegular() {
			if name != protectedName {
				_ = os.Remove(path)
			}
			continue
		}
		if name != protectedName && now.Sub(metadata.ModTime()) > applicationUpdateUploadTTL {
			_ = os.Remove(path)
			continue
		}
		kept = append(kept, stagedUploadEntry{path: path, name: name, size: metadata.Size(), modTime: metadata.ModTime()})
	}
	sort.Slice(kept, func(i, j int) bool { return kept[i].modTime.Before(kept[j].modTime) })
	var total int64
	for _, entry := range kept {
		total += entry.size
	}
	if maxFiles < 0 {
		maxFiles = 0
	}
	for len(kept) > maxFiles || total > applicationUpdateMaxStagedBytes {
		removeIndex := -1
		for index, entry := range kept {
			if entry.name != protectedName {
				removeIndex = index
				break
			}
		}
		if removeIndex < 0 {
			break
		}
		entry := kept[removeIndex]
		if err := os.Remove(entry.path); err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}
		total -= entry.size
		kept = append(kept[:removeIndex], kept[removeIndex+1:]...)
	}
	return nil
}

func writeApplicationUpdateError(w http.ResponseWriter, err error, fallback string) {
	raw := strings.ToLower(err.Error())
	status := http.StatusUnprocessableEntity
	code := "upgrade_validation_failed"
	message := fallback
	switch {
	case strings.Contains(raw, "update_busy"), strings.Contains(raw, "job_not_ready"):
		status, code, message = http.StatusConflict, "upgrade_busy", "已有升级任务正在执行，请稍后再试"
	case strings.Contains(raw, "upload_too_large"):
		status, code, message = http.StatusRequestEntityTooLarge, "upgrade_package_too_large", "升级包不能超过 256 MiB"
	case strings.Contains(raw, "upload_not_found"), strings.Contains(raw, "invalid_upload"):
		status, code, message = http.StatusBadRequest, "upgrade_upload_invalid", "上传文件已失效或不符合安全要求，请重新上传"
	case strings.Contains(raw, "insufficient_disk_space"):
		code, message = "upgrade_space_insufficient", "升级所需空间超过当前可用空间"
	case strings.Contains(raw, "current_install_changed"):
		status, code, message = http.StatusConflict, "upgrade_current_install_changed", "当前安装已发生变化，请重新上传并校验升级包"
	case strings.Contains(raw, "signature_required"):
		code, message = "upgrade_signature_required", "当前设备只接受带正式数字签名的升级包"
	case strings.Contains(raw, "signature"), strings.Contains(raw, "trusted_key"):
		code, message = "upgrade_signature_invalid", "升级包数字签名无效或设备信任公钥不可用"
	case strings.Contains(raw, "manifest"), strings.Contains(raw, "missing_required"):
		code, message = "upgrade_package_incomplete", "升级包文件不完整或完整性校验失败"
	case strings.Contains(raw, "elf_arch"), strings.Contains(raw, "package_mismatch"),
		strings.Contains(raw, "filename_version_mismatch"), strings.Contains(raw, "soname"),
		strings.Contains(raw, "invalid_static_binary"):
		code, message = "upgrade_target_mismatch", "升级包不是适用于 RK3562 ARM64 的正式版本"
	case strings.Contains(raw, "invalid_archive"), strings.Contains(raw, "unsafe_archive"),
		strings.Contains(raw, "extract_failed"), strings.Contains(raw, "invalid_top_level"):
		code, message = "upgrade_package_format_invalid", "升级包格式不正确"
	case strings.Contains(raw, "job_not_found"), strings.Contains(raw, "invalid_job"):
		status, code, message = http.StatusConflict, "upgrade_job_invalid", "升级任务已失效，请重新上传并校验升级包"
	}
	writeError(w, status, code, message)
}

func publicApplicationUpdateStatus(status model.UpdateStatus) model.UpdateStatus {
	status.PackagePath = ""
	if status.ErrorCode != "" || status.ErrorMessage != "" {
		status.ErrorMessage = publicApplicationUpdateErrorText(status.ErrorCode, status.ErrorMessage)
	}
	if status.UpgradeError != "" {
		status.UpgradeError = status.ErrorMessage
	}
	if status.RollbackError != "" {
		status.RollbackError = "自动恢复过程也出现错误，请查看升级日志或联系维护人员"
	}
	if status.RecoveryError != "" {
		status.RecoveryError = "升级中断后的自动恢复未完成，请查看升级日志或联系维护人员"
	}
	status.RunnerPID = 0
	return status
}

func publicApplicationUpdateErrorText(code string, message string) string {
	raw := strings.ToLower(code + " " + message)
	switch {
	case strings.Contains(raw, "insufficient_disk_space"):
		return "升级所需空间超过当前可用空间"
	case strings.Contains(raw, "current_install_changed"):
		return "当前安装已发生变化，请重新上传并校验升级包"
	case strings.Contains(raw, "signature_required"):
		return "当前设备只接受带正式数字签名的升级包"
	case strings.Contains(raw, "signature"), strings.Contains(raw, "trusted_key"):
		return "升级包数字签名无效或设备信任公钥不可用"
	case strings.Contains(raw, "manifest"), strings.Contains(raw, "missing_required"):
		return "升级包文件不完整或完整性校验失败"
	case strings.Contains(raw, "elf"), strings.Contains(raw, "package_mismatch"), strings.Contains(raw, "soname"):
		return "升级包不是适用于 RK3562 ARM64 的正式版本"
	case strings.Contains(raw, "archive"), strings.Contains(raw, "extract"):
		return "升级包格式不正确"
	case strings.Contains(raw, "update_busy"):
		return "已有升级任务正在执行"
	case strings.Contains(raw, "recovery"), strings.Contains(raw, "rollback"):
		return "升级中断后的自动恢复未完成，请查看升级日志或联系维护人员"
	case strings.TrimSpace(message) != "":
		return "升级未完成，请查看升级日志了解详细原因"
	default:
		return ""
	}
}

func (s *Server) readApplicationUpdateVersion(ctx context.Context) (model.UpdateVersion, error) {
	if s.getUpdateCurrentVersion != nil {
		return s.getUpdateCurrentVersion(ctx)
	}
	return s.console.GetUpdateCurrentVersion(ctx)
}

func (s *Server) readApplicationUpdateStatus(ctx context.Context) (model.UpdateStatus, error) {
	if s.getUpdateStatus != nil {
		return s.getUpdateStatus(ctx)
	}
	return s.console.GetUpdateStatus(ctx)
}

func (s *Server) importUploadedApplicationUpdate(ctx context.Context, uploadID string, packageName string) (model.UpdateStatus, error) {
	if s.importApplicationUpdate != nil {
		return s.importApplicationUpdate(ctx, uploadID, packageName)
	}
	return s.console.ImportApplicationUpgradePackage(ctx, uploadID, packageName)
}

func (s *Server) startUploadedApplicationUpdate(ctx context.Context, jobID string) (model.UpdateStatus, error) {
	if s.startApplicationUpdate != nil {
		return s.startApplicationUpdate(ctx, jobID)
	}
	return s.console.StartUpdateJob(ctx, jobID)
}
