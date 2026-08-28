package httpserver

// Modbus 北向页面与 JSON API。认证、CSRF 和 RBAC 仍由全局中间件统一执行。
import (
	"net/http"
	"strings"

	"edge-web/internal/model"
)

// handleModbusServerPage 处理Modbus服务页面请求。
func (s *Server) handleModbusServerPage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	data := model.ModbusServerPageData{
		BasePageData: s.basePageData(
			"Modbus 北向服务", "settings", "配置 TCP Server 监听与保持寄存器发布映射。", r),
	}
	if snapshot, err := s.console.GetModbusServerPageSnapshot(ctx); err == nil {
		data.Snapshot = snapshot
		data.SnapshotState.Available = true
	} else {
		data.SnapshotState.ErrorMessage = err.Error()
		data.BackendReachable = false
	}
	if data.CanManageModbusMappings {
		if points, err := s.console.ListModbusExportablePoints(ctx); err == nil {
			data.ExportablePoints = points
			data.PointsState.Available = true
		} else {
			data.PointsState.ErrorMessage = err.Error()
		}
	}
	s.applySystemDisplayNameFromBackend(ctx, &data.BasePageData)
	s.renderPage(w, "settings_modbus_server", data)
}

// handleGetModbusServerPageSnapshot 处理Modbus服务页面快照查询请求。
func (s *Server) handleGetModbusServerPageSnapshot(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.GetModbusServerPageSnapshot(ctx)
	writeResult(w, result, err)
}

// handleGetModbusServerRuntimeStatus 处理Modbus服务运行态状态查询请求。
func (s *Server) handleGetModbusServerRuntimeStatus(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.GetModbusServerRuntimeStatus(ctx)
	writeResult(w, result, err)
}

// handleListModbusExportablePoints 处理Modbus可导出点位列表查询请求。
func (s *Server) handleListModbusExportablePoints(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.ListModbusExportablePoints(ctx)
	writeResult(w, result, err)
}

// handleListModbusRegisterMappings 处理Modbus寄存器映射列表查询请求。
func (s *Server) handleListModbusRegisterMappings(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.ListModbusRegisterMappings(ctx)
	writeResult(w, result, err)
}

// handleUpdateModbusServerSettings 处理Modbus服务设置更新请求。
func (s *Server) handleUpdateModbusServerSettings(w http.ResponseWriter, r *http.Request) {
	var request model.ModbusServerSettings
	if !decodeJSONRequest(w, r, &request, maxStandardJSONRequestBodyBytes) {
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.UpdateModbusServerSettings(ctx, request)
	writeResult(w, result, err)
}

// handleCreateModbusRegisterMapping 处理Modbus寄存器映射创建请求。
func (s *Server) handleCreateModbusRegisterMapping(w http.ResponseWriter, r *http.Request) {
	var request model.ModbusRegisterMappingRequest
	if !decodeJSONRequest(w, r, &request, maxStandardJSONRequestBodyBytes) {
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.CreateModbusRegisterMapping(ctx, request)
	writeResult(w, result, err)
}

// handleUpdateModbusRegisterMapping 处理Modbus寄存器映射更新请求。
func (s *Server) handleUpdateModbusRegisterMapping(w http.ResponseWriter, r *http.Request) {
	mappingID := strings.TrimSpace(r.PathValue("id"))
	if mappingID == "" {
		writeError(w, http.StatusBadRequest, "invalid_argument", "缺少映射 ID")
		return
	}
	var request model.ModbusRegisterMappingRequest
	if !decodeJSONRequest(w, r, &request, maxStandardJSONRequestBodyBytes) {
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.UpdateModbusRegisterMapping(ctx, mappingID, request)
	writeResult(w, result, err)
}

// handleDeleteModbusRegisterMapping 处理Modbus寄存器映射删除请求。
func (s *Server) handleDeleteModbusRegisterMapping(w http.ResponseWriter, r *http.Request) {
	mappingID := strings.TrimSpace(r.PathValue("id"))
	if mappingID == "" {
		writeError(w, http.StatusBadRequest, "invalid_argument", "缺少映射 ID")
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	err := s.console.DeleteModbusRegisterMapping(ctx, mappingID)
	writeResult(w, map[string]bool{"deleted": err == nil}, err)
}
