#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <sys/types.h>

namespace robot_switch_server {

struct ProcessLaunchResult {
    bool success{false};
    pid_t pid{-1};
    std::string error_detail;
};

struct ProcessExitInfo {
    pid_t pid{-1};
    int raw_status{0};
    bool exited_normally{false};
    int exit_code{-1};
    int signal_number{0};
    std::string description;
};

using ChildExitCallback = std::function<void(const ProcessExitInfo&)>;

class AdapterProcessSupervisor {
public:
    struct Config {
        std::chrono::milliseconds stop_grace_timeout{3000};
        std::chrono::milliseconds stop_kill_timeout{2000};
    };

    AdapterProcessSupervisor();
    explicit AdapterProcessSupervisor(Config config);
    ~AdapterProcessSupervisor();

    AdapterProcessSupervisor(const AdapterProcessSupervisor&) = delete;
    AdapterProcessSupervisor& operator=(const AdapterProcessSupervisor&) = delete;

    bool Start(ChildExitCallback on_exit);
    ProcessLaunchResult Launch(const std::string& executable_path);
    bool Stop(pid_t pid, std::string* error_detail);
    void StopAll();
    void Shutdown();
    bool IsAlive(pid_t pid) const;

    static void BlockSigchld();

private:
    void ReaperLoop();
    void DrainExitedChildren();
    static std::string DescribeExitStatus(int status);

    Config config_;
    ChildExitCallback on_exit_;

    mutable std::mutex mutex_;
    std::set<pid_t> tracked_pids_;

    int signal_fd_{-1};
    int wakeup_pipe_[2]{-1, -1};  // [0]=read end, [1]=write end
    std::thread reaper_thread_;
    std::atomic<bool> running_{false};
};

}  // namespace robot_switch_server
