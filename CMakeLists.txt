# =============================================================================
#  rtspmech —— 基于 netmech 的 RTSP 协议胶水层（C++17）
# =============================================================================
#  依赖：
#    * netmech（git submodule，位于 thirdparty/netmech）
#    * picohttpparser（vendored，thirdparty/picohttpparser）
# =============================================================================
cmake_minimum_required(VERSION 3.16)

project(rtspmech
        VERSION 0.9.0
        DESCRIPTION "RTSP protocol glue layer built on netmech"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# 默认 Release（个人项目，Debug 显式指定）
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# 1. 输出目录
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# ---------------------------------------------------------------------------
# 2. 定位 netmech
# ---------------------------------------------------------------------------
set(RTSPMECH_NETMECH_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/netmech"
    CACHE PATH "netmech 所在目录（默认使用 git submodule）")

set(RTSPMECH_RTPMECH_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../rtpmech"
    CACHE PATH "rtpmech 所在目录（可选，仅 rtsp_server 示例需要）")

if(NOT EXISTS "${RTSPMECH_NETMECH_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "找不到 netmech：${RTSPMECH_NETMECH_DIR}\n"
        "请先：git submodule update --init --recursive\n"
        "   或指定：cmake -DRTSPMECH_NETMECH_DIR=<netmech 路径> ...")
endif()

set(NETMECH_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(NETMECH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${RTSPMECH_NETMECH_DIR}" "${CMAKE_BINARY_DIR}/netmech" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# 3. 构建选项
# ---------------------------------------------------------------------------
option(RTSPMECH_BUILD_TESTS       "构建 rtspmech 单元测试" ON)
option(RTSPMECH_BUILD_EXAMPLES    "构建 rtspmech 示例" ON)
option(RTSPMECH_WARNINGS_AS_ERRORS "把编译告警当错误（CI 用）" OFF)

add_library(rtspmech_compile_options INTERFACE)
target_compile_options(rtspmech_compile_options INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /W4 /permissive- /Zc:__cplusplus>
    $<$<C_COMPILER_ID:MSVC>:/utf-8 /W3>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)
if(RTSPMECH_WARNINGS_AS_ERRORS)
    target_compile_options(rtspmech_compile_options INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>
    )
endif()

# ---------------------------------------------------------------------------
# 4. picohttpparser（C）
# ---------------------------------------------------------------------------
add_library(picohttpparser STATIC
    thirdparty/picohttpparser/picohttpparser.c)
target_include_directories(picohttpparser PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/picohttpparser")
set_target_properties(picohttpparser PROPERTIES FOLDER "rtspmech/thirdparty")
target_compile_options(picohttpparser PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/wd4244 /wd4267>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>
)

# ---------------------------------------------------------------------------
# 5. 主库
# ---------------------------------------------------------------------------
file(GLOB RTSPMECH_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(rtspmech ${RTSPMECH_SOURCES})
add_library(rtspmech::rtspmech ALIAS rtspmech)

target_include_directories(rtspmech PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rtspmech
    PUBLIC  NetMechanism
    PRIVATE picohttpparser
    PRIVATE rtspmech_compile_options)

set_target_properties(rtspmech PROPERTIES FOLDER "rtspmech")

# ---------------------------------------------------------------------------
# 6. 测试
# ---------------------------------------------------------------------------
if(RTSPMECH_BUILD_TESTS)
    enable_testing()
    file(GLOB RTSPMECH_TEST_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/test/*.cpp")
    if(RTSPMECH_TEST_SOURCES)
        add_executable(rtspmech_ut ${RTSPMECH_TEST_SOURCES})
        set_target_properties(rtspmech_ut PROPERTIES FOLDER "rtspmech/test")
        target_include_directories(rtspmech_ut PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/test"
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${RTSPMECH_NETMECH_DIR}/test")
        target_link_libraries(rtspmech_ut PRIVATE rtspmech rtspmech_compile_options)
        add_test(NAME rtspmech_ut COMMAND rtspmech_ut)
    endif()
endif()

# ---------------------------------------------------------------------------
# 7. 示例
# ---------------------------------------------------------------------------
if(RTSPMECH_BUILD_EXAMPLES)
    file(GLOB RTSPMECH_EXAMPLE_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/example/*.cpp")

    foreach(src ${RTSPMECH_EXAMPLE_SOURCES})
        get_filename_component(name ${src} NAME_WE)

        # rtsp_server 依赖 rtpmech（H264Packetizer）；没有 rtpmech 就跳过它
        if(name STREQUAL "rtsp_server" OR name STREQUAL "rtsp_pusher")
            if(NOT EXISTS "${RTSPMECH_RTPMECH_DIR}/include/rtpmech/rtp_packet.hpp")
                message(STATUS "跳过 rtsp_server 示例（未找到 rtpmech：${RTSPMECH_RTPMECH_DIR}）")
                continue()
            endif()
            # 直接把 rtpmech 用到的几个源文件编进来（不引入它的完整工程，
            # 否则会重复 add_subdirectory(netmech)）。
            # h264.cpp / rtp_packet.cpp / error.cpp 都不依赖 netmech。
            add_executable(${name} ${src}
                "${RTSPMECH_RTPMECH_DIR}/src/h264.cpp"
                "${RTSPMECH_RTPMECH_DIR}/src/h265.cpp"
                "${RTSPMECH_RTPMECH_DIR}/src/rtp_packet.cpp"
                "${RTSPMECH_RTPMECH_DIR}/src/rtcp_packet.cpp"
                "${RTSPMECH_RTPMECH_DIR}/src/error.cpp")
            set_target_properties(${name} PROPERTIES FOLDER "rtspmech/example")
            target_include_directories(${name} PRIVATE
                "${RTSPMECH_RTPMECH_DIR}/include")
            target_link_libraries(${name} PRIVATE rtspmech rtspmech_compile_options)
            continue()
        endif()

        add_executable(${name} ${src})
        set_target_properties(${name} PROPERTIES FOLDER "rtspmech/example")
        target_link_libraries(${name} PRIVATE rtspmech rtspmech_compile_options)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 8. 汇总
# ---------------------------------------------------------------------------
message(STATUS "----------------------------------------------------------")
message(STATUS "  rtspmech ${PROJECT_VERSION}")
message(STATUS "    构建类型      : ${CMAKE_BUILD_TYPE}")
message(STATUS "    编译器        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "    netmech       : ${RTSPMECH_NETMECH_DIR}")
message(STATUS "    输出目录      : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "----------------------------------------------------------")
