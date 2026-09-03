#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "shared/common/filesystem_compat.h"
#include "data/datastore/config_store.h"
#include "data/datastore/database_paths.h"
#include "data/model/modbus_server.h"
#include "application/service/backend_service.h"

namespace {

using edge_controller::BackendService;
using edge_controller::ConfigStore;
using edge_controller::DatabasePaths;
using edge_controller::ModbusServerPageSnapshot;
using edge_controller::ModbusServerSettings;
using edge_controller::StatusCode;
using edge_controller::is_ok;

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd()
    {
        if (fd_ >= 0) ::close(fd_);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}

    ScopedFd& operator=(ScopedFd&& other) noexcept
    {
        if (this == &other) return *this;
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.release();
        return *this;
    }

    int get() const { return fd_; }

    int release()
    {
        const int result = fd_;
        fd_ = -1;
        return result;
    }

private:
    int fd_{-1};
};

class TempDirectory {
public:
    TempDirectory()
    {
        char path_template[] = "/tmp/edge-controller-modbus-test-XXXXXX";
        const auto* created = ::mkdtemp(path_template);
        if (created != nullptr) path_ = created;
    }

    ~TempDirectory()
    {
        if (path_.empty()) return;
        std::error_code ignored;
        edge::fs::remove_all(path_, ignored);
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

bool same_settings(const ModbusServerSettings& left, const ModbusServerSettings& right)
{
    return left.enabled == right.enabled &&
           left.listen_address == right.listen_address &&
           left.listen_port == right.listen_port &&
           left.unit_id == right.unit_id &&
           left.strict_unit_id == right.strict_unit_id &&
           left.max_clients == right.max_clients &&
           left.idle_timeout_seconds == right.idle_timeout_seconds &&
           left.max_read_registers == right.max_read_registers;
}

int reserve_loopback_listener(std::uint16_t* port, std::string* error)
{
    if (port == nullptr) return -1;
    *port = 0;
    ScopedFd socket_fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (socket_fd.get() < 0) {
        if (error != nullptr) *error = "socket failed: " + std::string(std::strerror(errno));
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(
            socket_fd.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0) {
        if (error != nullptr) *error = "bind failed: " + std::string(std::strerror(errno));
        return -1;
    }
    if (::listen(socket_fd.get(), 1) != 0) {
        if (error != nullptr) *error = "listen failed: " + std::string(std::strerror(errno));
        return -1;
    }
    socklen_t address_size = sizeof(address);
    if (::getsockname(
            socket_fd.get(),
            reinterpret_cast<sockaddr*>(&address),
            &address_size) != 0) {
        if (error != nullptr) *error = "getsockname failed: " + std::string(std::strerror(errno));
        return -1;
    }
    *port = ntohs(address.sin_port);
    return socket_fd.release();
}

std::uint16_t choose_available_loopback_port(std::string* error)
{
    std::uint16_t port = 0;
    ScopedFd reservation(reserve_loopback_listener(&port, error));
    return reservation.get() < 0 ? 0 : port;
}

bool connect_loopback(std::uint16_t port, ScopedFd* connection, std::string* error)
{
    if (connection == nullptr) return false;
    ScopedFd socket_fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (socket_fd.get() < 0) {
        if (error != nullptr) *error = "client socket failed: " + std::string(std::strerror(errno));
        return false;
    }
    timeval timeout{};
    timeout.tv_sec = 2;
    if (::setsockopt(
            socket_fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        if (error != nullptr) *error = "SO_RCVTIMEO failed: " + std::string(std::strerror(errno));
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(
            socket_fd.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0) {
        if (error != nullptr) *error = "connect failed: " + std::string(std::strerror(errno));
        return false;
    }
    *connection = ScopedFd(socket_fd.release());
    return true;
}

bool send_all(int fd, const std::uint8_t* data, std::size_t size, std::string* error)
{
    std::size_t sent = 0;
    while (sent < size) {
        const auto count = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (error != nullptr) *error = "send failed: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

bool verify_half_close_response(std::uint16_t port, std::string* error)
{
    ScopedFd connection;
    if (!connect_loopback(port, &connection, error)) return false;

    const std::array<std::uint8_t, 12> request{
        0x12, 0x34, 0x00, 0x00, 0x00, 0x06,
        0x01, 0x03, 0x00, 0x00, 0x00, 0x01,
    };
    if (!send_all(connection.get(), request.data(), request.size(), error)) return false;
    if (::shutdown(connection.get(), SHUT_WR) != 0) {
        if (error != nullptr) *error = "shutdown(SHUT_WR) failed: " + std::string(std::strerror(errno));
        return false;
    }

    std::vector<std::uint8_t> response;
    std::array<std::uint8_t, 32> buffer{};
    while (response.size() < 9) {
        const auto count = ::recv(connection.get(), buffer.data(), buffer.size(), 0);
        if (count > 0) {
            response.insert(
                response.end(),
                buffer.begin(),
                buffer.begin() + static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (error != nullptr) {
            *error = count == 0
                ? "server closed before sending a complete response"
                : "recv failed: " + std::string(std::strerror(errno));
        }
        return false;
    }

    const std::array<std::uint8_t, 9> expected{
        0x12, 0x34, 0x00, 0x00, 0x00, 0x03, 0x01, 0x83, 0x02,
    };
    if (!std::equal(expected.begin(), expected.end(), response.begin())) {
        if (error != nullptr) *error = "unexpected Modbus response after half-close";
        return false;
    }
    return true;
}

int fail(const std::string& message)
{
    std::cerr << "modbus integration test failed: " << message << '\n';
    return 1;
}

}  // namespace

int main()
{
    TempDirectory data_directory;
    if (data_directory.path().empty()) return fail("mkdtemp failed");

    BackendService service;
    const auto initialize_status = service.initialize(data_directory.path());
    if (!is_ok(initialize_status)) {
        return fail("BackendService initialize returned " +
                    std::string(edge_controller::to_string(initialize_status)));
    }

    std::string error;
    ModbusServerSettings working_settings;
    working_settings.enabled = true;
    working_settings.listen_address = "127.0.0.1";
    auto working_status = StatusCode::kInvalidState;
    for (int attempt = 0; attempt < 3 && !is_ok(working_status); ++attempt) {
        error.clear();
        working_settings.listen_port = choose_available_loopback_port(&error);
        if (working_settings.listen_port == 0) continue;
        working_status = service.apply_modbus_server_settings(working_settings, &error);
    }
    if (!is_ok(working_status)) return fail("could not start working listener: " + error);
    if (!verify_half_close_response(working_settings.listen_port, &error)) return fail(error);

    std::uint16_t occupied_port = 0;
    ScopedFd occupied_listener(reserve_loopback_listener(&occupied_port, &error));
    if (occupied_listener.get() < 0) return fail(error);

    auto rejected_settings = working_settings;
    rejected_settings.listen_port = occupied_port;
    error.clear();
    const auto rejected_status = service.apply_modbus_server_settings(rejected_settings, &error);
    if (is_ok(rejected_status)) return fail("occupied listen port was unexpectedly accepted");
    if (error.find("已恢复原持久化配置和运行状态") == std::string::npos) {
        return fail("rollback result was not reported: " + error);
    }

    ModbusServerPageSnapshot snapshot;
    error.clear();
    const auto snapshot_status = service.get_modbus_server_page_snapshot(&snapshot, &error);
    if (!is_ok(snapshot_status)) return fail("could not read restored snapshot: " + error);
    if (!same_settings(snapshot.settings, working_settings)) {
        return fail("in-memory Modbus settings were not restored");
    }
    if (!snapshot.runtime_status.running || !snapshot.runtime_status.listening ||
        snapshot.runtime_status.listen_port != working_settings.listen_port) {
        return fail("working Modbus listener was not restarted after rollback");
    }
    if (!verify_half_close_response(working_settings.listen_port, &error)) {
        return fail("restored listener did not answer: " + error);
    }

    service.shutdown();

    ConfigStore persisted_reader;
    const DatabasePaths paths(data_directory.path());
    error.clear();
    const auto persisted_initialize_status = persisted_reader.initialize(
        paths.config_database(), &error);
    if (!is_ok(persisted_initialize_status)) {
        return fail("could not open persisted settings: " + error);
    }
    ModbusServerSettings persisted_settings;
    const auto persisted_status = persisted_reader.load_modbus_server_settings(
        &persisted_settings, &error);
    if (!is_ok(persisted_status)) return fail("could not load persisted settings: " + error);
    if (!same_settings(persisted_settings, working_settings)) {
        return fail("SQLite Modbus settings were not restored");
    }

    std::cout << "modbus integration test passed\n";
    return 0;
}
