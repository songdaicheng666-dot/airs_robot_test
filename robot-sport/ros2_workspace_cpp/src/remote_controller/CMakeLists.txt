cmake_minimum_required(VERSION 3.8)
project(remote_controller)

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(websocketpp REQUIRED)
find_package(PahoMqttCpp REQUIRED)

# 告警基线 + -Werror 开关:全工作区统一(本地默认关,CI strict 才开)
option(ROBOT_SPORT_WERROR "CI strict 门禁:把编译告警升级为错误" OFF)
set(ROBOT_SPORT_WARN_OPTS
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;$<$<BOOL:${ROBOT_SPORT_WERROR}>:-Werror>>)

set(REMOTE_CONTROLLER_SOURCES
  src/remote_controller.cpp
  src/config.cpp
  src/device_info.cpp
  src/response.cpp
  src/validator.cpp
  src/velocity_processor.cpp
  src/websocket_server.cpp
  src/mqtt_subscriber.cpp
)

add_executable(remote_controller_node ${REMOTE_CONTROLLER_SOURCES})
target_compile_features(remote_controller_node PRIVATE cxx_std_17)
target_compile_options(remote_controller_node PRIVATE ${ROBOT_SPORT_WARN_OPTS})
ament_target_dependencies(remote_controller_node
  rclcpp
  geometry_msgs
)

target_include_directories(remote_controller_node PRIVATE
    include
)

target_link_libraries(remote_controller_node
  nlohmann_json::nlohmann_json
  websocketpp::websocketpp
  PahoMqttCpp::paho-mqttpp3
)

message(STATUS "PahoMqttCpp found - MQTT transport enabled")

install(TARGETS remote_controller_node
  DESTINATION lib/${PROJECT_NAME}
)

# Install configuration files
install(DIRECTORY config/
  DESTINATION share/${PROJECT_NAME}/config
)

if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  find_package(ament_lint_auto REQUIRED)
  find_package(ament_cmake_gtest REQUIRED)
  # 只让 cppcheck 进门禁:copyright/cpplint 跳过,其余风格项由 CI -E 过滤(详见 robot_adapter_interfaces 同段注释)
  set(ament_cmake_copyright_FOUND TRUE)
  set(ament_cmake_cpplint_FOUND TRUE)
  ament_add_gtest(test_config test/test_config.cpp src/config.cpp src/device_info.cpp)
  target_compile_features(test_config PRIVATE cxx_std_17)
  target_compile_options(test_config PRIVATE ${ROBOT_SPORT_WARN_OPTS})
  target_include_directories(test_config PRIVATE include)
  target_link_libraries(test_config nlohmann_json::nlohmann_json)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
