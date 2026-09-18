cmake_minimum_required(VERSION 3.16)

project(my_com VERSION 0.1.0 LANGUAGES CXX)

# C++17：Qt6 最低要求 C++17，任务规格亦指定
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 元对象编译（Q_OBJECT 类依赖 AUTOMOC）
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 本工具链没有 Qt SerialPort / Core5Compat 模块，只有 Widgets（串口走 Win32 API）
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# 纯 C++ 协议库（crc16/ymodem 等）：不依赖 Qt，方便单元测试单独链接
# 第二阶段工程师在此填实 YmodemSender 实现
add_library(mycom_proto STATIC
    src/upgrade/crc16.h
    src/upgrade/crc16.cpp
    src/upgrade/ymodem.h
    src/upgrade/ymodem.cpp
)
target_include_directories(mycom_proto PUBLIC src)

qt_add_executable(my_com MANUAL_FINALIZATION
    main.cpp
    resources.qrc                # 图标等资源 (AUTORCC)
    my_com.rc                    # Windows exe 图标 (windres)
    src/mainwindow.h
    src/mainwindow.cpp
    src/serial/serialconfig.h
    src/serial/serialconfig.cpp
    src/serial/serialport.h
    src/serial/serialport.cpp
    src/pages/terminalpage.h
    src/pages/terminalpage.cpp
    src/pages/debugpanel.h
    src/pages/debugpanel.cpp
    src/pages/upgradepage.h
    src/pages/upgradepage.cpp
    src/pages/upgradepage.ui       # 静态界面骨架 (AUTOUIC → ui_upgradepage.h)
    src/config/appconfig.h
    src/config/appconfig.cpp
    src/debugcmd/debugcommand.h
    src/debugcmd/debugcommand.cpp
    src/debugcmd/commandeditor.h
    src/debugcmd/commandeditor.cpp
    src/upgrade/upgradeengine.h
    src/upgrade/upgradeengine.cpp
)

# 桌面 GUI 程序：不弹控制台窗口
set_target_properties(my_com PROPERTIES WIN32_EXECUTABLE TRUE)

target_include_directories(my_com PRIVATE src)
target_link_libraries(my_com PRIVATE
    Qt6::Widgets
    mycom_proto
    setupapi   # SetupAPI：枚举串口设备与友好名
    winmm      # timeBeginPeriod：把等待超时分辨率提升到 1ms（协议要求 ≤10ms）
)
qt_finalize_executable(my_com)

# 调试指令表：运行期由“调试指令”页加载，复制到可执行文件目录
add_custom_command(TARGET my_com POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/commands/debug_commands.json
        $<TARGET_FILE_DIR:my_com>/debug_commands.json
    COMMENT "Copying commands/debug_commands.json next to my_com.exe"
)

# ---- 单元测试：默认不构建（用户要求：日常开发 Qt Creator 里只看主程序）----
# 想跑测试时开启：cmake 配置加 -DMYCOM_BUILD_TESTS=ON，或 Qt Creator 项目设置里
# 勾选 MYCOM_BUILD_TESTS。测试仍完整保留在仓库，只是不参与日常构建。
option(MYCOM_BUILD_TESTS "构建单元测试（my_com_tests / my_com_qt_tests）" OFF)
if(MYCOM_BUILD_TESTS)
    # 协议库（纯 C++）测试；YMODEM 状态机用假 IO 打桩，不依赖真实串口
    enable_testing()
    add_executable(my_com_tests
        tests/test_main.cpp
        tests/test_ymodem.cpp
        tests/test_crc16.cpp
    )
    target_include_directories(my_com_tests PRIVATE src)
    target_link_libraries(my_com_tests PRIVATE mycom_proto)
    add_test(NAME my_com_tests COMMAND my_com_tests)

    # Qt 层单元测试（T4）：DebugCommandModel / SerialConfig / DebugPanel（经 AUTOMOC）。
    # 输出到独立子目录 build/.../tests —— DebugPanel 的“JSON 缺失回退默认集”路径
    # 依赖 exe 目录无 debug_commands.json，独立目录保证该路径确定性可测。
    add_executable(my_com_qt_tests
        tests/qt_test_main.cpp
        tests/test_debugcmd.cpp
        tests/test_serialconfig.cpp
        tests/test_debugpanel.cpp
        src/debugcmd/debugcommand.cpp
        src/debugcmd/commandeditor.h
        src/debugcmd/commandeditor.cpp
        src/serial/serialconfig.cpp
        src/pages/debugpanel.cpp
    )
    set_target_properties(my_com_qt_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
    )
    target_include_directories(my_com_qt_tests PRIVATE src)
    target_link_libraries(my_com_qt_tests PRIVATE Qt6::Widgets)
    add_test(NAME my_com_qt_tests COMMAND my_com_qt_tests)

    # 测试代码自我要求更严：-Wall -Wextra（业务代码编译参数不动，保持与交付一致；
    # 业务源码的告警扫描见 T4 测试报告“静态检查”一节）
    target_compile_options(my_com_tests PRIVATE -Wall -Wextra)
    target_compile_options(my_com_qt_tests PRIVATE -Wall -Wextra)
endif()
