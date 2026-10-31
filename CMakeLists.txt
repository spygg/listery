cmake_minimum_required(VERSION 3.16)

project(Listery VERSION 1.3.4 LANGUAGES CXX)

# XP 兼容目标（MinGW 5.3）只支持 C++14，可用 -DLISTERY_CXX_STANDARD=14 覆盖
if(NOT LISTERY_CXX_STANDARD)
    set(LISTERY_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD ${LISTERY_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Sql Network)

set(PROJECT_SOURCES
    src/main.cpp

    src/AppController.h
    src/AppController.cpp
    src/SelfTest.h
    src/SelfTest.cpp

    src/Core/AppSettings.h
    src/Core/AppSettings.cpp
    src/Core/Database.h
    src/Core/Database.cpp
    src/Core/Scanner.h
    src/Core/Scanner.cpp
    src/Core/SearchEngine.h
    src/Core/SearchEngine.cpp
    src/Core/SearchService.h
    src/Core/SearchService.cpp
    src/Core/LaunchService.h
    src/Core/LaunchService.cpp
    src/Core/QtCompat.h
    src/Core/Localization.h
    src/Core/Localization.cpp
    src/Core/UsageRecorder.h
    src/Core/UsageRecorder.cpp
    src/Core/Pinyin.h
    src/Core/Pinyin.cpp
    src/Core/PinyinData.h
    src/Core/PinyinData.cpp
    src/Core/PluginHost.h
    src/Core/PluginHost.cpp
    src/Core/Updater.h
    src/Core/Updater.cpp
    src/Core/HttpClient.h
    src/Core/HttpClient.cpp

    src/Platform/HotkeyHook.h
    src/Platform/HotkeyHook.cpp
    src/Platform/Autostart.h
    src/Platform/Autostart.cpp
    src/Platform/DoubleCtrlDetector.h
    src/Platform/WindowUtils.h
    src/Platform/WindowUtils.cpp
    src/Platform/Branding.h
    src/Platform/Branding.cpp

    src/Ui/LauncherWindow.h
    src/Ui/LauncherWindow.cpp
    src/Ui/ResultModel.h
    src/Ui/ResultModel.cpp
    src/Ui/ResultDelegate.h
    src/Ui/ResultDelegate.cpp
    src/Ui/SettingsDialog.h
    src/Ui/SettingsDialog.cpp
)

if(WIN32)
    # Windows 下把正式图标编进 exe（资源管理器/任务栏显示，而非空白 exe）。
    # MinGW 不走 CMake 的 RC 语言：它按 PATH 自动挑资源编译器，CI runner 上会选中
    # MSVC 的 rc.exe，产出的对象 g++ 链接时报“file format not recognized”。
    # 这里直接调用与 g++ 同目录的 windres，保证资源对象与工具链配套。
    if(MINGW)
        get_filename_component(LISTERY_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_program(LISTERY_WINDRES NAMES windres HINTS "${LISTERY_MINGW_BIN}" REQUIRED)
        set(LISTERY_RC_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/listery_appicon.rc.o")
        add_custom_command(
            OUTPUT "${LISTERY_RC_SOURCE}"
            COMMAND "${LISTERY_WINDRES}" -O coff
                    -I "${CMAKE_CURRENT_SOURCE_DIR}/src/resources"
                    -i "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/appicon.rc"
                    -o "${LISTERY_RC_SOURCE}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/appicon.rc"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/appicon.ico"
            VERBATIM)
        set_source_files_properties("${LISTERY_RC_SOURCE}" PROPERTIES
            EXTERNAL_OBJECT TRUE GENERATED TRUE)
    else()
        enable_language(RC)
        set(LISTERY_RC_SOURCE src/resources/appicon.rc)
    endif()
endif()

add_executable(listery ${PROJECT_SOURCES})

if(WIN32)
    target_sources(listery PRIVATE "${LISTERY_RC_SOURCE}")
endif()

set_target_properties(listery PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE FALSE
)

target_include_directories(listery PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk
)

# 版本号从 project(VERSION) 注入，作为在线升级时与 GitHub Release 比较的基准
target_compile_definitions(listery PRIVATE LISTERY_VERSION="${Listery_VERSION}")

target_link_libraries(listery PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::Network
)

# 界面语言包：lrelease 把 .ts 编译成 .qm，再随资源嵌进 exe（部署时只有一个文件）。
# 没有安装 LinguistTools 时跳过：Localization 会退回简体中文，程序照常可用。
find_package(Qt${QT_VERSION_MAJOR} QUIET COMPONENTS LinguistTools)
if(TARGET Qt${QT_VERSION_MAJOR}::lrelease)
    set(LISTERY_LANGUAGES en fr zh_TW)
    set(LISTERY_QM_FILES)
    set(LISTERY_QRC_CONTENT "<RCC>\n    <qresource prefix=\"/i18n\">\n")
    foreach(lang ${LISTERY_LANGUAGES})
        set(ts "${CMAKE_CURRENT_SOURCE_DIR}/src/i18n/listery_${lang}.ts")
        set(qm "${CMAKE_CURRENT_BINARY_DIR}/i18n/listery_${lang}.qm")
        add_custom_command(
            OUTPUT "${qm}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/i18n"
            COMMAND Qt${QT_VERSION_MAJOR}::lrelease "${ts}" -qm "${qm}"
            DEPENDS "${ts}"
            COMMENT "lrelease listery_${lang}.ts"
            VERBATIM)
        list(APPEND LISTERY_QM_FILES "${qm}")
        string(APPEND LISTERY_QRC_CONTENT
               "        <file alias=\"listery_${lang}.qm\">${qm}</file>\n")
    endforeach()
    string(APPEND LISTERY_QRC_CONTENT "    </qresource>\n</RCC>\n")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/listery_translations.qrc" "${LISTERY_QRC_CONTENT}")

    add_custom_target(listery_translations ALL DEPENDS ${LISTERY_QM_FILES})
    target_sources(listery PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/listery_translations.qrc")
    add_dependencies(listery listery_translations)
else()
    message(WARNING "未找到 Qt LinguistTools：构建出的程序只有简体中文界面")
endif()

if(WIN32)
    # winhttp：更新检查与插件 HTTP 走 Windows 自带 TLS 栈，不依赖 OpenSSL DLL
    target_link_libraries(listery PRIVATE shell32 user32 ole32 advapi32 winhttp)
elseif(APPLE)
    find_library(APPLICATION_SERVICES ApplicationServices REQUIRED)
    target_link_libraries(listery PRIVATE ${APPLICATION_SERVICES})
elseif(UNIX)
    find_package(X11)
    if(X11_FOUND AND X11_XTest_LIB)
        target_compile_definitions(listery PRIVATE LISTERY_HAVE_X11)
        target_include_directories(listery PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(listery PRIVATE ${X11_LIBRARIES} ${X11_XTest_LIB})
    else()
        message(WARNING "未找到 X11/Xtst：Linux 上将无法注册全局双击 Ctrl，托盘与搜索功能仍可用")
    endif()
endif()

if(MSVC)
    target_compile_options(listery PRIVATE /utf-8 /W3)
else()
    target_compile_options(listery PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# 崩溃转储：仅 Windows/MSVC 桌面构建启用 Breakpad。
# XP(MinGW 5.3) 与 Linux/macOS 不启用：随附的 Breakpad 源码较旧，
# 在那些工具链上编译/链接风险高，会拖累发布流水线。
if(WIN32 AND MSVC)
    add_subdirectory(src/thirdparty/breakpad)
    target_link_libraries(listery PRIVATE listery_breakpad)
    target_compile_definitions(listery PRIVATE LISTERY_WITH_BREAKPAD=1)
endif()

# 插件与主程序一起构建（输出到 build/plugins/，主程序从 exe 同目录的 plugins/ 加载）
add_subdirectory(plugins)

include(GNUInstallDirs)
install(TARGETS listery RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY "${CMAKE_BINARY_DIR}/plugins/" DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins"
        FILES_MATCHING PATTERN "*.dll" PATTERN "*.so" PATTERN "*.dylib")
