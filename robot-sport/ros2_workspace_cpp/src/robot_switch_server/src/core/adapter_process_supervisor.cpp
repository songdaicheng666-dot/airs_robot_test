#include "robot_switch_server/core/adapter_process_supervisor.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace robot_switch_server {

namespace {

constexpr std::chrono::milliseconds kWaitPollInterval{50};

bool WaitForPidExit(pid_t pid, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const auto rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid || (rc < 0 && errno == ECHILD)) {
            return true;
        }
        std::this_thread::sleep_for(kWaitPollInterval);
    }
    return false;
}

}  // namespace

// static
void AdapterProcessSupervisor::BlockSigchld() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
}

AdapterProcessSupervisor::AdapterProcessSupervisor()
    : config_() {
}

AdapterProcessSupervisor::AdapterProcessSupervisor(Config config)
    : config_(config) {
}

AdapterProcessSupervisor::~AdapterProcessSupervisor() {
    Shutdown();
}

bool AdapterProcessSupervisor::Start(ChildExitCallback on_exit) {
    if (running_) {
        return false;
    }

    on_exit_ = std::move(on_exit);

    // Create signalfd for SIGCHLD (must be blocked beforehand via BlockSigchld)
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    signal_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd_ < 0) {
        return false;
    }

    // Create wakeup pipe for clean shutdown signaling
    if (pipe2(wakeup_pipe_, O_CLOEXEC | O_NONBLOCK) != 0) {
        close(signal_fd_);
        signal_fd_ = -1;
        return false;
    }

    running_ = true;
    reaper_thread_ = std::thread([this]() { ReaperLoop(); });
    return true;
}

ProcessLaunchResult AdapterProcessSupervisor::Launch(const std::string& executable_path) {
    ProcessLaunchResult result;

    // Create pipe for exec error reporting (CLOEXEC so it auto-closes on successful exec)
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        result.error_detail = std::string("pipe2 failed: ") + std::strerror(errno);
        return result;
    }

    const pid_t child = fork();
    if (child < 0) {
        result.error_detail = std::string("fork failed: ") + std::strerror(errno);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return result;
    }

    if (child == 0) {
        // Child process
        close(pipe_fds[0]);  // Close read end

        // New process group so we can kill the whole group
        setpgid(0, 0);

        // If parent dies, child gets SIGKILL
        prctl(PR_SET_PDEATHSIG, SIGKILL);

        execl(executable_path.c_str(), executable_path.c_str(),
              static_cast<char*>(nullptr));

        // exec failed — write errno to pipe and exit
        const int err = errno;
        auto written = write(pipe_fds[1], &err, sizeof(err));
        (void)written;
        _exit(127);
    }

    // Parent process
    close(pipe_fds[1]);  // Close write end

    // Read from pipe: if exec succeeded, pipe is closed (CLOEXEC) and read returns 0
    // If exec failed, we get the errno value
    int child_errno = 0;
    const auto bytes_read = read(pipe_fds[0], &child_errno, sizeof(child_errno));
    close(pipe_fds[0]);

    if (bytes_read > 0) {
        // exec failed
        waitpid(child, nullptr, 0);  // Reap the failed child
        result.error_detail = std::string("exec failed: ") + std::strerror(child_errno);
        return result;
    }

    // Track the pid
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tracked_pids_.insert(child);
    }

    result.success = true;
    result.pid = child;
    return result;
}

bool AdapterProcessSupervisor::Stop(pid_t pid, std::string* error_detail) {
    if (!error_detail) {
        return false;
    }
    if (pid <= 0) {
        return true;
    }

    // Check if already exited
    {
        int status = 0;
        const auto rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid || (rc < 0 && errno == ECHILD)) {
            std::lock_guard<std::mutex> lock(mutex_);
            tracked_pids_.erase(pid);
            return true;
        }
    }

    // Send SIGTERM to the process group
    const pid_t pgid = getpgid(pid);
    const pid_t kill_target = (pgid > 0 && pgid == pid) ? -pgid : pid;

    if (kill(kill_target, SIGTERM) != 0 && errno != ESRCH) {
        *error_detail = std::string("SIGTERM failed: ") + std::strerror(errno);
        return false;
    }

    if (WaitForPidExit(pid, config_.stop_grace_timeout)) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracked_pids_.erase(pid);
        return true;
    }

    // Escalate to SIGKILL on the process group
    if (kill(kill_target, SIGKILL) != 0 && errno != ESRCH) {
        *error_detail = std::string("SIGKILL failed: ") + std::strerror(errno);
        return false;
    }

    if (WaitForPidExit(pid, config_.stop_kill_timeout)) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracked_pids_.erase(pid);
        return true;
    }

    *error_detail = "process did not exit after SIGKILL; pid=" + std::to_string(pid);
    return false;
}

