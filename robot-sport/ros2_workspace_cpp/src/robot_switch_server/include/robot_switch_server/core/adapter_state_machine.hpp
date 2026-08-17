#pragma once

#include <string>

namespace robot_switch_server {

enum class AdapterState {
    kIdle,
    kStarting,
    kRunning,
    kStopping,
    kFaulted,
    kShuttingDown,
};

enum class AdapterEvent {
    kStartRequested,
    kStartSucceeded,
    kStartFailed,
    kStopRequested,
    kStopSucceeded,
    kChildExitedUnexpectedly,
    kShutdownRequested,
};

class AdapterStateMachine {
public:
    AdapterStateMachine() = default;

    bool ProcessEvent(AdapterEvent event);

    AdapterState state() const { return state_; }

    static std::string ToString(AdapterState state);
    static std::string ToString(AdapterEvent event);

private:
    AdapterState state_{AdapterState::kIdle};
};

}  // namespace robot_switch_server
