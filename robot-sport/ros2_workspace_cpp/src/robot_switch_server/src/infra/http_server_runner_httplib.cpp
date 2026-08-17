#include "robot_switch_server/infra/http_server_runner.hpp"
#include "robot_switch_server/core/adapter_runtime_manager.hpp"
#include "robot_switch_server/http/json_response_builder.hpp"
#include "robot_switch_server/utils/string_utils.hpp"

#include <httplib.h>
#include <rclcpp/rclcpp.hpp>

#include <atomic>
#include <charconv>
#include <cctype>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace robot_switch_server {

namespace {

constexpr const char* kJsonContentType = "application/json";

struct ListenEndpoint {
    std::string host;
    int port{0};
};

bool ParseListenAddress(const std::string& listen_address,
                        ListenEndpoint* endpoint) {
    if (endpoint == nullptr) {
        return false;
    }

    const auto split = listen_address.find(':');
    if (split == std::string::npos) {
        return false;
    }

    std::string host = TrimCopy(listen_address.substr(0, split));
    if (host.empty()) {
        host = "0.0.0.0";
    }

    const std::string port_text = TrimCopy(listen_address.substr(split + 1));
    if (port_text.empty()) {
        return false;
    }

    int port = 0;
    const auto [ptr, ec] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (ec != std::errc() || ptr != port_text.data() + port_text.size() ||
        port <= 0 || port > 65535) {
        return false;
    }

    endpoint->host = std::move(host);
    endpoint->port = port;
    return true;
}

void SetJsonResponse(httplib::Response* response, int status,
                     const std::string& body) {
    if (response == nullptr) {
        return;
    }
    response->status = status;
    response->set_content(body, kJsonContentType);
}

void RegisterRoutes(httplib::Server* server,
                    const std::shared_ptr<AdapterRuntimeManager>& manager) {
    server->Get("/health", [](const httplib::Request& /*request*/,
                               httplib::Response& response) {
        SetJsonResponse(&response, 200, JsonResponseBuilder::BuildHealthResponse());
    });

    server->Get("/status",
                [manager](const httplib::Request& /*request*/,
                          httplib::Response& response) {
                    SetJsonResponse(
                        &response, 200,
                        JsonResponseBuilder::BuildStatusResponse(
                            manager->GetStatusWithHealth()));
                });

    server->Post("/start", [manager](const httplib::Request& request,
                                     httplib::Response& response) {
        if (!request.has_param("adapter_type")) {
            SetJsonResponse(&response, 400,
                            JsonResponseBuilder::BuildBadRequestResponse(
                                "missing required parameter 'adapter_type'"));
            return;
        }
        const std::string adapter_type = TrimCopy(request.get_param_value("adapter_type"));

        const auto result = manager->Start(adapter_type);
        SetJsonResponse(&response, 200,
                        JsonResponseBuilder::BuildOperationResponse(result.operation, result.snapshot));
    });

    server->Post("/stop", [manager](const httplib::Request& /*request*/,
                                    httplib::Response& response) {
        const auto result = manager->Stop();
        SetJsonResponse(&response, 200,
                        JsonResponseBuilder::BuildOperationResponse(result.operation, result.snapshot));
    });

    server->Post("/motion", [manager](const httplib::Request& request,
                                     httplib::Response& response) {
        if (!request.has_param("motion_id")) {
            SetJsonResponse(&response, 400,
                            JsonResponseBuilder::BuildBadRequestResponse(
                                "missing required parameter 'motion_id'"));
            return;
        }
        const std::string motion_id =
            TrimCopy(request.get_param_value("motion_id"));

        const auto result = manager->InvokeMotion(motion_id);
        const int http_status = (result.code == 0)   ? 200
                              : (result.code == 400) ? 400
                              :                         502;
        SetJsonResponse(&response, http_status,
                        JsonResponseBuilder::BuildMotionResponse(result));
    });

    server->Get("/system_info",
                [manager](const httplib::Request& /*request*/,
                          httplib::Response& response) {
                    const auto system_info = manager->GetSystemInfo();
                    const auto snapshot = manager->GetStatusWithHealth();
                    SetJsonResponse(
                        &response, 200,
                        JsonResponseBuilder::BuildSystemInfoResponse(
                            system_info, snapshot));
                });

    server->Get("/adapters",
                [manager](const httplib::Request& /*request*/,
                          httplib::Response& response) {
                    SetJsonResponse(
                        &response, 200,
                        JsonResponseBuilder::BuildAdaptersResponse(
                            manager->GetEnabledAdapterTypes()));
                });
}

}  // namespace

struct HttpServerRunner::Impl {
    std::unique_ptr<httplib::Server> server;
    std::thread serve_thread;
    std::atomic<bool> stopping{false};
    std::string host;
    int port{0};
};

HttpServerRunner::HttpServerRunner(rclcpp::Logger logger)
    : logger_(std::move(logger)), impl_(std::make_unique<Impl>()) {
}

HttpServerRunner::~HttpServerRunner() {
    Stop();
}

bool HttpServerRunner::Start(
    const std::string& listen_address,
    const std::shared_ptr<AdapterRuntimeManager>& manager) {
    if (impl_->server != nullptr) {
        RCLCPP_WARN(logger_, "HTTP server already started");
        return true;
    }

    if (manager == nullptr) {
        RCLCPP_ERROR(logger_, "failed to start HTTP server: manager is null");
        return false;
    }

    ListenEndpoint endpoint;
    if (!ParseListenAddress(listen_address, &endpoint)) {
        RCLCPP_ERROR(logger_,
                     "failed to start HTTP server: invalid listen address '%s' "
                     "(expected host:port or [ipv6]:port)",
                     listen_address.c_str());
        return false;
    }

    auto server = std::make_unique<httplib::Server>();
    RegisterRoutes(server.get(), manager);

    // httplib::bind_to_port(host, port) 返回 bool(成功 true / 失败 false),不是端口号;
    // 返回端口号的是另一个重载 bind_to_any_port。原来写 `bound_port < 0` 把 bool 当 int 比,
    // bool 恒 >=0,绑定失败分支永远进不去——是个潜伏 bug,这里按 bool 判定修正。
    const bool bind_ok =
        server->bind_to_port(endpoint.host.c_str(), endpoint.port);
    if (!bind_ok) {
        RCLCPP_ERROR(logger_, "failed to bind HTTP server on %s:%d",
                     endpoint.host.c_str(), endpoint.port);
        return false;
    }

    impl_->host = endpoint.host;
    impl_->port = endpoint.port;
    impl_->stopping.store(false);
    impl_->server = std::move(server);

    impl_->serve_thread = std::thread([this]() {
        const bool listen_result = impl_->server->listen_after_bind();
        if (!listen_result && !impl_->stopping.load()) {
            RCLCPP_ERROR(logger_, "HTTP server terminated unexpectedly");
        }
    });

    RCLCPP_INFO(logger_, "HTTP server listening on %s:%d", impl_->host.c_str(),
                impl_->port);
    return true;
}

void HttpServerRunner::Stop() {
    if (!impl_ || impl_->server == nullptr) {
        return;
    }

    impl_->stopping.store(true);
    impl_->server->stop();
    if (impl_->serve_thread.joinable()) {
        impl_->serve_thread.join();
    }

    impl_->server.reset();
    impl_->host.clear();
    impl_->port = 0;
}

}  // namespace robot_switch_server
