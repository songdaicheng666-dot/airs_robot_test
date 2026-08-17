cmake_minimum_required(VERSION 3.16)
project(adapter_lynx)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(robot_adapter_interfaces REQUIRED)
find_package(Threads REQUIRED)

# 告警基线 + -Werror 开关:全工作区统一(本地默认关,CI strict 才开)
option(ROBOT_SPORT_WERROR "CI strict 门禁:把编译告警升级为错误" OFF)
set(ROBOT_SPORT_WARN_OPTS
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;$<$<BOOL:${ROBOT_SPORT_WERROR}>:-Werror>>)

add_library(adapter_lynx_velocity_conversion STATIC
  src/lynx_velocity_converter.cpp
)
target_compile_features(adapter_lynx_velocity_conversion PUBLIC cxx_std_17)
target_compile_options(adapter_lynx_velocity_conversion PRIVATE ${ROBOT_SPORT_WARN_OPTS})
target_include_directories(adapter_lynx_velocity_conversion PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

add_executable(adapter_lynx_node
  src/main.cpp
  src/lynx_adapter_node.cpp
  src/lynx_sdk_client.cpp
)
target_compile_features(adapter_lynx_node PRIVATE cxx_std_17)
target_compile_options(adapter_lynx_node PRIVATE ${ROBOT_SPORT_WARN_OPTS})
target_include_directories(adapter_lynx_node PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
ament_target_dependencies(adapter_lynx_node
  rclcpp
  std_srvs
  geometry_msgs
  robot_adapter_interfaces
)
target_link_libraries(adapter_lynx_node
  adapter_lynx_velocity_conversion
  nlohmann_json::nlohmann_json
  Threads::Threads
)

install(TARGETS adapter_lynx_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_lint_auto REQUIRED)

  ament_add_gtest(test_lynx_velocity_converter
    test/test_lynx_velocity_converter.cpp
  )
  target_compile_features(test_lynx_velocity_converter PRIVATE cxx_std_17)
  target_compile_options(test_lynx_velocity_converter PRIVATE ${ROBOT_SPORT_WARN_OPTS})
  target_link_libraries(test_lynx_velocity_converter
    adapter_lynx_velocity_conversion
  )

  ament_add_gtest(test_lynx_sdk_client
    test/test_lynx_sdk_client.cpp
    src/lynx_sdk_client.cpp
  )
  target_compile_features(test_lynx_sdk_client PRIVATE cxx_std_17)
  target_compile_options(test_lynx_sdk_client PRIVATE ${ROBOT_SPORT_WARN_OPTS})
  target_include_directories(test_lynx_sdk_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(test_lynx_sdk_client
    nlohmann_json::nlohmann_json
    Threads::Threads
  )

  # lint 项中只让 cppcheck 进门禁:copyright/cpplint 跳过,其余风格项由 CI -E 过滤(详见 robot_adapter_interfaces 同段注释)
  set(ament_cmake_copyright_FOUND TRUE)
  set(ament_cmake_cpplint_FOUND TRUE)
  # cppcheck 2.x 不会展开 GoogleTest 的 TEST 宏，会将第二个测试用例误报为
  # syntaxError。测试源仍由 ament_add_gtest 编译并运行；仅将其排除在 cppcheck
  # 的源码扫描之外，保留对生产 src/ 和 include/ 的静态分析。
  set(AMENT_LINT_AUTO_FILE_EXCLUDE "test/*")
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
