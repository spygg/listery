# 法汉翻译插件 / Translator plugin (FR <-> ZH)

add_library(listery_french MODULE french.cpp)

set_target_properties(listery_french PROPERTIES
    OUTPUT_NAME "french"
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)

if(WIN32)
    # Windows 上插件必须是 french.dll（不能有 lib 前缀）
    set_target_properties(listery_french PROPERTIES PREFIX "" SUFFIX ".dll")
endif()

target_include_directories(listery_french PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk"
)

if(MSVC)
    target_compile_options(listery_french PRIVATE /utf-8 /W3)
else()
    target_compile_options(listery_french PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
