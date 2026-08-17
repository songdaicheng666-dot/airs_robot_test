#include "robot_adapter_interfaces/types.hpp"

namespace robot_adapter_interfaces {

std::string ToString(SwitchState value) {
    switch (value) {
    case SwitchState::kDisconnected:
        return "DISCONNECTED";
    case SwitchState::kConnecting:
        return "CONNECTING";
    case SwitchState::kConnected:
        return "CONNECTED";
    case SwitchState::kDisconnecting:
        return "DISCONNECTING";
    case SwitchState::kError:
        return "ERROR";
    default:
        return "ERROR";
    }
}

std::string ToString(ErrorCode value) {
    switch (value) {
    case ErrorCode::kNone:
        return "NONE";
    case ErrorCode::kUnknownRobot:
        return "UNKNOWN_ROBOT";
    case ErrorCode::kTargetUnavailable:
        return "TARGET_UNAVAILABLE";
    case ErrorCode::kBusy:
        return "BUSY";
    case ErrorCode::kConnectFailed:
        return "CONNECT_FAILED";
    case ErrorCode::kDisconnectFailed:
        return "DISCONNECT_FAILED";
    case ErrorCode::kPreconditionRequired:
        return "PRECONDITION_REQUIRED";
    default:
        return "UNKNOWN";
    }
}

}  // namespace robot_adapter_interfaces
