package service

// 应用升级服务仅转发受限 IPC 能力，不在 Web 层复制包校验或安装逻辑。

import (
	"context"

	"edge-web/internal/model"
)

func (s *ConsoleService) GetUpdateCurrentVersion(ctx context.Context) (model.UpdateVersion, error) {
	return s.backend.GetUpdateCurrentVersion(ctx)
}

func (s *ConsoleService) GetUpdateStatus(ctx context.Context) (model.UpdateStatus, error) {
	return s.backend.GetUpdateStatus(ctx)
}

func (s *ConsoleService) ImportApplicationUpgradePackage(
	ctx context.Context,
	uploadID string,
	packageIdentifier string,
) (model.UpdateStatus, error) {
	return s.backend.ImportApplicationUpgradePackage(ctx, uploadID, packageIdentifier)
}

func (s *ConsoleService) StartUpdateJob(ctx context.Context, jobID string) (model.UpdateStatus, error) {
	return s.backend.StartUpdateJob(ctx, jobID)
}
