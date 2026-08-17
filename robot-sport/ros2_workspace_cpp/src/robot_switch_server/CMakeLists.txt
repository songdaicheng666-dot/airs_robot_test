cmake_minimum_required(VERSION 3.16)
project(robot_switch_server)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(robot_adapter_interfaces REQUIRED)
find_package(Threads REQUIRED)
find_package(httplib REQUIRED)

# 告警基线 + -Werror 开关:含义同 robot_adapter_interfaces,全工作区统一(本地默认关,CI strict 才开)
option(ROBOT_SPORT_WERROR "CI strict 门禁:把编译告警升级为错误" OFF)
set(ROBOT_SPORT_WARN_OPTS
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;$<$<BOOL:${ROBOT_SPORT_WERROR}>:-Werror>>)

set(TARGET_NAME robot_switch_server_node)
set(TARGET_SRCS
  src/main.cpp
  src/core/adapter_runtime_manager.cpp
  src/core/adapter_process_supervisor.cpp
  src/core/adapter_state_machine.cpp
  src/utils/json_utils.cpp
  src/http/json_response_builder.cpp
  src/infra/http_server_runner_httplib.cpp
)

add_executable(${TARGET_NAME} ${TARGET_SRCS})
target_compile_features(${TARGET_NAME} PRIVATE cxx_std_17)
target_compile_options(${TARGET_NAME} PRIVATE ${ROBOT_SPORT_WARN_OPTS})
target_include_directories(${TARGET_NAME} PRIVATE include)
target_link_libraries(${TARGET_NAME}
  Threads::Threads
  httplib::httplib
)

ament_target_dependencies(${TARGET_NAME}
  rclcpp
  std_srvs
  ament_index_cpp
  robot_adapter_interfaces
)

install(TARGETS ${TARGET_NAME} DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  # 只让 cppcheck 进门禁:copyright/cpplint 跳过,其余风格项由 CI -E 过滤(详见 robot_adapter_interfaces 同段注释)
  set(ament_cmake_copyright_FOUND TRUE)
  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
