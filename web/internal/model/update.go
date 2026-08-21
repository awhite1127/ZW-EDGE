package model

// UpdateVersion 是当前安装版本的轻量查询结果。
type UpdateVersion struct {
	Version string `json:"version"`
}

// UpdateProgress 描述持久升级任务当前阶段和近似进度。
type UpdateProgress struct {
	Percent int    `json:"percent"`
	Stage   string `json:"stage"`
}

// UpdateSignatureStatus 描述升级包真实性签名的校验结果。
type UpdateSignatureStatus struct {
	Required      bool   `json:"required"`
	Status        string `json:"status"`
	Scheme        string `json:"scheme"`
	KeyID         string `json:"key_id"`
	SignatureFile string `json:"signature_file"`
	Signed        bool   `json:"signed"`
	Verified      bool   `json:"signature_verified"`
	Algorithm     string `json:"signature_algorithm"`
}

// UpdatePackageInfo 是签名、清单和版本约束校验通过后可展示的安全包摘要。
type UpdatePackageInfo struct {
	Product   string `json:"product"`
	Platform  string `json:"platform"`
	Arch      string `json:"arch"`
	Version   string `json:"version"`
	BuildTime string `json:"build_time"`
	SizeBytes int64  `json:"size_bytes"`
}

// UpdateStatus 是 status.json 和 UDS 升级查询共用的响应模型。
type UpdateStatus struct {
	JobID              string                `json:"job_id"`
	State              string                `json:"state"`
	CurrentVersion     string                `json:"current_version"`
	TargetVersion      string                `json:"target_version"`
	PackagePath        string                `json:"package_path"`
	CreatedAt          string                `json:"created_at"`
	StartedAt          string                `json:"started_at"`
	FinishedAt         string                `json:"finished_at"`
	Progress           UpdateProgress        `json:"progress"`
	Message            string                `json:"message"`
	ErrorCode          string                `json:"error_code"`
	ErrorMessage       string                `json:"error_message"`
	RollbackPerformed  bool                  `json:"rollback_performed"`
	UpgradeError       string                `json:"upgrade_error"`
	RollbackError      string                `json:"rollback_error"`
	RunnerRequested    bool                  `json:"runner_requested"`
	RunnerPID          int                   `json:"runner_pid"`
	RunnerUnit         string                `json:"runner_unit"`
	LastHeartbeat      string                `json:"last_heartbeat"`
	Interrupted        bool                  `json:"interrupted"`
	RecoveryRequired   bool                  `json:"recovery_required"`
	RecoveryPending    bool                  `json:"recovery_pending_health"`
	RecoveryStartedAt  string                `json:"recovery_started_at"`
	RecoveryFinishedAt string                `json:"recovery_finished_at"`
	RecoveryReason     string                `json:"recovery_reason"`
	RecoveryFromState  string                `json:"recovery_from_state"`
	RecoveryAttempts   int                   `json:"recovery_attempt_count"`
	RecoveryError      string                `json:"recovery_error"`
	BackupID           string                `json:"backup_id"`
	BackupComplete     bool                  `json:"backup_complete"`
	PreviousVersion    string                `json:"previous_version"`
	ServiceStatus      map[string]string     `json:"service_status,omitempty"`
	Signature          UpdateSignatureStatus `json:"signature"`
	PackageInfo        UpdatePackageInfo     `json:"package_info"`
}

// ApplicationUpdateStartRequest 启动已校验的 READY 任务。
type ApplicationUpdateStartRequest struct {
	JobID string `json:"job_id"`
}
