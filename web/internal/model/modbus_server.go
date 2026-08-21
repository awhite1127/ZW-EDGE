package model

// Modbus 北向管理 DTO。配置请求使用 int，避免 JSON 绑定时发生窄整数回绕。
type ModbusServerSettings struct {
	Enabled            bool   `json:"enabled"`
	ListenAddress      string `json:"listen_address"`
	ListenPort         int    `json:"listen_port"`
	UnitID             int    `json:"unit_id"`
	StrictUnitID       bool   `json:"strict_unit_id"`
	MaxClients         int    `json:"max_clients"`
	IdleTimeoutSeconds int    `json:"idle_timeout_seconds"`
	MaxReadRegisters   int    `json:"max_read_registers"`
}

type ModbusServerRuntimeStatus struct {
	ConfiguredEnabled        bool   `json:"configured_enabled"`
	Running                  bool   `json:"running"`
	Listening                bool   `json:"listening"`
	State                    string `json:"state"`
	ListenAddress            string `json:"listen_address"`
	ListenPort               int    `json:"listen_port"`
	UnitID                   int    `json:"unit_id"`
	CurrentConnections       uint64 `json:"current_connections"`
	TotalConnections         uint64 `json:"total_connections"`
	TotalRequests            uint64 `json:"total_requests"`
	SuccessfulRequests       uint64 `json:"successful_requests"`
	ExceptionResponses       uint64 `json:"exception_responses"`
	UnsupportedFunctionCount uint64 `json:"unsupported_function_count"`
	InvalidAddressCount      uint64 `json:"invalid_address_count"`
	InvalidValueCount        uint64 `json:"invalid_value_count"`
	MalformedRequestCount    uint64 `json:"malformed_request_count"`
	RejectedConnectionCount  uint64 `json:"rejected_connection_count"`
	StartedAtMS              uint64 `json:"started_at_ms"`
	LastRequestTimeMS        uint64 `json:"last_request_time_ms"`
	LastClientIP             string `json:"last_client_ip"`
	LastErrorMessage         string `json:"last_error_message"`
}

type ModbusRegisterMapping struct {
	MappingID          string  `json:"mapping_id"`
	DeviceID           string  `json:"device_id"`
	PointKey           string  `json:"point_key"`
	DeviceNameSnapshot string  `json:"device_name_snapshot"`
	PointNameSnapshot  string  `json:"point_name_snapshot"`
	StartAddress       int     `json:"start_address"`
	DataType           string  `json:"data_type"`
	ValueMultiplier    float64 `json:"value_multiplier"`
	ValueOffset        float64 `json:"value_offset"`
	ByteOrder          string  `json:"byte_order"`
	WordOrder          string  `json:"word_order"`
	QualityAddress     int     `json:"quality_address"`
	Enabled            bool    `json:"enabled"`
	CreatedAtMS        uint64  `json:"created_at_ms"`
	UpdatedAtMS        uint64  `json:"updated_at_ms"`
}

type ModbusRegisterMappingRequest struct {
	DeviceID           string  `json:"device_id"`
	PointKey           string  `json:"point_key"`
	DeviceNameSnapshot string  `json:"device_name_snapshot"`
	PointNameSnapshot  string  `json:"point_name_snapshot"`
	StartAddress       int     `json:"start_address"`
	DataType           string  `json:"data_type"`
	ValueMultiplier    float64 `json:"value_multiplier"`
	ValueOffset        float64 `json:"value_offset"`
	ByteOrder          string  `json:"byte_order"`
	WordOrder          string  `json:"word_order"`
	QualityAddress     int     `json:"quality_address"`
	Enabled            bool    `json:"enabled"`
}

type ModbusExportablePoint struct {
	DeviceID       string `json:"device_id"`
	DeviceName     string `json:"device_name"`
	DeviceTypeID   string `json:"device_type_id"`
	DeviceTypeName string `json:"device_type_name"`
	PointKey       string `json:"point_key"`
	PointName      string `json:"point_name"`
	Unit           string `json:"unit"`
	Summary        bool   `json:"summary"`
}

type ModbusServerPageSnapshot struct {
	Settings      ModbusServerSettings      `json:"settings"`
	RuntimeStatus ModbusServerRuntimeStatus `json:"runtime_status"`
	Mappings      []ModbusRegisterMapping   `json:"mappings"`
}
