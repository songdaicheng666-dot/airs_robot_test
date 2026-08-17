#pragma once

#include <map>
#include <string>

namespace robot_adapter_interfaces {

enum class SwitchState {
    kDisconnected = 0,
    kConnecting = 1,
    kConnected = 2,
    kError = 3,
    kDisconnecting = 4,
};

enum class ErrorCode {
    kNone = 0,
    kUnknownRobot = 1,
    kTargetUnavailable = 2,
    kBusy = 3,
    kConnectFailed = 4,
    kDisconnectFailed = 5,
    kPreconditionRequired = 6,
};

struct AdapterStatus {
    bool registered{false};
    bool reachable{false};
    bool available{false};
    std::string detail;
};

struct OperationResult {
    bool success{false};
    ErrorCode code{ErrorCode::kNone};
    std::string message;
    std::string detail;
};

struct SwitchStatusSnapshot {
    SwitchState state{SwitchState::kDisconnected};
    std::string active_robot;
    bool busy{false};
    ErrorCode last_code{ErrorCode::kNone};
    std::string last_message;
    std::string last_detail;
    std::map<std::string, AdapterStatus> adapters;
};

std::string ToString(SwitchState value);
std::string ToString(ErrorCode value);

}  // namespace robot_adapter_interfaces
