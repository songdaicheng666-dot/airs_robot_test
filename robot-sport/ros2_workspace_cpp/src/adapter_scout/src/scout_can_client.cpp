#include "adapter_scout/scout_can_client.hpp"

#include <cerrno>
#include <cstring>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace adapter_scout {

namespace {

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::string ErrnoMessage(const std::string& operation, int error_number) {
    return operation + ": " + std::strerror(error_number);
}

bool ValidateInterfaceName(
    const std::string& interface_name,
    std::string* error) {
    if (interface_name.empty()) {
        SetError(error, "CAN interface name must not be empty");
        return false;
    }
    if (interface_name.size() >= IFNAMSIZ) {
        SetError(error, "CAN interface name is too long");
        return false;
    }
    return true;
}

void FillInterfaceRequest(const std::string& interface_name, ifreq* request) {
    std::memset(request, 0, sizeof(*request));
    std::memcpy(
        request->ifr_name, interface_name.data(), interface_name.size());
    request->ifr_name[interface_name.size()] = '\0';
}

bool QueryInterfaceUp(
    int socket_fd,
    const std::string& interface_name,
    std::string* error) {
    ifreq request{};
    FillInterfaceRequest(interface_name, &request);
    if (ioctl(socket_fd, SIOCGIFFLAGS, &request) < 0) {
        SetError(
            error,
            ErrnoMessage(
                "failed to query CAN interface '" + interface_name + "'",
                errno));
        return false;
    }
    if ((request.ifr_flags & IFF_UP) == 0) {
        SetError(error, "CAN interface '" + interface_name + "' is down");
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

}  // namespace

ScoutCanClient::~ScoutCanClient() {
    Disconnect();
}

bool ScoutCanClient::Connect(
    const std::string& interface_name,
    std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!ValidateInterfaceName(interface_name, error)) {
        return false;
    }
    if (socket_fd_ >= 0) {
        if (interface_name_ == interface_name) {
            if (error != nullptr) {
                error->clear();
            }
            return true;
        }
        SetError(
            error,
            "SocketCAN client is already connected to '" + interface_name_ +
                "'");
        return false;
    }

    interface_name_ = interface_name;
    const int socket_fd = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (socket_fd < 0) {
        SetError(error, ErrnoMessage("failed to create raw CAN socket", errno));
        return false;
    }

    ifreq request{};
    FillInterfaceRequest(interface_name, &request);
    if (ioctl(socket_fd, SIOCGIFINDEX, &request) < 0) {
        const int error_number = errno;
        close(socket_fd);
        SetError(
            error,
            ErrnoMessage(
                "failed to resolve CAN interface '" + interface_name + "'",
                error_number));
        return false;
    }
    const int interface_index = request.ifr_ifindex;

    if (!QueryInterfaceUp(socket_fd, interface_name, error)) {
        close(socket_fd);
        return false;
    }

    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = interface_index;
    if (bind(
            socket_fd,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) < 0) {
        const int error_number = errno;
        close(socket_fd);
        SetError(
            error,
            ErrnoMessage(
                "failed to bind CAN interface '" + interface_name + "'",
                error_number));
        return false;
    }

    socket_fd_ = socket_fd;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void ScoutCanClient::Disconnect() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
}

bool ScoutCanClient::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return socket_fd_ >= 0;
}

std::string ScoutCanClient::interface_name() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return interface_name_;
}

bool ScoutCanClient::Send(
    const ScoutCanFrame& frame,
    std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (socket_fd_ < 0) {
        SetError(error, "SocketCAN client is not connected");
        return false;
    }
    if (frame.can_id > CAN_SFF_MASK) {
        SetError(error, "Scout frame CAN ID must be a standard 11-bit ID");
        return false;
    }
    if (frame.can_dlc > CAN_MAX_DLEN) {
        SetError(error, "Scout frame payload exceeds 8 bytes");
        return false;
    }

    can_frame native_frame{};
    native_frame.can_id = frame.can_id;
    native_frame.can_dlc = frame.can_dlc;
    std::memcpy(
        native_frame.data, frame.data.data(), native_frame.can_dlc);

    ssize_t bytes_written = -1;
    do {
        bytes_written = write(socket_fd_, &native_frame, sizeof(native_frame));
    } while (bytes_written < 0 && errno == EINTR);

    if (bytes_written < 0) {
        SetError(error, ErrnoMessage("failed to write CAN frame", errno));
        return false;
    }
    if (bytes_written != static_cast<ssize_t>(sizeof(native_frame))) {
        SetError(error, "incomplete CAN frame write");
        return false;
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool ScoutCanClient::CheckInterfaceUp(
    const std::string& interface_name,
    std::string* error) {
    if (!ValidateInterfaceName(interface_name, error)) {
        return false;
    }

    const int socket_fd = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (socket_fd < 0) {
        SetError(error, ErrnoMessage("failed to create raw CAN socket", errno));
        return false;
    }
    const bool interface_up = QueryInterfaceUp(socket_fd, interface_name, error);
    close(socket_fd);
    return interface_up;
}

void ScoutCanClient::CloseLocked() noexcept {
    if (socket_fd_ < 0) {
        return;
    }
    const int socket_fd = socket_fd_;
    socket_fd_ = -1;
    close(socket_fd);
}

}  // namespace adapter_scout