void AdapterProcessSupervisor::StopAll() {
    std::set<pid_t> pids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pids = tracked_pids_;
    }

    for (const auto pid : pids) {
        std::string error;
        Stop(pid, &error);
    }
}

void AdapterProcessSupervisor::Shutdown() {
    StopAll();

    running_ = false;

    // 唤醒 reaper 线程:向自管道写 1 字节,让它从 poll 阻塞中退出(running_ 已置 false)。
    // write 带 warn_unused_result,必须妥善处理返回值:这是 shutdown 的尽力而为唤醒——
    // 管道满(EAGAIN)说明已有未读唤醒字节、reaper 必会醒;即便写失败,reaper 的 poll
    // 也会因后续 SIGCHLD 退出。故此处消费返回值、不据此恢复。
    if (wakeup_pipe_[1] >= 0) {
        const char b = 1;
        const ssize_t written = write(wakeup_pipe_[1], &b, 1);
        (void)written;
    }

    if (reaper_thread_.joinable()) {
        reaper_thread_.join();
    }

    if (signal_fd_ >= 0) {
        close(signal_fd_);
        signal_fd_ = -1;
    }
    if (wakeup_pipe_[0] >= 0) { close(wakeup_pipe_[0]); wakeup_pipe_[0] = -1; }
    if (wakeup_pipe_[1] >= 0) { close(wakeup_pipe_[1]); wakeup_pipe_[1] = -1; }
}

bool AdapterProcessSupervisor::IsAlive(pid_t pid) const {
    if (pid <= 0) {
        return false;
    }
    // kill(pid, 0) checks if process exists without sending a signal
    return kill(pid, 0) == 0;
}

void AdapterProcessSupervisor::ReaperLoop() {
    while (running_) {
        struct pollfd pfds[2]{};
        pfds[0].fd = signal_fd_;
        pfds[0].events = POLLIN;
        pfds[1].fd = wakeup_pipe_[0];
        pfds[1].events = POLLIN;

        const int ret = poll(pfds, 2, 200);  // 200ms poll timeout
        if (ret > 0) {
            if (pfds[0].revents & POLLIN) {
                // Drain signalfd
                struct signalfd_siginfo info{};
                while (read(signal_fd_, &info, sizeof(info)) == sizeof(info)) {
                    // Just drain; actual reaping below
                }
            }
            if (pfds[1].revents & POLLIN) {
                // Wakeup pipe triggered — shutdown requested
                break;
            }
        }

        // Always try to drain exited children (handles races)
        DrainExitedChildren();
    }

    // Final drain on shutdown
    DrainExitedChildren();
}

void AdapterProcessSupervisor::DrainExitedChildren() {
    while (true) {
        int status = 0;
        const pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            break;
        }

        bool tracked = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tracked = tracked_pids_.erase(pid) > 0;
        }

        if (tracked && on_exit_) {
            ProcessExitInfo info;
            info.pid = pid;
            info.raw_status = status;
            info.exited_normally = WIFEXITED(status);
            if (info.exited_normally) {
                info.exit_code = WEXITSTATUS(status);
            }
            if (WIFSIGNALED(status)) {
                info.signal_number = WTERMSIG(status);
            }
            info.description = DescribeExitStatus(status);
            on_exit_(info);
        }
    }
}

// static
std::string AdapterProcessSupervisor::DescribeExitStatus(int status) {
    if (WIFEXITED(status)) {
        return "exit_code=" + std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return "signal=" + std::to_string(WTERMSIG(status));
    }
    return "unknown_exit_status";
}

}  // namespace robot_switch_server
