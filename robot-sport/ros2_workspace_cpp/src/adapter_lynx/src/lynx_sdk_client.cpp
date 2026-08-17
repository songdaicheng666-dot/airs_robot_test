#include "adapter_lynx/lynx_sdk_client.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace adapter_lynx {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

LynxSdkClient::~LynxSdkClient() {
    Shutdown();
}

bool LynxSdkClient::Initialize(const LynxConfig& config, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;

    // Clear stale status from any previous session
    {
        std::lock_guard<std::mutex> slock(status_mutex_);
        latest_status_ = LynxStatus{};
    }

    config_ = config;

    sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        if (error) *error = std::string("socket() failed: ") + std::strerror(errno);
        return false;
    }

    // SO_RCVTIMEO lets RecvLoop wake up periodically to check running_
    struct timeval tv{};
    tv.tv_sec  = static_cast<long>(config_.recv_timeout_sec);
    tv.tv_usec = static_cast<long>((config_.recv_timeout_sec - tv.tv_sec) * 1e6);
    setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Bind local port (0 = OS-assigned; fixes the source port so the robot
    // can send status replies back to us)
    sockaddr_in local_addr{};
    local_addr.sin_family      = AF_INET;
    local_addr.sin_port        = htons(config_.local_port);
    local_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sockfd_, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
        close(sockfd_);
        sockfd_ = -1;
        if (error) *error = std::string("bind() failed: ") + std::strerror(errno);
        return false;
    }

    robot_addr_ = {};
    robot_addr_.sin_family = AF_INET;
    robot_addr_.sin_port   = htons(config_.robot_port);
    if (inet_pton(AF_INET, config_.robot_ip.c_str(), &robot_addr_.sin_addr) <= 0) {
        close(sockfd_);
        sockfd_ = -1;
        if (error) *error = "Invalid robot IP: " + config_.robot_ip;
        return false;
    }

    running_ = true;
    heartbeat_thread_ = std::thread(&LynxSdkClient::HeartbeatLoop, this);
    recv_thread_      = std::thread(&LynxSdkClient::RecvLoop, this);

    initialized_ = true;
    return true;
}

bool LynxSdkClient::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

void LynxSdkClient::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) return;
        running_     = false;
        initialized_ = false;
    }
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (recv_thread_.joinable())      recv_thread_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    if (sockfd_ >= 0) {
        close(sockfd_);
        sockfd_ = -1;
    }

    // Clear stale status so it doesn't leak into the next session
    {
        std::lock_guard<std::mutex> slock(status_mutex_);
        latest_status_ = LynxStatus{};
    }
}

// ---------------------------------------------------------------------------
// 1.1 心跳
// ---------------------------------------------------------------------------

bool LynxSdkClient::SendHeartbeat() {
    std::ostringstream oss;
    oss << R"({"PatrolDevice":{"Type":100,"Command":100,"Time":")"
        << NowStr() << R"(","Items":{}}})";
    return SendPacket(oss.str());
}

// ---------------------------------------------------------------------------
// 1.2 使用模式切换  mode: 0=常规(可用轴指令), 1=导航
// ---------------------------------------------------------------------------

bool LynxSdkClient::SetMode(int mode) {
    std::ostringstream oss;
    oss << R"({"PatrolDevice":{"Type":1101,"Command":5,"Time":")"
        << NowStr()
        << R"(","Items":{"Mode":)" << mode << R"(}}})";
    return SendPacket(oss.str());
}

// ---------------------------------------------------------------------------
// 1.3 运动状态转换  0=空闲, 1=站立, 2=软急停, 3=开机阻尼, 4=趴下, 17=RL 控制
// ---------------------------------------------------------------------------

bool LynxSdkClient::SetMotionState(int param) {
    std::ostringstream oss;
    oss << R"({"PatrolDevice":{"Type":2,"Command":22,"Time":")"
        << NowStr()
        << R"(","Items":{"MotionParam":)" << param << R"(}}})";
    return SendPacket(oss.str());
}

