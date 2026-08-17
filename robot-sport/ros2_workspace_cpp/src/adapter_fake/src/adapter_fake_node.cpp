#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <rclcpp/rclcpp.hpp>
#include <robot_adapter_interfaces/adapter_node_base.hpp>
#include <robot_adapter_interfaces/system_info.hpp>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

// Global flag for delayed_sigterm mode
volatile sig_atomic_t g_sigterm_received = 0;
int g_sigterm_delay_ms = 5000;

void DelayedSigtermHandler(int /*sig*/) {
    g_sigterm_received = 1;
}

class FakeAdapterNode : public robot_adapter_interfaces::AdapterNodeBase {
public:
    FakeAdapterNode()
        : AdapterNodeBase("fake") {
        behavior_mode_ = GetParamOrDefault<std::string>("behavior_mode", "normal");
        exit_delay_ms_ = GetParamOrDefault<int>("exit_delay_ms", 500);
        disconnect_hang_ms_ = GetParamOrDefault<int>("disconnect_hang_ms", 10000);
        sigterm_delay_ms_ = GetParamOrDefault<int>("sigterm_delay_ms", 5000);

        RCLCPP_INFO(get_logger(), "adapter_fake started, mode=%s",
                    behavior_mode_.c_str());
    }

    ~FakeAdapterNode() override {
        // Clean up forked child if any
        if (forked_child_pid_ > 0) {
            kill(forked_child_pid_, SIGKILL);
        }
    }

protected:
    void RegisterExtensions() override {
        if (behavior_mode_ == "delayed_sigterm") {
            g_sigterm_delay_ms = sigterm_delay_ms_;
            struct sigaction sa {};
            sa.sa_handler = DelayedSigtermHandler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            sigaction(SIGTERM, &sa, nullptr);

            sigterm_timer_ = create_wall_timer(
                std::chrono::milliseconds(50),
                [this]() {
                    if (g_sigterm_received) {
                        RCLCPP_INFO(
                            get_logger(),
                            "delayed_sigterm: sleeping %d ms before exit",
                            g_sigterm_delay_ms);
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(g_sigterm_delay_ms));
                        _exit(0);
                    }
                });
        }

        if (behavior_mode_ == "exit_immediately") {
            exit_timer_ = create_wall_timer(
                std::chrono::milliseconds(exit_delay_ms_),
                [this]() {
                    RCLCPP_INFO(
                        get_logger(),
                        "exit_immediately: exiting with code 42");
                    _exit(42);
                });
        }

        const std::string prefix = std::string("/") + get_name() + "/";

        echo_srv_ = create_service<std_srvs::srv::Trigger>(
            prefix + "echo",
            [](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
                response->success = true;
                response->message = "echo";
            });

        fail_motion_srv_ = create_service<std_srvs::srv::Trigger>(
            prefix + "fail_motion",
            [](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
                response->success = false;
                response->message = "forced failure";
            });
    }

    void OnConnect(TriggerResponse response) override {
        if (behavior_mode_ == "connect_fail") {
            response->success = false;
            response->message = "fake connect failure (connect_fail mode)";
            return;
        }

        if (behavior_mode_ == "fork_child") {
            // Fork a sleep child process to test process group cleanup
            pid_t child = fork();
            if (child == 0) {
                // Child: sleep forever
                while (true) {
                    sleep(3600);
                }
            } else if (child > 0) {
                forked_child_pid_ = child;
                RCLCPP_INFO(get_logger(),
                            "fork_child mode: spawned child pid=%d", child);
            }
        }

        connected_ = true;
        response->success = true;
        response->message = "fake adapter connected (mode=" + behavior_mode_ + ")";
    }

    void OnDisconnect(TriggerResponse response) override {
        if (behavior_mode_ == "disconnect_hang") {
            RCLCPP_INFO(get_logger(),
                        "disconnect_hang: blocking for %d ms",
                        disconnect_hang_ms_);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(disconnect_hang_ms_));
        }

        connected_ = false;
        response->success = true;
        response->message = "fake adapter disconnected";
    }

    void OnSafeStop(TriggerResponse response) override {
        if (behavior_mode_ == "safe_stop_fail") {
            response->success = false;
            response->message = "fake safe_stop failure (safe_stop_fail mode)";
            return;
        }

        response->success = true;
        response->message = "fake safe_stop ok";
    }

    void OnHealth(TriggerResponse response) override {
        response->success = true;
        response->message = "{\"ready\":true,\"connected\":" +
                            std::string(connected_ ? "true" : "false") +
                            ",\"mode\":\"" + behavior_mode_ + "\"}";
    }

    void OnSystemInfo(TriggerResponse response) override {
        robot_adapter_interfaces::SystemInfoBuilder system_info;
        system_info.SetDetailsJson("{\"adapter\":\"fake\",\"mode\":\"" +
                                   behavior_mode_ + "\",\"connected\":" +
                                   std::string(connected_ ? "true" : "false") + "}");
        system_info.SetMotions({
            {"echo", "echo", "Test success path", "测试成功"},
            {"fail_motion", "fail_motion", "Test failure path", "测试失败"},
        });

        response->success = true;
        response->message = system_info.Build();
    }

    std::string behavior_mode_;
    int exit_delay_ms_{500};
    int disconnect_hang_ms_{10000};
    int sigterm_delay_ms_{5000};
    bool connected_{false};
    pid_t forked_child_pid_{-1};

    rclcpp::TimerBase::SharedPtr exit_timer_;
    rclcpp::TimerBase::SharedPtr sigterm_timer_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr echo_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr fail_motion_srv_;
};

}  // namespace

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<FakeAdapterNode>();
    node->Init();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
