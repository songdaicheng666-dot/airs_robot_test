cmake_minimum_required(VERSION 3.16)
project(robot_adapter_interfaces VERSION 0.1.0)  # 版本与 package.xml <version> 对齐

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rcl_yaml_param_parser REQUIRED)
find_package(nlohmann_json REQUIRED)

# 告警基线:全工作区 6 个包统一 -Wall -Wextra -Wpedantic(仅 GNU/Clang 生效)。
# ROBOT_SPORT_WERROR=ON 时再追加 -Werror 把告警升级为错误——本地默认关,CI strict 门禁传
# -DROBOT_SPORT_WERROR=ON 打开(契合 CMAKE_STANDARD「本地不强制 -Werror、CI strict 才开」)。
# 用 target 绑定而非全局 add_compile_options:避免污染同一 colcon 构建里的其它包/第三方子目录。
option(ROBOT_SPORT_WERROR "CI strict 门禁:把编译告警升级为错误" OFF)
set(ROBOT_SPORT_WARN_OPTS
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;$<$<BOOL:${ROBOT_SPORT_WERROR}>:-Werror>>)

# SHARED:让 PRIVATE 的 nlohmann 被 .so 吸收不泄漏给下游,并使 VERSION/SOVERSION 生效(静态库会忽略)
add_library(${PROJECT_NAME} SHARED
  src/adapter_client.cpp
  src/adapter_node_base.cpp
  src/system_info.cpp
  src/types.cpp
)

target_include_directories(${PROJECT_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)
# 告警选项 PRIVATE:只约束本库自身编译,不随 .so 传播给下游
target_compile_options(${PROJECT_NAME} PRIVATE ${ROBOT_SPORT_WARN_OPTS})

# ROS 依赖在 public 头暴露(rclcpp 等),作用域 PUBLIC;加 PUBLIC 关键字让 ament 内部走 keyword 签名,
# 才能与下面 nlohmann 的 PRIVATE 共存(同 target 不可 plain/keyword 混用)。
ament_target_dependencies(${PROJECT_NAME}
  PUBLIC
    rclcpp
    std_srvs
    ament_index_cpp
    rcl_yaml_param_parser
)
# nlohmann 仅 .cpp 用,PRIVATE
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)

# 共享库 VERSION/SOVERSION -> lib*.so.0.1.0 + SONAME 软链 .so.0
set_target_properties(${PROJECT_NAME} PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

install(
  TARGETS ${PROJECT_NAME}
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

ament_export_targets(export_${PROJECT_NAME} HAS_LIBRARY_TARGET)
# nlohmann 是 PRIVATE、不传播,不 export
ament_export_dependencies(rclcpp std_srvs ament_index_cpp rcl_yaml_param_parser)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_lint_auto REQUIRED)

  # 直接编译 system_info.cpp 进测试目标(同 adapter_lynx 的 test_lynx_sdk_client 写法),
  # 避免链接整个依赖 rclcpp 的共享库——本测试只用到 SystemInfoBuilder + nlohmann。
  ament_add_gtest(test_system_info
    test/test_system_info.cpp
    src/system_info.cpp
  )
  target_compile_features(test_system_info PRIVATE cxx_std_17)
  target_compile_options(test_system_info PRIVATE ${ROBOT_SPORT_WARN_OPTS})
  target_include_directories(test_system_info PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(test_system_info
    nlohmann_json::nlohmann_json
  )

  # 只让 cppcheck 进门禁:copyright/cpplint 在此跳过;uncrustify/lint_cmake/xmllint 等风格项
  # 由 CI colcon test 的 -E 过滤(对存量代码必红、属风格债,暂不纳入门禁)。
  set(ament_cmake_copyright_FOUND TRUE)
  set(ament_cmake_cpplint_FOUND TRUE)
  # cppcheck 2.x 不会展开 GoogleTest 的 TEST 宏，会将第二个测试用例误报为
  # syntaxError。测试源仍由 ament_add_gtest 编译并运行；仅将其排除在 cppcheck
  # 的源码扫描之外，保留对生产 src/ 和 include/ 的静态分析。
  set(AMENT_LINT_AUTO_FILE_EXCLUDE "test/*")
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
