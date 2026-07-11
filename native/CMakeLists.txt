cmake_minimum_required(VERSION 3.24)
project(native_webview_widget LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(native_webview_widget SHARED)
target_include_directories(native_webview_widget PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_sources(native_webview_widget PRIVATE win32/webview_host.cpp)
    target_compile_definitions(native_webview_widget PRIVATE UNICODE NOMINMAX)

    if(NOT DEFINED WEBVIEW2_SDK_DIR)
        message(FATAL_ERROR "Set WEBVIEW2_SDK_DIR to the Microsoft.Web.WebView2 SDK package root.")
    endif()

    target_include_directories(
        native_webview_widget
        PRIVATE
        "${WEBVIEW2_SDK_DIR}/build/native/include"
    )

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(WEBVIEW2_ARCH "x64")
    else()
        set(WEBVIEW2_ARCH "x86")
    endif()

    target_link_libraries(
        native_webview_widget
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
    target_sources(native_webview_widget PRIVATE macos/webview_host.mm)
    target_compile_options(native_webview_widget PRIVATE -fobjc-arc)
    target_link_libraries(native_webview_widget PRIVATE "-framework Cocoa" "-framework WebKit")
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

    target_sources(native_webview_widget PRIVATE linux/webview_host.cpp)
    target_link_libraries(native_webview_widget PRIVATE PkgConfig::LINUX_WEBVIEW)
else()
    message(FATAL_ERROR "native-webview-widget supports Windows, macOS, and Linux.")
endif()

set_target_properties(native_webview_widget PROPERTIES
    OUTPUT_NAME "native_webview_widget"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
