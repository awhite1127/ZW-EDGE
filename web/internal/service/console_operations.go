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
) model.OperationsPageData {
	var wait sync.WaitGroup
	users := startLoad(ctx, &wait, s.ListWebUsers)
	maintenance := startLoad(ctx, &wait, s.GetDataMaintenanceSummary)
	wait.Wait()

	pageData := model.OperationsPageData{
		BasePageData:     model.BasePageData{BackendReachable: true},
		Users:            users.value,
		UserCount:        len(users.value),
		EnabledUserCount: enabledWebUserCount(users.value),
		UsersState:       model.SectionState{Available: true},
		Maintenance:      maintenance.value,
		MaintenanceState: model.SectionState{Available: true},
		Roles:            roles,
	}
	if users.err != nil {
		pageData.UsersState = model.SectionState{ErrorMessage: users.err.Error()}
	}
	if maintenance.err != nil {
		pageData.MaintenanceState = model.SectionState{ErrorMessage: maintenance.err.Error()}
	}
	if users.err != nil && maintenance.err != nil {
		pageData.BackendReachable = false
		pageData.ErrorMessage = "后端不可达，运维管理暂时无法获取"
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
