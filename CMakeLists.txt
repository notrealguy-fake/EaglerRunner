cmake_minimum_required(VERSION 3.20)
project(EaglerRunner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static CRT by default -> single self-contained .exe, no VC++ redistributable needed
# (great for potato PCs / clean machines). Set EAGLER_SHARED_CRT=ON to use the DLL CRT.
option(EAGLER_SHARED_CRT "Use the dynamic MSVC runtime (/MD) instead of static (/MT)" OFF)

if(MSVC)
  if(EAGLER_SHARED_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_compile_options(/utf-8 /W3 /EHsc /permissive- /MP)
  add_compile_options($<$<CONFIG:Release>:/O2>)
  add_compile_options($<$<CONFIG:Release>:/GL>)
  add_compile_options($<$<CONFIG:Release>:/Gy>)
  add_compile_options($<$<CONFIG:Release>:/Gw>)
  add_compile_options($<$<CONFIG:Release>:/fp:fast>)
  add_compile_options($<$<CONFIG:Release>:/DNDEBUG>)
  add_link_options($<$<CONFIG:Release>:/LTCG>)
  add_link_options($<$<CONFIG:Release>:/OPT:REF>)
  add_link_options($<$<CONFIG:Release>:/OPT:ICF>)
  # A map file lets crash reports (see crash.log) be resolved to functions.
  add_link_options($<$<CONFIG:Release>:/MAP>)
endif()

set(WEBVIEW2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2")

if(NOT EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
  message(FATAL_ERROR
    "WebView2 SDK not found at ${WEBVIEW2_DIR}.\n"
    "Download Microsoft.Web.WebView2 from nuget.org and extract it there, "
    "or run third_party/fetch_webview2.ps1")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(WEBVIEW2_ARCH "x64")
else()
  set(WEBVIEW2_ARCH "x86")
endif()

add_executable(EaglerRunner
  src/main.cpp
  src/net_bridge.cpp
  src/util.cpp
  src/settings.cpp
  src/library.cpp
  src/downloader.cpp
  src/app.rc
)

# Use wWinMain as entry point and set subsystem to Windows (GUI)
# Disable linker's automatic manifest generation (we provide one in app.rc)
target_link_options(EaglerRunner PRIVATE /ENTRY:wWinMainCRTStartup /SUBSYSTEM:WINDOWS /MANIFEST:NO)

# Recompile the resources when the icon or manifest changes.
set_property(SOURCE src/app.rc PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/app.ico;${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")

target_include_directories(EaglerRunner PRIVATE
  "${WEBVIEW2_DIR}/build/native/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(EaglerRunner PRIVATE
  "${WEBVIEW2_DIR}/build/native/${WEBVIEW2_ARCH}/WebView2LoaderStatic.lib"
  ws2_32
  comctl32
  comdlg32
  shlwapi
  ole32
  shell32
  user32
  gdi32
  advapi32
  version
)

# Ship the Minecraft-style launcher UI next to the executable.
add_custom_command(TARGET EaglerRunner POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/resources"
          "$<TARGET_FILE_DIR:EaglerRunner>/resources"
  COMMENT "Copying launcher resources"
)

if(MSVC)
  # Keep the binary small: no PDB in the shipped Release build.
  target_compile_options(EaglerRunner PRIVATE $<$<CONFIG:Release>:/Zi->)
endif()

message(STATUS "EaglerRunner: WebView2 SDK ${WEBVIEW2_ARCH} from ${WEBVIEW2_DIR}")
