#include "remote_controller/websocket_server.hpp"

namespace remote_controller
{

WebSocketServerManager::WebSocketServerManager(
  std::shared_ptr<ConfigManager> config_manager,
  rclcpp::Logger logger
)
: config_manager_(config_manager),
  logger_(logger),
  server_running_(false)
{
  ws_server_.clear_access_channels(websocketpp::log::alevel::all);
  ws_server_.clear_error_channels(websocketpp::log::elevel::all);
  ws_server_.init_asio();
  ws_server_.set_reuse_addr(true);
  setupEventHandlers();
}

WebSocketServerManager::~WebSocketServerManager()
{
  stop();
}

bool WebSocketServerManager::start()
{
  if (server_running_.load()) {
    RCLCPP_WARN(logger_, "WebSocket server is already running");
    return false;
  }

  try {
    const auto & ws_config = config_manager_->getWebSocketConfig();
    server_thread_ = std::thread(
      [this, ws_config]() {
        runServer();
      });

    server_running_.store(true);
    RCLCPP_INFO(
      logger_, "WebSocket server started on %s:%d",
      ws_config.host.c_str(), ws_config.port);
    return true;

  } catch (const std::exception & e) {
    RCLCPP_ERROR(logger_, "Failed to start WebSocket server: %s", e.what());
    return false;
  }
}

void WebSocketServerManager::stop()
{
  if (!server_running_.load()) {
    return;
  }

  server_running_.store(false);

  // Gracefully shutdown WebSocket server
  ws_server_.stop_listening();
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto hdl : connections_) {
      ws_server_.close(hdl, websocketpp::close::status::going_away, "Shutting down");
    }
  }

  if (server_thread_.joinable()) {
    server_thread_.join();
  }

  RCLCPP_INFO(logger_, "WebSocket server stopped");
}

void WebSocketServerManager::setupEventHandlers()
{

  ws_server_.set_open_handler(
    [this](connection_hdl hdl) {
      onOpen(hdl);
    });

  ws_server_.set_close_handler(
    [this](connection_hdl hdl) {
      onClose(hdl);
    });

  ws_server_.set_message_handler(
    [this](connection_hdl hdl, ws_message_ptr msg) {
      onMessage(hdl, msg);
    });
}

void WebSocketServerManager::runServer()
{
  try {
    const auto & ws_config = config_manager_->getWebSocketConfig();
    try {
      auto addr = websocketpp::lib::asio::ip::address::from_string(ws_config.host);
      websocketpp::lib::asio::ip::tcp::endpoint ep(addr,
        static_cast<unsigned short>(ws_config.port));
      ws_server_.listen(ep);
    } catch (const std::exception & e) {
      RCLCPP_WARN(
        logger_, "Invalid host '%s' (%s). Falling back to port-only listen on %d",
        ws_config.host.c_str(), e.what(), ws_config.port);
      ws_server_.listen(ws_config.port);
    }
    ws_server_.start_accept();
    ws_server_.run();
  } catch (const std::exception & e) {
    RCLCPP_ERROR(logger_, "WebSocket server error: %s", e.what());
    server_running_.store(false);
  }
}

void WebSocketServerManager::onOpen(connection_hdl hdl)
{
  const auto & config = config_manager_->getConfig();
  bool reject = false;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    std::size_t current = connections_.size();
    if (config.websocket.max_connections > 0 &&
      current >= static_cast<std::size_t>(config.websocket.max_connections))
    {
      reject = true;
    } else {
      connections_.insert(hdl);
    }
  }
  if (reject) {
    ws_server_.close(hdl, websocketpp::close::status::policy_violation, "Too many connections");
    if (config.logging.enable_websocket_logs) {
      RCLCPP_WARN(
        logger_, "Rejected WebSocket client: max connections %d reached",
        config.websocket.max_connections);
    }
    return;
  }
  if (config.logging.enable_websocket_logs) {
    RCLCPP_INFO(logger_, "WebSocket client connected (total: %zu)", getConnectionCount());
  }
}

void WebSocketServerManager::onClose(connection_hdl hdl)
{
  const auto & config = config_manager_->getConfig();
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.erase(hdl);
    if (config.logging.enable_websocket_logs) {
      RCLCPP_INFO(logger_, "WebSocket client disconnected (total: %zu)", connections_.size());
    }
  }
}

void WebSocketServerManager::onMessage(connection_hdl hdl, ws_message_ptr msg)
{
  if (!message_handler_) {
    RCLCPP_ERROR(logger_, "No message handler set for WebSocket server");
    return;
  }

  try {
    nlohmann::json response = message_handler_(msg->get_payload());
    sendResponse(hdl, response);
  } catch (const std::exception & e) {
    RCLCPP_ERROR(logger_, "Error handling WebSocket message: %s", e.what());

    // Send internal error response
    nlohmann::json error_response = {
      {"success", false},
      {"status", 500},
      {"message", "Internal server error"}
    };
    sendResponse(hdl, error_response);
  }
}

void WebSocketServerManager::sendResponse(connection_hdl hdl, const nlohmann::json & response)
{
  try {
    ws_server_.send(hdl, response.dump(), websocketpp::frame::opcode::text);
  } catch (const std::exception & e) {
    RCLCPP_ERROR(logger_, "Failed to send WebSocket response: %s", e.what());
  }
}

} // namespace remote_controller