// ---------------------------------------------------------------------------
// 1.4 步态切换  0x1001=基础, 0x1003=标准楼梯, 0x3002=敏捷平地, 0x3003=敏捷楼梯
// ---------------------------------------------------------------------------

bool LynxSdkClient::SetGait(int gait) {
    std::ostringstream oss;
    oss << R"({"PatrolDevice":{"Type":2,"Command":23,"Time":")"
        << NowStr()
        << R"(","Items":{"GaitParam":)" << gait << R"(}}})";
    return SendPacket(oss.str());
}

// ---------------------------------------------------------------------------
// 1.5 运动控制轴指令  建议 20Hz，X/Y/Yaw 均为 [-1, 1] 轴比例
//     移动步态下通常只有 X(前后) / Y(左右) / Yaw(转向) 生效
// ---------------------------------------------------------------------------

bool LynxSdkClient::SendMotionCmd(
    double x_ratio, double y_ratio, double yaw_ratio) {
    const auto is_valid_axis = [](double value) {
        return std::isfinite(value) && value >= -1.0 && value <= 1.0;
    };
    if (!is_valid_axis(x_ratio) ||
        !is_valid_axis(y_ratio) ||
        !is_valid_axis(yaw_ratio)) {
        return false;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    oss << R"({"PatrolDevice":{"Type":2,"Command":21,"Time":")"
        << NowStr()
        << R"(","Items":{"X":)" << x_ratio
        << R"(,"Y":)" << y_ratio
        << R"(,"Z":0.0,"Roll":0.0,"Pitch":0.0,"Yaw":)" << yaw_ratio
        << R"(}}})";
    return SendPacket(oss.str());
}

bool LynxSdkClient::SendZeroAxisVelocity() {
    return SendMotionCmd(0.0, 0.0, 0.0);
}

// ---------------------------------------------------------------------------
// 1.6 运动控制速度真值  建议 20Hz，X/Y: m/s，Yaw: rad/s
// ---------------------------------------------------------------------------

bool LynxSdkClient::SendVelocityCmd(
    double linear_x_mps,
    double linear_y_mps,
    double angular_z_radps) {
    if (!std::isfinite(linear_x_mps) ||
        !std::isfinite(linear_y_mps) ||
        !std::isfinite(angular_z_radps)) {
        return false;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    oss << R"({"PatrolDevice":{"Type":2,"Command":25,"Time":")"
        << NowStr()
        << R"(","Items":{"X":)" << linear_x_mps
        << R"(,"Y":)" << linear_y_mps
        << R"(,"Z":0.0,"Roll":0.0,"Pitch":0.0,"Yaw":)" << angular_z_radps
        << R"(}}})";
    return SendPacket(oss.str());
}

bool LynxSdkClient::SendZeroAbsoluteVelocity() {
    return SendVelocityCmd(0.0, 0.0, 0.0);
}

// ---------------------------------------------------------------------------
// 1.6 照明灯  front/back: 0=关, 1=开
// ---------------------------------------------------------------------------

bool LynxSdkClient::SetLights(int front, int back) {
    std::ostringstream oss;
    oss << R"({"PatrolDevice":{"Type":1101,"Command":2,"Time":")"
        << NowStr()
        << R"(","Items":{"Front":)" << front
        << R"(,"Back":)" << back
        << R"(}}})";
    return SendPacket(oss.str());
}

// ---------------------------------------------------------------------------
// 1.7 自主充电  charge: 1=进入充电流程, 0=退出
// ---------------------------------------------------------------------------

bool LynxSdkClient::SetAutoCharge(int charge) {
    std::ostringstream oss;
    oss << R"({"PatrolDevice":{"Type":2,"Command":24,"Time":")"
        << NowStr()
        << R"(","Items":{"Charge":)" << charge
        << R"(}}})";
    return SendPacket(oss.str());
}

