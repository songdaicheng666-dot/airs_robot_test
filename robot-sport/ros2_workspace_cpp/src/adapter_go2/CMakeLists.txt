cmake_minimum_required(VERSION 3.16)
project(adapter_go2)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(robot_adapter_interfaces REQUIRED)

# 告警基线 + -Werror 开关:全工作区统一(本地默认关,CI strict 才开)
option(ROBOT_SPORT_WERROR "CI strict 门禁:把编译告警升级为错误" OFF)
set(ROBOT_SPORT_WARN_OPTS
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;$<$<BOOL:${ROBOT_SPORT_WERROR}>:-Werror>>)

option(ADAPTER_GO2_USE_LOCAL_UNITREE_SDK2 "Build adapter_go2 with bundled Unitree SDK2" ON)

if(ADAPTER_GO2_USE_LOCAL_UNITREE_SDK2)
  set(BUILD_EXAMPLES OFF CACHE BOOL "Build Unitree SDK examples" FORCE)
  add_subdirectory(unitree_sdk2)
endif()

add_executable(adapter_go2_node
  src/main.cpp
  src/go2_adapter_node.cpp
  src/go2_sdk_client.cpp
)
target_compile_features(adapter_go2_node PRIVATE cxx_std_17)
target_compile_options(adapter_go2_node PRIVATE ${ROBOT_SPORT_WARN_OPTS})
target_include_directories(adapter_go2_node PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
ament_target_dependencies(adapter_go2_node
  rclcpp
  std_srvs
  geometry_msgs
  robot_adapter_interfaces
)

if(ADAPTER_GO2_USE_LOCAL_UNITREE_SDK2)
  target_link_libraries(adapter_go2_node unitree_sdk2)
  # unitree_sdk2 是 add_subdirectory 进来的第三方,默认其头按普通 -I 暴露给我们;开 -Werror 后,
  # 第三方头里的告警会算到我们的 .cpp 头上、引爆门禁。把它的 interface 头标成 SYSTEM(走 -isystem),
  # 这样第三方头的告警不计入,而我们自己的 src 照常受 -Werror 约束。
  get_target_property(_unitree_inc unitree_sdk2 INTERFACE_INCLUDE_DIRECTORIES)
  if(_unitree_inc)
    set_target_properties(unitree_sdk2 PROPERTIES
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_unitree_inc}")
  endif()
  # Executable path: lib/adapter_go2/adapter_go2_node, SDK shared libs path: lib/
  set_target_properties(adapter_go2_node PROPERTIES INSTALL_RPATH "$ORIGIN/..")
endif()
target_link_libraries(adapter_go2_node nlohmann_json::nlohmann_json)

install(TARGETS adapter_go2_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  # 只让 cppcheck 进门禁:copyright/cpplint 跳过,其余风格项由 CI -E 过滤(详见 robot_adapter_interfaces 同段注释)
  set(ament_cmake_copyright_FOUND TRUE)
  set(ament_cmake_cpplint_FOUND TRUE)
  # 本包内含 unitree_sdk2 第三方子目录,lint_auto 的 cppcheck 会 GLOB 全目录扫到它。用
  # AMENT_LINT_AUTO_FILE_EXCLUDE 排除:它最终转成 cppcheck 的 --suppress 模式匹配,实测要写成
  # "unitree_sdk2/*" 这种相对通配才匹配得上 cppcheck 报告的相对路径(裸目录名 / "*/.../*" 都不生效)。
  set(AMENT_LINT_AUTO_FILE_EXCLUDE "unitree_sdk2/*")
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
