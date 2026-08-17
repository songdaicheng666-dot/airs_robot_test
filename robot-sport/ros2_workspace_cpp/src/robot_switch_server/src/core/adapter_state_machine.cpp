#include "robot_switch_server/core/adapter_state_machine.hpp"

namespace robot_switch_server {

bool AdapterStateMachine::ProcessEvent(AdapterEvent event) {
    switch (state_) {
    case AdapterState::kIdle:
        if (event == AdapterEvent::kStartRequested) {
            state_ = AdapterState::kStarting;
            return true;
        }
        if (event == AdapterEvent::kShutdownRequested) {
            state_ = AdapterState::kShuttingDown;
            return true;
        }
        return false;

    case AdapterState::kStarting:
        if (event == AdapterEvent::kStartSucceeded) {
            state_ = AdapterState::kRunning;
            return true;
        }
        if (event == AdapterEvent::kStartFailed ||
            event == AdapterEvent::kChildExitedUnexpectedly) {
            state_ = AdapterState::kFaulted;
            return true;
        }
        if (event == AdapterEvent::kShutdownRequested) {
            state_ = AdapterState::kShuttingDown;
            return true;
        }
        return false;

    case AdapterState::kRunning:
        if (event == AdapterEvent::kStopRequested) {
            state_ = AdapterState::kStopping;
            return true;
        }
        if (event == AdapterEvent::kChildExitedUnexpectedly) {
            state_ = AdapterState::kFaulted;
            return true;
        }
        if (event == AdapterEvent::kShutdownRequested) {
            state_ = AdapterState::kShuttingDown;
            return true;
        }
        return false;

    case AdapterState::kStopping:
        if (event == AdapterEvent::kStopSucceeded) {
            state_ = AdapterState::kIdle;
            return true;
        }
        if (event == AdapterEvent::kChildExitedUnexpectedly) {
            // Process exited during stop — treat as stop succeeded
            state_ = AdapterState::kIdle;
            return true;
        }
        if (event == AdapterEvent::kShutdownRequested) {
            state_ = AdapterState::kShuttingDown;
            return true;
        }
        return false;

    case AdapterState::kFaulted:
        if (event == AdapterEvent::kStartRequested) {
            // Auto-acknowledge fault and transition to starting
            state_ = AdapterState::kStarting;
            return true;
        }
        if (event == AdapterEvent::kShutdownRequested) {
            state_ = AdapterState::kShuttingDown;
            return true;
        }
        return false;

    case AdapterState::kShuttingDown:
        // Terminal state — no transitions
        return false;
    }

    return false;
}

// static
std::string AdapterStateMachine::ToString(AdapterState state) {
    switch (state) {
    case AdapterState::kIdle:          return "Idle";
    case AdapterState::kStarting:      return "Starting";
    case AdapterState::kRunning:       return "Running";
    case AdapterState::kStopping:      return "Stopping";
    case AdapterState::kFaulted:       return "Faulted";
    case AdapterState::kShuttingDown:  return "ShuttingDown";
    }
    return "Unknown";
}

// static
std::string AdapterStateMachine::ToString(AdapterEvent event) {
    switch (event) {
    case AdapterEvent::kStartRequested:          return "StartRequested";
    case AdapterEvent::kStartSucceeded:          return "StartSucceeded";
    case AdapterEvent::kStartFailed:             return "StartFailed";
    case AdapterEvent::kStopRequested:           return "StopRequested";
    case AdapterEvent::kStopSucceeded:           return "StopSucceeded";
    case AdapterEvent::kChildExitedUnexpectedly: return "ChildExitedUnexpectedly";
    case AdapterEvent::kShutdownRequested:       return "ShutdownRequested";
    }
    return "Unknown";
}

}  // namespace robot_switch_server