// ---------------------------------------------------------------------------
// 1.8 休眠模式设置
//     sleep=true 立即进入休眠, auto_sleep=true 开启自动休眠, time_min=超时分钟
// ---------------------------------------------------------------------------

bool LynxSdkClient::SetSleepMode(bool sleep, bool auto_sleep, int time_min) {
    std::ostringstream oss;
    oss << R"({"PatrolDevice":{"Type":1101,"Command":6,"Time":")"
        << NowStr()
        << R"(","Items":{"Sleep":)" << (sleep ? "true" : "false")
        << R"(,"Auto":)" << (auto_sleep ? "true" : "false")
        << R"(,"Time":)" << time_min
        << R"(}}})";
    return SendPacket(oss.str());
}

// ---------------------------------------------------------------------------
// 2.5 休眠状态及设置信息查询
// ---------------------------------------------------------------------------

bool LynxSdkClient::QuerySleepStatus() {
    std::ostringstream oss;
    oss << R"({"PatrolDevice":{"Type":1101,"Command":7,"Time":")"
        << NowStr()
        << R"(","Items":{}}})";
    return SendPacket(oss.str());
}

// ---------------------------------------------------------------------------
// 状态访问
// ---------------------------------------------------------------------------

LynxStatus LynxSdkClient::GetLatestStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return latest_status_;
}

void LynxSdkClient::ResetStatus() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    latest_status_ = LynxStatus{};
}

void LynxSdkClient::SetStatusCallback(StatusCallback callback) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_callback_ = std::move(callback);
}

uint64_t LynxSdkClient::GetSleepStatusVersion() const {
    std::lock_guard<std::mutex> lock(query_mutex_);
    return sleep_status_version_;
}

bool LynxSdkClient::WaitForSleepStatusUpdate(
    uint64_t old_version, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(query_mutex_);
    return query_cv_.wait_for(lock, timeout, [this, old_version]() {
        return sleep_status_version_ > old_version;
    });
}

uint64_t LynxSdkClient::GetBasicStatusVersion() const {
    std::lock_guard<std::mutex> lock(query_mutex_);
    return basic_status_version_;
}

bool LynxSdkClient::WaitForGait(
    uint64_t old_version,
    int expected_gait,
    std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(query_mutex_);
    return query_cv_.wait_for(lock, timeout, [this, old_version, expected_gait]() {
        if (basic_status_version_ <= old_version) {
            return false;
        }
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        return latest_status_.gait == expected_gait;
    });
}

bool LynxSdkClient::WaitForUsageMode(
    uint64_t old_version,
    int expected_mode,
    std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(query_mutex_);
    return query_cv_.wait_for(lock, timeout, [this, old_version, expected_mode]() {
        if (basic_status_version_ <= old_version) {
            return false;
        }
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        return latest_status_.control_usage_mode == expected_mode;
    });
}

bool LynxSdkClient::WaitForMotionState(
    uint64_t old_version,
    int expected_motion_state,
    std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(query_mutex_);
    return query_cv_.wait_for(
        lock, timeout, [this, old_version, expected_motion_state]() {
            if (basic_status_version_ <= old_version) {
                return false;
            }
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            return latest_status_.motion_state == expected_motion_state;
        });
}

// ---------------------------------------------------------------------------
// 内部：发包
// ---------------------------------------------------------------------------

bool LynxSdkClient::SendPacket(const std::string& json) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sockfd_ < 0) return false;

    uint16_t mid    = msg_id_counter_++;
    auto     packet = BuildPacket(json, mid);

    ssize_t sent = sendto(
        sockfd_, packet.data(), packet.size(), 0,
        reinterpret_cast<const sockaddr*>(&robot_addr_), sizeof(robot_addr_));

    return sent == static_cast<ssize_t>(packet.size());
}

