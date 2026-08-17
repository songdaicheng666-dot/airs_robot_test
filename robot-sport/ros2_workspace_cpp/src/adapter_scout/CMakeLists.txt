cmake_minimum_required(VERSION 3.16)
project(adapter_scout)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(robot_adapter_interfaces REQUIRED)

option(ROBOT_SPORT_WERROR "CI strict gate: promote warnings to errors" OFF)
set(ROBOT_SPORT_WARN_OPTS
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;$<$<BOOL:${ROBOT_SPORT_WERROR}>:-Werror>>)

add_library(adapter_scout_command_codec STATIC
  src/scout_command_codec.cpp
)
target_compile_features(adapter_scout_command_codec PUBLIC cxx_std_17)
target_compile_options(adapter_scout_command_codec PRIVATE ${ROBOT_SPORT_WARN_OPTS})
target_include_directories(adapter_scout_command_codec PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

add_library(adapter_scout_can_client STATIC
  src/scout_can_client.cpp
)
target_compile_features(adapter_scout_can_client PUBLIC cxx_std_17)
target_compile_options(adapter_scout_can_client PRIVATE ${ROBOT_SPORT_WARN_OPTS})
target_include_directories(adapter_scout_can_client PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(adapter_scout_can_client
  adapter_scout_command_codec
)

add_executable(adapter_scout_node
  src/main.cpp
  src/scout_adapter_node.cpp
)
target_compile_features(adapter_scout_node PRIVATE cxx_std_17)
target_compile_options(adapter_scout_node PRIVATE ${ROBOT_SPORT_WARN_OPTS})
target_include_directories(adapter_scout_node PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
ament_target_dependencies(adapter_scout_node
  rclcpp
  std_srvs
  geometry_msgs
  robot_adapter_interfaces
)
target_link_libraries(adapter_scout_node
  adapter_scout_command_codec
  adapter_scout_can_client
  nlohmann_json::nlohmann_json
)

install(TARGETS adapter_scout_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_lint_auto REQUIRED)

  ament_add_gtest(test_scout_command_codec
    test/test_scout_command_codec.cpp
  )
  target_compile_features(test_scout_command_codec PRIVATE cxx_std_17)
  target_compile_options(test_scout_command_codec PRIVATE ${ROBOT_SPORT_WARN_OPTS})
  target_link_libraries(test_scout_command_codec
    adapter_scout_command_codec
  )

  ament_add_gtest(test_scout_can_client
    test/test_scout_can_client.cpp
  )
  target_compile_features(test_scout_can_client PRIVATE cxx_std_17)
  target_compile_options(test_scout_can_client PRIVATE ${ROBOT_SPORT_WARN_OPTS})
  target_link_libraries(test_scout_can_client
    adapter_scout_can_client
  )

  set(ament_cmake_copyright_FOUND TRUE)
  set(ament_cmake_cpplint_FOUND TRUE)
  # The workspace follows its repository-wide formatting rules, not the ROS
  # uncrustify defaults registered by ament_lint_auto.
  set(ament_cmake_uncrustify_FOUND TRUE)
  set(AMENT_LINT_AUTO_FILE_EXCLUDE "test/*")
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
