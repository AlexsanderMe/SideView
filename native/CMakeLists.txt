cmake_minimum_required(VERSION 3.24)
project(sideview_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sideview_native SHARED)
target_include_directories(sideview_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(sideview_native PRIVATE /W4)
else()
    target_compile_options(sideview_native PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_sources(sideview_native PRIVATE win32/webview_host.cpp)
    target_compile_definitions(sideview_native PRIVATE UNICODE NOMINMAX)

    if(NOT DEFINED WEBVIEW2_SDK_DIR)
        message(FATAL_ERROR "Set WEBVIEW2_SDK_DIR to the Microsoft.Web.WebView2 SDK package root.")
    endif()

    target_include_directories(
        sideview_native
        PRIVATE
        "${WEBVIEW2_SDK_DIR}/build/native/include"
    )

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(WEBVIEW2_ARCH "x64")
    else()
        set(WEBVIEW2_ARCH "x86")
    endif()

    target_link_libraries(
        sideview_native
        PRIVATE
        ole32
        user32
        shlwapi
        crypt32
        windowscodecs
        "${WEBVIEW2_SDK_DIR}/build/native/${WEBVIEW2_ARCH}/WebView2LoaderStatic.lib"
    )
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(sideview_native PRIVATE macos/webview_host.mm)
    target_compile_options(sideview_native PRIVATE -fobjc-arc)
    target_link_libraries(sideview_native PRIVATE "-framework Cocoa" "-framework WebKit")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(
        LINUX_WEBVIEW
        REQUIRED
        IMPORTED_TARGET
        webkit2gtk-4.1>=2.40
        gtk+-x11-3.0
        gdk-pixbuf-2.0
    )

    target_sources(sideview_native PRIVATE linux/webview_host.cpp)
    target_link_libraries(sideview_native PRIVATE PkgConfig::LINUX_WEBVIEW)
else()
    message(FATAL_ERROR "SideView supports Windows, macOS, and Linux.")
endif()

set_target_properties(sideview_native PROPERTIES
    OUTPUT_NAME "sideview_native"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

install(
    TARGETS sideview_native
    LIBRARY DESTINATION sideview
    RUNTIME DESTINATION sideview
)

if(APPLE)
    install(CODE [[
        set(installed_library "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/sideview/libsideview_native.dylib")
        execute_process(
            COMMAND codesign --force --sign - --timestamp=none "${installed_library}"
            RESULT_VARIABLE codesign_result
        )
        if(NOT codesign_result EQUAL 0)
            message(FATAL_ERROR "Failed to sign ${installed_library}")
        endif()
    ]])
endif()
