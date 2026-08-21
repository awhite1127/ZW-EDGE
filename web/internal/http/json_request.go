package httpserver

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
)

const (
	maxStandardJSONRequestBodyBytes int64 = 64 << 10
	maxBatchJSONRequestBodyBytes    int64 = 512 << 10
	maxDeviceTemplateBodyBytes      int64 = 512 << 10
)

// decodeJSONRequest 统一限制产品 JSON API 的请求体，并拒绝未知字段及尾随内容。
func decodeJSONRequest(w http.ResponseWriter, r *http.Request, output any, maxBytes int64) bool {
	if maxBytes <= 0 {
		maxBytes = maxStandardJSONRequestBodyBytes
	}
	r.Body = http.MaxBytesReader(w, r.Body, maxBytes)
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()

	if err := decoder.Decode(output); err != nil {
		writeJSONDecodeError(w, err, maxBytes)
		return false
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeError(w, http.StatusBadRequest, "invalid_request", "请求体只能包含一个 JSON 对象，不能带有尾随内容")
		return false
	}
	return true
}

// writeJSONDecodeError 写入JSON解码错误。
func writeJSONDecodeError(w http.ResponseWriter, err error, maxBytes int64) {
	var maxBytesError *http.MaxBytesError
	var syntaxError *json.SyntaxError
	var typeError *json.UnmarshalTypeError
	switch {
	case errors.Is(err, io.EOF):
		writeError(w, http.StatusBadRequest, "invalid_request", "请求体不能为空，必须提供一个 JSON 对象")
	case errors.As(err, &maxBytesError):
		writeError(w, http.StatusRequestEntityTooLarge, "request_too_large", fmt.Sprintf("请求体过大，不能超过 %d KiB", maxBytes>>10))
	case strings.HasPrefix(err.Error(), "json: unknown field "):
		field := strings.Trim(strings.TrimPrefix(err.Error(), "json: unknown field "), "\"")
		writeError(w, http.StatusBadRequest, "unknown_field", "请求体格式不正确：包含未知字段 "+field)
	case errors.As(err, &syntaxError):
		writeError(w, http.StatusBadRequest, "invalid_json", "请求体不是合法 JSON")
	case errors.As(err, &typeError):
		field := strings.TrimSpace(typeError.Field)
		if field == "" {
			writeError(w, http.StatusBadRequest, "invalid_json", "请求体字段类型不正确")
		} else {
			writeError(w, http.StatusBadRequest, "invalid_json", "请求体格式不正确：字段 "+field+" 类型不正确")
		}
	default:
		writeError(w, http.StatusBadRequest, "invalid_json", "请求体格式不正确")
	}
}
