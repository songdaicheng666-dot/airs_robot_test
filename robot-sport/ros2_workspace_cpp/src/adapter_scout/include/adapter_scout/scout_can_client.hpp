#pragma once

#include <mutex>
#include <string>

#include "adapter_scout/scout_command_codec.hpp"

namespace adapter_scout {

class ScoutCanClient {
public:
    ScoutCanClient() = default;
    ~ScoutCanClient();

    ScoutCanClient(const ScoutCanClient&) = delete;
    ScoutCanClient& operator=(const ScoutCanClient&) = delete;

    bool Connect(
        const std::string& interface_name,
        std::string* error = nullptr);
    void Disconnect() noexcept;

    [[nodiscard]] bool IsConnected() const;
    [[nodiscard]] std::string interface_name() const;
    [[nodiscard]] bool Send(
        const ScoutCanFrame& frame,
        std::string* error = nullptr);

    [[nodiscard]] static bool CheckInterfaceUp(
        const std::string& interface_name,
        std::string* error = nullptr);

private:
    void CloseLocked() noexcept;

    mutable std::mutex mutex_;
    int socket_fd_{-1};
    std::string interface_name_;
};

}  // namespace adapter_scout
