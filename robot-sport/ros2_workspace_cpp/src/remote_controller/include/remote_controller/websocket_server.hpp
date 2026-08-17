#ifndef REMOTE_CONTROLLER_WEBSOCKET_SERVER_HPP
#define REMOTE_CONTROLLER_WEBSOCKET_SERVER_HPP

#include <functional>
#include <atomic>
#include <thread>
#include <set>
#include <memory>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <nlohmann/json.hpp>
#include "rclcpp/rclcpp.hpp"
#include "remote_controller/config.hpp"
#include <mutex>

namespace remote_controller
{

typedef websocketpp::server<websocketpp::config::asio> ws_server;
typedef ws_server::message_ptr ws_message_ptr;
typedef websocketpp::connection_hdl connection_hdl;

using MessageHandler = std::function<nlohmann::json(const std::string &)>;

class WebSocketServerManager
{
public:
  WebSocketServerManager(
    std::shared_ptr<ConfigManager> config_manager,
    rclcpp::Logger logger
  );

  ~WebSocketServerManager();

  // Server lifecycle
  bool start();
  void stop();
  bool isRunning() const {return server_running_;}

  // Message handling
  void setMessageHandler(MessageHandler handler) {message_handler_ = handler;}

  // Connection management
  size_t getConnectionCount() const
  {
    std::lock_guard<std::mutex> lock(connections_mutex_); return connections_.size();
  }

private:
  std::shared_ptr<ConfigManager> config_manager_;
  rclcpp::Logger logger_;

  ws_server ws_server_;
  std::thread server_thread_;
  std::set<connection_hdl, std::owner_less<connection_hdl>> connections_;
  mutable std::mutex connections_mutex_;
  std::atomic<bool> server_running_;

  MessageHandler message_handler_;

  // WebSocket event handlers
  void onOpen(connection_hdl hdl);
  void onClose(connection_hdl hdl);
  void onMessage(connection_hdl hdl, ws_message_ptr msg);

  // Helper methods
  void sendResponse(connection_hdl hdl, const nlohmann::json & response);
  void setupEventHandlers();
  void runServer();
};

} // namespace remote_controller

#endif // REMOTE_CONTROLLER_WEBSOCKET_SERVER_HPP
