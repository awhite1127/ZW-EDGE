cmake_minimum_required(VERSION 3.16)

set(PACKAGE_VERSION "" CACHE STRING "Explicit edge-controller product version override")
if("${PACKAGE_VERSION}" STREQUAL "")
    set(_edge_version_file "${CMAKE_CURRENT_LIST_DIR}/../VERSION")
    if(NOT EXISTS "${_edge_version_file}")
        message(FATAL_ERROR "Product version file does not exist: ${_edge_version_file}")
    endif()
    file(READ "${_edge_version_file}" _edge_version_file_contents)
    string(LENGTH "${_edge_version_file_contents}" _edge_version_file_length)
    set(_edge_resolved_package_version "${_edge_version_file_contents}")
    if(_edge_version_file_length GREATER 0)
        math(EXPR _edge_version_last_index "${_edge_version_file_length} - 1")
        string(SUBSTRING "${_edge_version_file_contents}" ${_edge_version_last_index} 1 _edge_version_last_character)
        if(_edge_version_last_character STREQUAL "\n")
            string(SUBSTRING "${_edge_version_file_contents}" 0 ${_edge_version_last_index} _edge_resolved_package_version)
        endif()
    endif()
    set(_edge_package_version_source "${_edge_version_file}")
else()
    set(_edge_resolved_package_version "${PACKAGE_VERSION}")
    set(_edge_package_version_source "explicit PACKAGE_VERSION")
endif()

string(FIND "${_edge_resolved_package_version}" "\n" _edge_version_newline_index)
string(FIND "${_edge_resolved_package_version}" "\r" _edge_version_carriage_return_index)
if(NOT _edge_version_newline_index EQUAL -1 OR NOT _edge_version_carriage_return_index EQUAL -1)
    message(FATAL_ERROR "${_edge_package_version_source} must contain exactly one plain SemVer line")
endif()

# SemVer 2.0.0: numeric identifiers reject leading zeroes; prerelease and build identifiers are optional.
set(_edge_semver_numeric_identifier "(0|[1-9][0-9]*)")
set(_edge_semver_prerelease_identifier "(0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)")
set(_edge_semver_pattern
    "^${_edge_semver_numeric_identifier}\\.${_edge_semver_numeric_identifier}\\.${_edge_semver_numeric_identifier}(-${_edge_semver_prerelease_identifier}(\\.${_edge_semver_prerelease_identifier})*)?(\\+[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$"
)
if(NOT "${_edge_resolved_package_version}" MATCHES "${_edge_semver_pattern}")
    message(FATAL_ERROR
        "${_edge_package_version_source} must contain exactly one SemVer value "
        "(for example 0.9.0 or 0.9.1-rc.1+build.4): '${_edge_resolved_package_version}'"
    )
endif()

string(REGEX REPLACE
    "^([0-9]+\\.[0-9]+\\.[0-9]+).*$" "\\1"
    _edge_cmake_project_version "${_edge_resolved_package_version}"
)
set(EDGE_CONTROLLER_PACKAGE_VERSION "${_edge_resolved_package_version}"
    CACHE INTERNAL "Validated edge-controller product version" FORCE)