std::vector<uint8_t>
LynxSdkClient::BuildPacket(const std::string& json, uint16_t msg_id) const {
    //  字节: 0-3   = EB 91 EB 90  (同步头)
    //        4-5   = JSON长度小端
    //        6-7   = 消息ID小端
    //        8     = 0x01 (ASDU格式=JSON)
    //        9-15  = 预留(0)
    //        16+   = JSON字符串
    std::vector<uint8_t> packet(kHeaderSize + json.size(), 0);

    packet[0] = kSync0;
    packet[1] = kSync1;
    packet[2] = kSync2;
    packet[3] = kSync3;

    const uint16_t len = static_cast<uint16_t>(json.size());
    packet[4] = static_cast<uint8_t>(len & 0xFF);
    packet[5] = static_cast<uint8_t>((len >> 8) & 0xFF);

    packet[6] = static_cast<uint8_t>(msg_id & 0xFF);
    packet[7] = static_cast<uint8_t>((msg_id >> 8) & 0xFF);

    packet[8] = kAsduJson;
    // 9-15: reserved, already 0
    std::memcpy(packet.data() + kHeaderSize, json.data(), json.size());
    return packet;
}

std::string LynxSdkClient::NowStr() const {
    std::time_t now = std::time(nullptr);
    std::tm     tm{};
    localtime_r(&now, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// ---------------------------------------------------------------------------
// 后台线程：心跳
// ---------------------------------------------------------------------------

void LynxSdkClient::HeartbeatLoop() {
    while (running_) {
        SendHeartbeat();
        const auto interval = std::chrono::milliseconds(
            static_cast<int>(config_.heartbeat_interval_sec * 1000.0));
        std::this_thread::sleep_for(interval);
    }
}

// ---------------------------------------------------------------------------
// 后台线程：接收（解析机器人主动上报的状态）
// ---------------------------------------------------------------------------

void LynxSdkClient::RecvLoop() {
    constexpr size_t kBufSize = 8192;
    std::vector<uint8_t> buf(kBufSize);

    while (running_) {
        int fd;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fd = sockfd_;
        }
        if (fd < 0) break;

        sockaddr_in from{};
        socklen_t   from_len = sizeof(from);
        const ssize_t n = recvfrom(
            fd, buf.data(), buf.size(), 0,
            reinterpret_cast<sockaddr*>(&from), &from_len);

        if (n < 0) {
            // EAGAIN/EWOULDBLOCK = SO_RCVTIMEO 超时，属于正常，用于检查 running_
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;  // 真正的错误
        }

        if (static_cast<size_t>(n) <= kHeaderSize) continue;

        // 验证同步头
        if (buf[0] != kSync0 || buf[1] != kSync1 ||
            buf[2] != kSync2 || buf[3] != kSync3) continue;

        const uint16_t payload_len =
            static_cast<uint16_t>(buf[4]) | (static_cast<uint16_t>(buf[5]) << 8);

        if (kHeaderSize + payload_len > static_cast<size_t>(n)) continue;

        std::string json(
            reinterpret_cast<char*>(buf.data() + kHeaderSize), payload_len);
        ParseReceivedJson(json);
    }
}

// ---------------------------------------------------------------------------
// 解析机器人上报的 JSON，更新本地状态缓存
// 机器人主动上报 4 类数据（持续心跳后触发）：
//   基础状态(2Hz)、运控状态(10Hz)、设备状态(2Hz)、异常状态(2Hz)
// ---------------------------------------------------------------------------

void LynxSdkClient::ParseReceivedJson(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        if (!j.contains("PatrolDevice")) return;

        auto& pd    = j["PatrolDevice"];
        if (!pd.contains("Items")) return;
        auto& items = pd["Items"];

        LynxStatus status;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status = latest_status_;  // 从缓存开始，增量更新
        }

        bool updated = false;
        bool sleep_status_updated = false;
        bool basic_status_updated = false;

        // ---- 基础状态：实际包装在 Items.BasicStatus 下 ----
        if (items.contains("BasicStatus")) {
            auto& bs = items["BasicStatus"];
            basic_status_updated = true;
            status.basic_status_last_update = std::chrono::steady_clock::now();
            if (bs.contains("MotionState"))     { status.motion_state      = bs["MotionState"].get<int>();      updated = true; }
            if (bs.contains("Gait"))            { status.gait              = bs["Gait"].get<int>();             updated = true; }
            if (bs.contains("ControlUsageMode")){ status.control_usage_mode= bs["ControlUsageMode"].get<int>(); updated = true; }
            if (bs.contains("Sleep"))           {
                auto& v = bs["Sleep"];
                status.sleeping = v.is_boolean() ? v.get<bool>() : (v.get<int>() != 0);
                updated = true;
            }
            if (bs.contains("Charge"))          {
                auto& v = bs["Charge"];
                status.charging = v.is_boolean() ? v.get<bool>() : (v.get<int>() != 0);
                updated = true;
            }
            if (bs.contains("HES"))             {
                auto& v = bs["HES"];
                status.e_stop = v.is_boolean() ? v.get<bool>() : (v.get<int>() != 0);
                updated = true;
            }
        }

        // ---- 运控状态 (MotionStatus) ----
        if (items.contains("MotionStatus")) {
            auto& ms = items["MotionStatus"];
            if (ms.contains("LinearX"))    status.vel_x       = ms["LinearX"].get<double>();
            if (ms.contains("LinearY"))    status.vel_y       = ms["LinearY"].get<double>();
            if (ms.contains("OmegaZ"))     status.yaw_rate    = ms["OmegaZ"].get<double>();
            if (ms.contains("Pitch"))      status.pitch       = ms["Pitch"].get<double>();
            if (ms.contains("Roll"))       status.roll        = ms["Roll"].get<double>();
            if (ms.contains("Yaw"))        status.yaw         = ms["Yaw"].get<double>();
            if (ms.contains("Height"))     status.body_height = ms["Height"].get<double>();
            updated = true;
        }

        // ---- 设备状态 (BatteryStatus)：字段名为 BatteryLevelLeft/Right ----
        if (items.contains("BatteryStatus")) {
            auto& batt = items["BatteryStatus"];
            // 取左右电量平均值作为整体电量百分比
            if (batt.contains("BatteryLevelLeft") && batt.contains("BatteryLevelRight")) {
                status.battery_percentage = (batt["BatteryLevelLeft"].get<int>() +
                                             batt["BatteryLevelRight"].get<int>()) / 2;
            }
            // 电压取左侧（单位：V，原始值已是整数，如 71 表示 71V？需实机确认）
            if (batt.contains("VoltageLeft"))  status.battery_voltage = batt["VoltageLeft"].get<double>();
            updated = true;
        }

        // ---- 异常状态 ----
        if (items.contains("errorCode")) {
            status.error_code      = items["errorCode"].get<int>();
            status.error_component = items.value("component", 0);
            updated = true;
        }

        // ---- 休眠设置查询结果 ----
        if (items.contains("Auto")) {
            auto& v = items["Auto"];
            status.sleep_auto = v.is_boolean() ? v.get<bool>() : (v.get<int>() != 0);
            updated = true;
            sleep_status_updated = true;
        }
        if (items.contains("Time") && items["Time"].is_number()) {
            status.sleep_auto_time_min = items["Time"].get<int>();
            updated = true;
            sleep_status_updated = true;
        }

        if (!updated) return;

        status.valid       = true;
        status.last_update = std::chrono::steady_clock::now();

        StatusCallback callback;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            latest_status_ = status;
            callback       = status_callback_;
        }
        {
            std::lock_guard<std::mutex> lock(query_mutex_);
            if (basic_status_updated) {
                ++basic_status_version_;
            }
            if (sleep_status_updated) {
                ++sleep_status_version_;
            }
        }
        if (basic_status_updated || sleep_status_updated) {
            query_cv_.notify_all();
        }
        if (callback) callback(status);

    } catch (const std::exception&) {
        // 格式异常，静默忽略
    }
}

}  // namespace adapter_lynx
