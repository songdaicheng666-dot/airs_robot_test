cmake_minimum_required(VERSION 3.16)
project(adapter_fake)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(robot_adapter_interfaces REQUIRED)

# 告警基线 + -Werror 开关:全工作区统一(本地默认关,CI strict 才开)
option(ROBOT_SPORT_WERROR "CI strict 门禁:把编译告警升级为错误" OFF)
set(ROBOT_SPORT_WARN_OPTS
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;$<$<BOOL:${ROBOT_SPORT_WERROR}>:-Werror>>)

add_executable(adapter_fake_node src/adapter_fake_node.cpp)
target_compile_features(adapter_fake_node PRIVATE cxx_std_17)
target_compile_options(adapter_fake_node PRIVATE ${ROBOT_SPORT_WARN_OPTS})
ament_target_dependencies(adapter_fake_node
  rclcpp
  std_srvs
  robot_adapter_interfaces
)

install(TARGETS adapter_fake_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  # 只让 cppcheck 进门禁:copyright/cpplint 跳过,其余风格项由 CI -E 过滤(详见 robot_adapter_interfaces 同段注释)
  set(ament_cmake_copyright_FOUND TRUE)
  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
