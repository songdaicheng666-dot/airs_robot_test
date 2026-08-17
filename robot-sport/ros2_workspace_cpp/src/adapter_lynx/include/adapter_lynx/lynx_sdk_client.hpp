#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace adapter_lynx {

struct LynxConfig {
    std::string robot_ip{"10.21.31.103"};
    uint16_t robot_port{30000};
    uint16_t local_port{0};  // 0 = OS-assigned
    double heartbeat_interval_sec{1.0};
    double recv_timeout_sec{1.0};
};

// Passively received robot status snapshot (updated from robot push reports)
struct LynxStatus {
    // Basic status (2 Hz)
    int motion_state{-1};       // MotionState
    int gait{-1};               // Gait
    int control_usage_mode{-1}; // ControlUsageMode: 0=regular, 1=navigation
    bool sleeping{false};
    bool charging{false};
    bool e_stop{false};

    // Battery (from DeviceStatus, 2 Hz)
    double battery_voltage{0.0};
    double battery_current{0.0};
    int battery_percentage{-1};

    // Motion velocity (from MotionStatus, 10 Hz)
    double vel_x{0.0};
    double vel_y{0.0};
    double yaw_rate{0.0};
    double pitch{0.0};
    double roll{0.0};
    double yaw{0.0};
    double body_height{0.0};

    // Error status (2 Hz, extra push on change)
    int error_code{0};
    int error_component{0};

    // Sleep settings
    bool sleep_auto{false};
    int sleep_auto_time_min{0};

    bool valid{false};
    std::chrono::steady_clock::time_point last_update{};
    std::chrono::steady_clock::time_point basic_status_last_update{};
};

class LynxSdkClient {
public:
    using StatusCallback = std::function<void(const LynxStatus&)>;

    static constexpr int kUsageModeRegular = 0;
    static constexpr int kUsageModeNavigation = 1;
    static constexpr int kUsageModeAssisted = 2;
    static constexpr int kMotionStateIdle = 0;
    static constexpr int kMotionStateStanding = 1;
    static constexpr int kMotionStateSoftStop = 2;
    static constexpr int kMotionStateProne = 4;
    static constexpr int kMotionStateRlControl = 17;
    static constexpr int kGaitStandardFlat = 0x1001;
    static constexpr int kGaitStandardStairs = 0x1003;
    static constexpr int kGaitAgileFlat = 0x3002;
    static constexpr int kGaitAgileStairs = 0x3003;

    LynxSdkClient() = default;
    ~LynxSdkClient();

    LynxSdkClient(const LynxSdkClient&) = delete;
    LynxSdkClient& operator=(const LynxSdkClient&) = delete;

    // Opens UDP socket, binds local port, starts heartbeat + recv threads.
    bool Initialize(const LynxConfig& config, std::string* error = nullptr);
    bool IsInitialized() const;

    // Sends zero-velocity, stops threads, closes socket.
    void Shutdown();

    // =========================================================================
    // 1. 控制类接口
    // =========================================================================

    // 1.1 心跳 (Type:100, Cmd:100) — 由 HeartbeatLoop 自动发，也可手动调
    bool SendHeartbeat();

    // 1.2 使用模式切换 (Type:1101, Cmd:5) — mode: 0=常规, 1=导航
    bool SetMode(int mode);

    // 1.3 运动状态转换 (Type:2, Cmd:22) — 0=空闲, 1=站立, 2=软急停,
    // 3=开机阻尼, 4=趴下, 17=RL 控制
    bool SetMotionState(int param);

    // 1.4 步态切换 (Type:2, Cmd:23) — 0x1001=基础, 0x1003=标准楼梯,
    // 0x3002=敏捷平地, 0x3003=敏捷楼梯
    bool SetGait(int gait);

    // 1.5 运动控制轴指令 (Type:2, Cmd:21) — 参数为 [-1, 1] 轴比例，建议 20Hz
    bool SendMotionCmd(double x_ratio, double y_ratio, double yaw_ratio);
    bool SendZeroAxisVelocity();

    // 1.6 运动控制速度真值 (Type:2, Cmd:25) — X/Y: m/s, Yaw: rad/s
    bool SendVelocityCmd(
        double linear_x_mps,
        double linear_y_mps,
        double angular_z_radps);
    bool SendZeroAbsoluteVelocity();

    // 1.6 照明灯 (Type:1101, Cmd:2) — front/back: 0=关, 1=开
    bool SetLights(int front, int back);

    // 1.7 自主充电 (Type:2, Cmd:24) — charge: 1=进入充电, 0=退出
    bool SetAutoCharge(int charge);

    // 1.8 休眠模式 (Type:1101, Cmd:6)
    bool SetSleepMode(bool sleep, bool auto_sleep, int time_min);

    // 2.5 休眠状态查询 (Type:1101, Cmd:7)
    bool QuerySleepStatus();

    // =========================================================================
    // 状态访问
    // =========================================================================
    LynxStatus GetLatestStatus() const;
    void ResetStatus();
    void SetStatusCallback(StatusCallback callback);
    uint64_t GetSleepStatusVersion() const;
    bool WaitForSleepStatusUpdate(
        uint64_t old_version, std::chrono::milliseconds timeout) const;
    uint64_t GetBasicStatusVersion() const;
    bool WaitForGait(
        uint64_t old_version,
        int expected_gait,
        std::chrono::milliseconds timeout) const;
    bool WaitForUsageMode(
        uint64_t old_version,
        int expected_mode,
        std::chrono::milliseconds timeout) const;
    bool WaitForMotionState(
        uint64_t old_version,
        int expected_motion_state,
        std::chrono::milliseconds timeout) const;

private:
    static constexpr size_t kHeaderSize = 16;
    static constexpr uint8_t kSync0 = 0xEB;
    static constexpr uint8_t kSync1 = 0x91;
    static constexpr uint8_t kSync2 = 0xEB;
    static constexpr uint8_t kSync3 = 0x90;
    static constexpr uint8_t kAsduJson = 0x01;

    bool SendPacket(const std::string& json);
    std::vector<uint8_t> BuildPacket(const std::string& json, uint16_t msg_id) const;
    std::string NowStr() const;

    void HeartbeatLoop();
    void RecvLoop();
    void ParseReceivedJson(const std::string& json);

    mutable std::mutex mutex_;
    mutable std::mutex status_mutex_;
    mutable std::mutex query_mutex_;
    mutable std::condition_variable query_cv_;

    bool initialized_{false};
    int sockfd_{-1};
    sockaddr_in robot_addr_{};
    LynxConfig config_;
    uint16_t msg_id_counter_{0};

    LynxStatus latest_status_;
    StatusCallback status_callback_;
    uint64_t sleep_status_version_{0};
    uint64_t basic_status_version_{0};

    std::atomic<bool> running_{false};
    std::thread heartbeat_thread_;
    std::thread recv_thread_;
};

}  // namespace adapter_lynx
