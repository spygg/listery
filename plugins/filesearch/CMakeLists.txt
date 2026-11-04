# 文件搜索插件 / File search plugin（仅 Windows）

add_library(listery_filesearch MODULE
    filesearch.cpp
    file_index.cpp
    file_scanner.cpp
)

set_target_properties(listery_filesearch PROPERTIES
    OUTPUT_NAME "filesearch"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)

if(WIN32)
    set_target_properties(listery_filesearch PROPERTIES PREFIX "" SUFFIX ".dll")
endif()

target_include_directories(listery_filesearch PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk"
)

target_compile_definitions(listery_filesearch PRIVATE
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(listery_filesearch PRIVATE
    shell32
)

if(MSVC)
    target_compile_options(listery_filesearch PRIVATE /utf-8 /W3)
else()
    target_compile_options(listery_filesearch PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# 开发用基准工具：不参与默认构建，用
#   cmake --build <build> --target filesearch_bench
# 手动编译。
add_executable(filesearch_bench EXCLUDE_FROM_ALL
    filesearch_bench.cpp
    file_index.cpp
    file_scanner.cpp
)

set_target_properties(filesearch_bench PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_compile_definitions(filesearch_bench PRIVATE
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(filesearch_bench PRIVATE
    shell32
)

if(MSVC)
    target_compile_options(filesearch_bench PRIVATE /utf-8 /W3 /Zi)
    target_link_options(filesearch_bench PRIVATE /DEBUG)
else()
    target_compile_options(filesearch_bench PRIVATE -Wall -Wextra -Wno-unused-parameter -g)
endif()

# 端到端测试：直接加载 filesearch.dll，模拟宿主回调。仅开发使用。
add_executable(filesearch_plugin_test EXCLUDE_FROM_ALL
    filesearch_plugin_test.cpp
)

set_target_properties(filesearch_plugin_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_include_directories(filesearch_plugin_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk"
)

target_compile_definitions(filesearch_plugin_test PRIVATE
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(filesearch_plugin_test PRIVATE
    shell32
)

if(MSVC)
    target_compile_options(filesearch_plugin_test PRIVATE /utf-8 /W3)
else()
    target_compile_options(filesearch_plugin_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