# 后端构建分为可复用核心静态库和最终守护进程。
project(edge-controller VERSION "${_edge_cmake_project_version}" LANGUAGES C CXX)
message(STATUS
    "edge-controller product version: ${EDGE_CONTROLLER_PACKAGE_VERSION} "
    "(source: ${_edge_package_version_source})"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    include(CheckIPOSupported)
    check_ipo_supported(
        RESULT EDGE_CONTROLLER_IPO_SUPPORTED
        OUTPUT EDGE_CONTROLLER_IPO_ERROR
        LANGUAGES C CXX
    )
    if(NOT EDGE_CONTROLLER_IPO_SUPPORTED)
        message(FATAL_ERROR "IPO/LTO was requested but is not supported: ${EDGE_CONTROLLER_IPO_ERROR}")
    endif()
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# 第一方源文件集中链接到核心库，便于主程序复用一致的实现。
add_library(edge_controller_core STATIC
    third_party/sqlite/sqlite3.c
    src/application/app/application.cpp
    src/shared/common/readable_error.cpp
    src/communication/channel/bounded_address_resolver.cpp
    src/communication/channel/channel_wake_event.cpp
    src/communication/channel/serial_channel.cpp
    src/communication/channel/tcp_channel.cpp
    src/communication/collect/block_read_plan.cpp
    src/communication/collect/master_collector.cpp
    src/communication/collect/modbus_trace_recorder.cpp
    src/communication/collect/modbus_write_executor.cpp
    src/communication/collect/register_mapper.cpp
    src/data/datastore/communication_store.cpp
    src/data/datastore/alarm_store.cpp
    src/data/datastore/config_import_transaction.cpp
    src/data/datastore/config_store.cpp
    src/data/datastore/config_store_schema.cpp
    src/data/datastore/config_store_system.cpp
    src/data/datastore/config_store_time.cpp
    src/data/datastore/config_store_collection.cpp
    src/data/datastore/config_store_network.cpp
    src/data/datastore/config_store_mqtt.cpp
    src/data/datastore/config_store_modbus_server.cpp
    src/data/datastore/config_store_web_auth.cpp
    src/data/datastore/config_store_sql.cpp
    src/data/datastore/data_store.cpp
    src/data/datastore/device_template_store.cpp
    src/data/datastore/history_store.cpp
    src/data/datastore/event_store.cpp
    src/application/interface/ipc_json.cpp
    src/application/interface/ipc_protocol.cpp
    src/application/interface/ipc_server.cpp
    src/application/interface/ipc_handlers_system.cpp
    src/application/interface/ipc_handlers_events.cpp
    src/application/interface/ipc_handlers_alarms.cpp
    src/application/interface/ipc_handlers_settings.cpp
    src/application/interface/ipc_handlers_modbus_server.cpp
    src/application/interface/ipc_handlers_collection.cpp
    src/application/interface/ipc_handlers_history.cpp
    src/application/interface/ipc_handlers_templates.cpp
    src/application/interface/ipc_handlers_device_command.cpp
    src/application/manager/channel_manager.cpp
    src/application/manager/topology_manager.cpp
    src/data/model/diagnosis_status.cpp
    src/data/model/device_value_status.cpp
    src/data/model/alarm_validation.cpp
    src/data/model/device_template.cpp
    src/data/model/builtin_device_templates.cpp
    src/data/model/builtin_wired_devices.cpp
    src/data/model/builtin_wireless_devices.cpp
    src/data/model/modbus_server.cpp
    src/communication/modbus_server/modbus_register_encoder.cpp
    src/communication/modbus_server/modbus_register_bank.cpp
    src/communication/modbus_server/modbus_export_service.cpp
    src/communication/modbus_server/modbus_tcp_server.cpp
    src/infrastructure/platform/linux_time_runtime.cpp
    src/communication/mqtt/mqtt_client_mosquitto.cpp
    src/communication/mqtt/mqtt_payload_builder.cpp
    src/communication/mqtt/mqtt_publisher_service.cpp
    src/communication/protocol/modbus_rtu_protocol.cpp
    src/communication/protocol/modbus_tcp_client.cpp
    src/communication/protocol/modbus_tcp_protocol.cpp
    src/infrastructure/security/password_hash.cpp
    src/application/service/backend_lifecycle.cpp
    src/application/service/backend_polling_control.cpp
    src/application/service/backend_runtime_config.cpp
    src/application/service/backend_alarm.cpp
    src/application/service/alarm_evaluator.cpp
    src/application/service/backend_channel_config.cpp
    src/application/service/backend_config_common.cpp
    src/application/service/backend_config_import_export.cpp
    src/application/service/backend_config_validation.cpp
    src/application/service/backend_master_config.cpp
    src/application/service/backend_device_command.cpp
    src/application/service/backend_mqtt_config.cpp
    src/application/service/backend_modbus_server.cpp
    src/application/service/backend_network_config.cpp
    src/application/service/backend_settings.cpp
    src/application/service/backend_system_overview.cpp
    src/application/service/backend_update.cpp
    src/application/service/backend_time_settings.cpp
    src/application/service/backend_time_change.cpp
    src/application/service/time_jump_detector.cpp
    src/application/service/backend_query_collection.cpp
    src/application/service/backend_query_events.cpp
    src/application/service/backend_query_history.cpp
    src/application/service/backend_query_templates.cpp
    src/application/service/backend_network_runtime.cpp
    src/application/service/polling_service.cpp
    src/application/service/polling_service_loop.cpp
    src/application/service/polling_service_collect.cpp
    src/application/service/polling_service_failure.cpp
    src/application/service/polling_service_history.cpp
    src/application/service/polling_service_status.cpp
    src/application/service/serial_port_enumerator.cpp
)

target_include_directories(edge_controller_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/third_party
    PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/sqlite
)

target_compile_features(edge_controller_core PUBLIC cxx_std_17)

target_compile_definitions(edge_controller_core
    PRIVATE
        EDGE_CONTROLLER_PACKAGE_VERSION="${EDGE_CONTROLLER_PACKAGE_VERSION}"
        SQLITE_OMIT_LOAD_EXTENSION
)

# libmosquitto 是正式产品的必需构建与运行依赖；缺少头文件或目标库时必须停止构建。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MOSQUITTO QUIET libmosquitto)
endif()
find_path(MOSQUITTO_INCLUDE_DIR
    NAMES mosquitto.h
    HINTS ${MOSQUITTO_INCLUDE_DIRS}
)
find_library(MOSQUITTO_LIBRARY
    NAMES mosquitto libmosquitto
    HINTS ${MOSQUITTO_LIBRARY_DIRS}
)
if(NOT MOSQUITTO_INCLUDE_DIR OR NOT MOSQUITTO_LIBRARY)
    message(FATAL_ERROR "edge-controller requires libmosquitto headers and library")
endif()
target_include_directories(edge_controller_core PRIVATE ${MOSQUITTO_INCLUDE_DIR})
target_link_libraries(edge_controller_core PUBLIC ${MOSQUITTO_LIBRARY})

target_link_libraries(edge_controller_core PUBLIC Threads::Threads)

add_executable(${PROJECT_NAME}
    main.cpp
    src/infrastructure/maintenance/admin_recovery.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

target_link_libraries(${PROJECT_NAME} PRIVATE edge_controller_core Threads::Threads)

# Release 产物按函数和数据分段，最终链接时移除静态核心库中未被产品入口使用的内容。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(edge_controller_core PRIVATE
        "$<$<CONFIG:Release>:-ffunction-sections>"
        "$<$<CONFIG:Release>:-fdata-sections>"
    )
    target_compile_options(${PROJECT_NAME} PRIVATE
        "$<$<CONFIG:Release>:-ffunction-sections>"
        "$<$<CONFIG:Release>:-fdata-sections>"
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        "$<$<CONFIG:Release>:-Wl,--gc-sections>"
    )
endif()

option(EDGE_CONTROLLER_BUILD_TESTS "Build edge-controller backend tests" OFF)
if(EDGE_CONTROLLER_BUILD_TESTS)
    enable_testing()
    add_executable(edge_controller_modbus_integration_test
        tests/modbus_server_integration_test.cpp
    )
    target_compile_features(edge_controller_modbus_integration_test PRIVATE cxx_std_17)
    target_link_libraries(edge_controller_modbus_integration_test
        PRIVATE edge_controller_core Threads::Threads
    )
    add_test(
        NAME edge_controller_modbus_integration
        COMMAND edge_controller_modbus_integration_test
    )
    set_tests_properties(edge_controller_modbus_integration PROPERTIES TIMEOUT 30)

    add_executable(edge_controller_config_import_atomicity_test
        tests/config_import_atomicity_test.cpp
    )
    target_compile_features(edge_controller_config_import_atomicity_test PRIVATE cxx_std_17)
    target_include_directories(edge_controller_config_import_atomicity_test
        PRIVATE ${PROJECT_SOURCE_DIR}/third_party/sqlite
    )
    target_link_libraries(edge_controller_config_import_atomicity_test
        PRIVATE edge_controller_core Threads::Threads
    )
    add_test(
        NAME edge_controller_config_import_atomicity
        COMMAND edge_controller_config_import_atomicity_test
    )
    set_tests_properties(edge_controller_config_import_atomicity PROPERTIES TIMEOUT 30)

    add_executable(edge_controller_event_store_query_test
        tests/event_store_query_test.cpp
    )
    target_compile_features(edge_controller_event_store_query_test PRIVATE cxx_std_17)
    target_link_libraries(edge_controller_event_store_query_test
        PRIVATE edge_controller_core Threads::Threads
    )
    add_test(
        NAME edge_controller_event_store_query
        COMMAND edge_controller_event_store_query_test
    )
    set_tests_properties(edge_controller_event_store_query PROPERTIES TIMEOUT 30)

    add_executable(edge_controller_data_store_realtime_snapshot_test
        tests/data_store_realtime_snapshot_test.cpp
    )
    target_compile_features(edge_controller_data_store_realtime_snapshot_test PRIVATE cxx_std_17)
    target_link_libraries(edge_controller_data_store_realtime_snapshot_test
        PRIVATE edge_controller_core Threads::Threads
    )
    add_test(
        NAME edge_controller_data_store_realtime_snapshot
        COMMAND edge_controller_data_store_realtime_snapshot_test
    )
    set_tests_properties(edge_controller_data_store_realtime_snapshot PROPERTIES TIMEOUT 30)

    add_executable(edge_controller_alarm_evaluator_batch_test
        tests/alarm_evaluator_batch_test.cpp
    )
    target_compile_features(edge_controller_alarm_evaluator_batch_test PRIVATE cxx_std_17)
    target_include_directories(edge_controller_alarm_evaluator_batch_test
        PRIVATE ${PROJECT_SOURCE_DIR}/third_party/sqlite
    )
    target_link_libraries(edge_controller_alarm_evaluator_batch_test
        PRIVATE edge_controller_core Threads::Threads
    )
    add_test(
        NAME edge_controller_alarm_evaluator_batch
        COMMAND edge_controller_alarm_evaluator_batch_test
    )
    set_tests_properties(edge_controller_alarm_evaluator_batch PROPERTIES TIMEOUT 30)

    add_executable(edge_controller_alarm_rule_validation_test
        tests/alarm_rule_validation_test.cpp
    )
    target_compile_features(edge_controller_alarm_rule_validation_test PRIVATE cxx_std_17)
    target_link_libraries(edge_controller_alarm_rule_validation_test
        PRIVATE edge_controller_core Threads::Threads
    )
    add_test(
        NAME edge_controller_alarm_rule_validation
        COMMAND edge_controller_alarm_rule_validation_test
    )
    set_tests_properties(edge_controller_alarm_rule_validation PROPERTIES TIMEOUT 30)

    add_executable(edge_controller_device_value_status_test
        tests/device_value_status_test.cpp
    )
    target_compile_features(edge_controller_device_value_status_test PRIVATE cxx_std_17)
    target_link_libraries(edge_controller_device_value_status_test
        PRIVATE edge_controller_core Threads::Threads
    )
    add_test(
        NAME edge_controller_device_value_status
        COMMAND edge_controller_device_value_status_test
    )
    set_tests_properties(edge_controller_device_value_status PROPERTIES TIMEOUT 30)

    add_executable(edge_controller_history_store_test
        tests/history_store_test.cpp
    )
    target_compile_features(edge_controller_history_store_test PRIVATE cxx_std_17)
    target_include_directories(edge_controller_history_store_test
        PRIVATE ${PROJECT_SOURCE_DIR}/third_party/sqlite
    )
    target_link_libraries(edge_controller_history_store_test
        PRIVATE edge_controller_core Threads::Threads
    )
    add_test(
        NAME edge_controller_history_store
        COMMAND edge_controller_history_store_test
    )
    set_tests_properties(edge_controller_history_store PROPERTIES TIMEOUT 30)

    add_executable(edge_controller_bounded_address_resolver_integration_test
        tests/bounded_address_resolver_integration_test.cpp
    )
    target_compile_features(edge_controller_bounded_address_resolver_integration_test PRIVATE cxx_std_17)
    target_link_libraries(edge_controller_bounded_address_resolver_integration_test
        PRIVATE edge_controller_core Threads::Threads
    )
    add_dependencies(edge_controller_bounded_address_resolver_integration_test ${PROJECT_NAME})
    add_test(
        NAME edge_controller_bounded_address_resolver_integration
        COMMAND ${CMAKE_COMMAND} -E env
            "EDGE_CONTROLLER_RESOLVER_HELPER=$<TARGET_FILE:${PROJECT_NAME}>"
            "$<TARGET_FILE:edge_controller_bounded_address_resolver_integration_test>"
    )
    set_tests_properties(edge_controller_bounded_address_resolver_integration PROPERTIES TIMEOUT 30)
endif()

set(edge_controller_targets edge_controller_core ${PROJECT_NAME})
if(EDGE_CONTROLLER_BUILD_TESTS)
    list(APPEND edge_controller_targets
        edge_controller_modbus_integration_test
        edge_controller_config_import_atomicity_test
        edge_controller_event_store_query_test
        edge_controller_data_store_realtime_snapshot_test
        edge_controller_alarm_evaluator_batch_test
        edge_controller_alarm_rule_validation_test
        edge_controller_device_value_status_test
        edge_controller_history_store_test
        edge_controller_bounded_address_resolver_integration_test
    )
endif()
foreach(target ${edge_controller_targets})
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    endif()
endforeach()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(edge_controller_core PUBLIC stdc++fs)
endif()
