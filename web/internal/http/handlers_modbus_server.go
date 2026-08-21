package httpserver

// Modbus 北向页面与 JSON API。认证、CSRF 和 RBAC 仍由全局中间件统一执行。
import (
	"errors"
	"math"
	"net"
	"net/http"
	"strings"

	"edge-web/internal/ipc"
	"edge-web/internal/model"
)

const maxModbusRequestBodyBytes int64 = 64 << 10

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
		data.SnapshotState.ErrorMessage = userVisibleErrorMessage(err.Error())
		data.BackendReachable = false
	}
	if data.CanManageModbusMappings {
		if points, err := s.console.ListModbusExportablePoints(ctx); err == nil {
			data.ExportablePoints = points
			data.PointsState.Available = true
		} else {
			data.PointsState.ErrorMessage = userVisibleErrorMessage(err.Error())
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
	writeModbusResult(w, result, err)
}

// handleGetModbusServerRuntimeStatus 处理Modbus服务运行态状态查询请求。
func (s *Server) handleGetModbusServerRuntimeStatus(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.GetModbusServerRuntimeStatus(ctx)
	writeModbusResult(w, result, err)
}

// handleListModbusExportablePoints 处理Modbus可导出点位列表查询请求。
func (s *Server) handleListModbusExportablePoints(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.ListModbusExportablePoints(ctx)
	writeModbusResult(w, result, err)
}

// handleListModbusRegisterMappings 处理Modbus寄存器映射列表查询请求。
func (s *Server) handleListModbusRegisterMappings(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.ListModbusRegisterMappings(ctx)
	writeModbusResult(w, result, err)
}

// handleUpdateModbusServerSettings 处理Modbus服务设置更新请求。
func (s *Server) handleUpdateModbusServerSettings(w http.ResponseWriter, r *http.Request) {
	var request model.ModbusServerSettings
	if !decodeStrictJSON(w, r, &request) {
		return
	}
	if message := validateModbusServerSettings(request); message != "" {
		writeError(w, http.StatusBadRequest, "invalid_argument", message)
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.UpdateModbusServerSettings(ctx, request)
	writeModbusResult(w, result, err)
}

// handleCreateModbusRegisterMapping 处理Modbus寄存器映射创建请求。
func (s *Server) handleCreateModbusRegisterMapping(w http.ResponseWriter, r *http.Request) {
	var request model.ModbusRegisterMappingRequest
	if !decodeStrictJSON(w, r, &request) {
		return
	}
	if message := validateModbusMappingRequest(request); message != "" {
		writeError(w, http.StatusBadRequest, "invalid_argument", message)
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.CreateModbusRegisterMapping(ctx, request)
	writeModbusResult(w, result, err)
}

// handleUpdateModbusRegisterMapping 处理Modbus寄存器映射更新请求。
func (s *Server) handleUpdateModbusRegisterMapping(w http.ResponseWriter, r *http.Request) {
	mappingID := strings.TrimSpace(r.PathValue("id"))
	if mappingID == "" {
		writeError(w, http.StatusBadRequest, "invalid_argument", "缺少映射 ID")
		return
	}
	var request model.ModbusRegisterMappingRequest
	if !decodeStrictJSON(w, r, &request) {
		return
	}
	if message := validateModbusMappingRequest(request); message != "" {
		writeError(w, http.StatusBadRequest, "invalid_argument", message)
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.UpdateModbusRegisterMapping(ctx, mappingID, request)
	writeModbusResult(w, result, err)
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
	writeModbusResult(w, map[string]bool{"deleted": err == nil}, err)
}

// decodeStrictJSON 解码严格JSON。
func decodeStrictJSON(w http.ResponseWriter, r *http.Request, output any) bool {
	return decodeJSONRequest(w, r, output, maxModbusRequestBodyBytes)
}

// validateModbusServerSettings 校验Modbus服务设置。
func validateModbusServerSettings(value model.ModbusServerSettings) string {
	address := strings.TrimSpace(value.ListenAddress)
	ip := net.ParseIP(address)
	if address == "" || len(address) > 255 || ip == nil || ip.To4() == nil || strings.Contains(address, ":") {
		return "监听地址必须是有效 IPv4 地址"
	}
	if value.ListenPort < 1 || value.ListenPort > 65535 {
		return "监听端口必须在 1～65535 之间"
	}
	if value.UnitID < 0 || value.UnitID > 255 {
		return "Unit ID 必须在 0～255 之间"
	}
	if value.MaxClients < 1 || value.MaxClients > 64 {
		return "最大客户端数必须在 1～64 之间"
	}
	if value.IdleTimeoutSeconds < 5 || value.IdleTimeoutSeconds > 3600 {
		return "客户端空闲超时必须在 5～3600 秒之间"
	}
	if value.MaxReadRegisters < 1 || value.MaxReadRegisters > 125 {
		return "单次最大读取寄存器数必须在 1～125 之间"
	}
	return ""
}

// validateModbusMappingRequest 校验Modbus映射请求。
func validateModbusMappingRequest(value model.ModbusRegisterMappingRequest) string {
	if strings.TrimSpace(value.DeviceID) == "" || strings.TrimSpace(value.PointKey) == "" {
		return "设备和数据项不能为空"
	}
	if len(value.DeviceID) > 256 || len(value.PointKey) > 128 ||
		len(value.DeviceNameSnapshot) > 256 || len(value.PointNameSnapshot) > 256 {
		return "设备、数据项或名称快照长度超过限制"
	}
	counts := map[string]int{"uint16": 1, "int16": 1, "uint32": 2, "int32": 2, "float32": 2}
	count, ok := counts[value.DataType]
	if !ok {
		return "数据类型不受支持"
	}
	if value.StartAddress < 0 || value.StartAddress > 65535 || value.StartAddress+count-1 > 65535 {
		return "数据寄存器地址范围超过 0～65535"
	}
	if value.QualityAddress < 0 || value.QualityAddress > 65535 {
		return "质量寄存器地址必须在 0～65535 之间"
	}
	if value.QualityAddress >= value.StartAddress && value.QualityAddress < value.StartAddress+count {
		return "质量寄存器不能与本映射的数据寄存器重叠"
	}
	if !isFinite(value.ValueMultiplier) || value.ValueMultiplier == 0 {
		return "倍率必须是非零有限数"
	}
	if !isFinite(value.ValueOffset) {
		return "偏移必须是有限数"
	}
	if value.ByteOrder != "big_endian" && value.ByteOrder != "little_endian" {
		return "字节序不受支持"
	}
	if value.WordOrder != "high_word_first" && value.WordOrder != "low_word_first" {
		return "字序不受支持"
	}
	return ""
}

// isFinite 判断是否为有限数。
func isFinite(value float64) bool { return !math.IsNaN(value) && !math.IsInf(value, 0) }

// writeModbusResult 写入Modbus结果。
func writeModbusResult(w http.ResponseWriter, data any, err error) {
	if err == nil {
		writeResult(w, data, nil)
		return
	}
	status, code := http.StatusBadGateway, "backend_error"
	message := err.Error()
	var callError *ipc.CallError
	if errors.As(err, &callError) {
		message = callError.Message
		switch callError.Code {
		case "invalid_argument":
			status, code = http.StatusBadRequest, callError.Code
			if strings.Contains(callError.Message, "冲突") {
				status = http.StatusConflict
			}
		case "not_found":
			status, code = http.StatusNotFound, callError.Code
		case "invalid_state", "io_error", "timeout":
			status, code = http.StatusServiceUnavailable, callError.Code
		default:
			code = callError.Code
		}
	}
	writeError(w, status, code, message)
}
