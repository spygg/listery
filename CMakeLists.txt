cmake_minimum_required(VERSION 3.16)

project(Listery VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
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
    src/Core/LaunchService.h
    src/Core/LaunchService.cpp
    src/Core/UsageRecorder.h
    src/Core/UsageRecorder.cpp

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

set_target_properties(listery PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE FALSE
)

target_include_directories(listery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(listery PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::Network
)

if(WIN32)
    target_link_libraries(listery PRIVATE shell32 user32 ole32 advapi32)
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

include(GNUInstallDirs)
install(TARGETS listery RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
