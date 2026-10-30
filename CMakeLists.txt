cmake_minimum_required(VERSION 3.16)

project(Listery VERSION 1.3.1 LANGUAGES CXX)

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

add_executable(listery ${PROJECT_SOURCES})

# Windows 下把正式图标编进 exe（资源管理器/任务栏显示，而非空白 exe）
if(WIN32)
    target_sources(listery PRIVATE src/resources/appicon.rc)
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

# 插件与主程序一起构建（输出到 build/plugins/，主程序从 exe 同目录的 plugins/ 加载）
add_subdirectory(plugins)

include(GNUInstallDirs)
install(TARGETS listery RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY "${CMAKE_BINARY_DIR}/plugins/" DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins"
        FILES_MATCHING PATTERN "*.dll" PATTERN "*.so" PATTERN "*.dylib")
