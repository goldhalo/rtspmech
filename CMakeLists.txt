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

# 安装目录规范。必须早于 target 定义：$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
# 里的变量是立即展开的，晚定义会展开成空串。
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# 1. 输出目录
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# 静态库统一放 build/lib —— 与 netmech / modbusmech / rtpmech / mpegmech 一致。
# 之前本仓的 .a（含 picohttpparser）落在构建目录根上、netmech 的落在
# build/lib/，写链接命令时得来回切目录。
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

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

# 位置无关代码。
#
# 本库只出静态库，但静态库常常被链进 .so。缺 -fPIC 时，编静态库、跑单测都
# 正常，**只在最终链 .so 那一步**报：
#     relocation R_X86_64_PC32 against symbol `...' can not be used when
#     making a shared object; recompile with -fPIC
# 这种"晚一步才炸"的坑，只能靠默认打开来避免。纯静态场景可 -DRTSPMECH_PIC=OFF。
option(RTSPMECH_PIC "编译为位置无关代码（静态库要链进 .so 时必须开）" ON)

add_library(rtspmech_compile_options INTERFACE)
# 多个选项用 ';' 分隔：空格写法只有 Makefile/Ninja 才恰好正确，
# VS 生成器会把整个字符串原样塞进 AdditionalOptions。
target_compile_options(rtspmech_compile_options INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8;/W4;/permissive-;/Zc:__cplusplus>
    $<$<C_COMPILER_ID:MSVC>:/utf-8;/W3>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra>
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
# 只在 src/parser.cpp 里用（PRIVATE），但静态库场景下 PRIVATE 依赖仍会以
# $<LINK_ONLY:> 形式进入导出的 interface，所以它也必须能被安装/导出。
target_include_directories(picohttpparser PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/picohttpparser>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/rtspmech/thirdparty>)
# 第三方静态库同样会被链进 .so，一样要 PIC
set_target_properties(picohttpparser PROPERTIES
    FOLDER "rtspmech/thirdparty"
    POSITION_INDEPENDENT_CODE ${RTSPMECH_PIC})
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
set_target_properties(rtspmech PROPERTIES
    POSITION_INDEPENDENT_CODE ${RTSPMECH_PIC})

target_include_directories(rtspmech PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rtspmech
    PUBLIC  netmech
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
# 8. 安装与导出
# ---------------------------------------------------------------------------
#  装完之后使用者只需要：
#      find_package(rtspmech REQUIRED)
#      target_link_libraries(app PRIVATE rtspmech::rtspmech)
#  netmech、picohttpparser、Threads 都由导出的 target 带着走。
#
#  注意：netmech / rtspmech 的公共头都暴露了对方的类型
#  （session.hpp 直接 #include <netmech/conn.hpp>），所以 netmech 那个包
#  必须一起装 —— 由 Config 里的 find_dependency 自动找。
#
#  默认前缀：Linux -> /usr/local，Windows -> C:/Program Files/rtspmech
# ---------------------------------------------------------------------------
install(TARGETS rtspmech
        EXPORT rtspmechTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# vendored 的 C 解析库与编译选项载体：都是 static/shared 目标的链接期依赖
install(TARGETS picohttpparser rtspmech_compile_options
        EXPORT rtspmechTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY include/rtspmech
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

install(DIRECTORY thirdparty/picohttpparser/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rtspmech/thirdparty
        FILES_MATCHING PATTERN "*.h")

install(EXPORT rtspmechTargets
        FILE rtspmechTargets.cmake
        NAMESPACE rtspmech::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtspmech)

configure_package_config_file(cmake/rtspmechConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/rtspmechConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtspmech)

write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/rtspmechConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/rtspmechConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/rtspmechConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtspmech)

install(FILES LICENSE NOTICE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/rtspmech)

# ---------------------------------------------------------------------------
# 9. 汇总
# ---------------------------------------------------------------------------
message(STATUS "----------------------------------------------------------")
message(STATUS "  rtspmech ${PROJECT_VERSION}")
message(STATUS "    构建类型      : ${CMAKE_BUILD_TYPE}")
message(STATUS "    编译器        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "    netmech       : ${RTSPMECH_NETMECH_DIR}")
message(STATUS "    输出目录      : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "----------------------------------------------------------")
