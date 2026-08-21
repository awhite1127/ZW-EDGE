// 运维页面服务：并列加载用户与数据维护摘要，并把各自错误收敛为可独立展示的区块状态。
package service

import (
	"context"
	"sync"

	"edge-web/internal/model"
)

// LoadOperations 不因一个区块失败而丢弃另一个区块的有效数据，便于后端局部异常时继续运维。
func (s *ConsoleService) LoadOperations(
	ctx context.Context,
	roles []model.RolePermissionView,
	includeApplicationUpdate bool,
) model.OperationsPageData {
	var (
		users          []model.WebUser
		usersErr       error
		maintenance    model.DataMaintenanceSummary
		maintenanceErr error
		version        model.UpdateVersion
		versionErr     error
		status         model.UpdateStatus
		statusErr      error
		wait           sync.WaitGroup
	)
	wait.Add(2)
	go func() {
		defer wait.Done()
		users, usersErr = s.ListWebUsers(ctx)
	}()
	go func() {
		defer wait.Done()
		maintenance, maintenanceErr = s.GetDataMaintenanceSummary(ctx)
	}()
	if includeApplicationUpdate {
		wait.Add(2)
		go func() {
			defer wait.Done()
			version, versionErr = s.GetUpdateCurrentVersion(ctx)
		}()
		go func() {
			defer wait.Done()
			status, statusErr = s.GetUpdateStatus(ctx)
		}()
	}
	wait.Wait()

	pageData := model.OperationsPageData{
		BasePageData:     model.BasePageData{BackendReachable: true},
		Users:            users,
		UserCount:        len(users),
		EnabledUserCount: enabledWebUserCount(users),
		UsersState:       model.SectionState{Available: true},
		Maintenance:      maintenance,
		MaintenanceState: model.SectionState{Available: true},
		Roles:            roles,
	}
	if usersErr != nil {
		pageData.UsersState = model.SectionState{ErrorMessage: usersErr.Error()}
	}
	if maintenanceErr != nil {
		pageData.MaintenanceState = model.SectionState{ErrorMessage: maintenanceErr.Error()}
	}
	if usersErr != nil && maintenanceErr != nil {
		pageData.BackendReachable = false
		pageData.ErrorMessage = "后端不可达，运维管理暂时无法获取"
	}
	if includeApplicationUpdate {
		pageData.UpdateVersion = version
		pageData.UpdateStatus = status
		pageData.UpdateState = model.SectionState{Available: versionErr == nil && statusErr == nil}
		if versionErr != nil {
			pageData.UpdateState.ErrorMessage = versionErr.Error()
		} else if statusErr != nil {
			pageData.UpdateState.ErrorMessage = statusErr.Error()
		}
	}
	return pageData
}

// enabledWebUserCount 统计当前已启用的 Web 用户数量。
func enabledWebUserCount(users []model.WebUser) int {
	count := 0
	for _, user := range users {
		if user.Enabled {
			count++
		}
	}
	return count
}
