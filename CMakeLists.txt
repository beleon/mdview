cmake_minimum_required(VERSION 3.16)
project(mdview VERSION 1.1.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-cursor)
pkg_check_modules(XKB     REQUIRED xkbcommon)
pkg_check_modules(CAIRO   REQUIRED cairo)
pkg_check_modules(PANGO   REQUIRED pangocairo)
pkg_check_modules(GDKPIX  REQUIRED gdk-pixbuf-2.0)

# ==============================================================================
# Generate xdg-shell protocol (if not already present)
# ==============================================================================

find_program(WAYLAND_SCANNER wayland-scanner)
set(XDG_SHELL_XML "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
set(XDG_DECO_XML  "/usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")
set(PTR_GEST_XML  "/usr/share/wayland-protocols/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml")
set(CURSOR_SHAPE_XML  "/usr/share/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml")
set(TL_ICON_XML       "/usr/share/wayland-protocols/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml")

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocol)

add_custom_command(
    OUTPUT  ${PROTO_DIR}/xdg-shell-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_XML} ${PROTO_DIR}/xdg-shell-protocol.h
    DEPENDS ${XDG_SHELL_XML}
)

add_custom_command(
    OUTPUT  ${PROTO_DIR}/xdg-shell-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_XML} ${PROTO_DIR}/xdg-shell-protocol.c
    DEPENDS ${XDG_SHELL_XML}
)

add_custom_command(
    OUTPUT  ${PROTO_DIR}/xdg-decoration-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header ${XDG_DECO_XML} ${PROTO_DIR}/xdg-decoration-protocol.h
    DEPENDS ${XDG_DECO_XML}
)

add_custom_command(
    OUTPUT  ${PROTO_DIR}/xdg-decoration-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code ${XDG_DECO_XML} ${PROTO_DIR}/xdg-decoration-protocol.c
    DEPENDS ${XDG_DECO_XML}
)

add_custom_command(
    OUTPUT  ${PROTO_DIR}/pointer-gestures-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header ${PTR_GEST_XML} ${PROTO_DIR}/pointer-gestures-protocol.h
    DEPENDS ${PTR_GEST_XML}
)

add_custom_command(
    OUTPUT  ${PROTO_DIR}/pointer-gestures-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code ${PTR_GEST_XML} ${PROTO_DIR}/pointer-gestures-protocol.c
    DEPENDS ${PTR_GEST_XML}
)

add_custom_command(
    OUTPUT  ${PROTO_DIR}/cursor-shape-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header ${CURSOR_SHAPE_XML} ${PROTO_DIR}/cursor-shape-protocol.h
    DEPENDS ${CURSOR_SHAPE_XML}
)

add_custom_command(
    OUTPUT  ${PROTO_DIR}/cursor-shape-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code ${CURSOR_SHAPE_XML} ${PROTO_DIR}/cursor-shape-protocol.c
    DEPENDS ${CURSOR_SHAPE_XML}
)

add_custom_command(
    OUTPUT  ${PROTO_DIR}/xdg-toplevel-icon-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header ${TL_ICON_XML} ${PROTO_DIR}/xdg-toplevel-icon-protocol.h
    DEPENDS ${TL_ICON_XML}
)

add_custom_command(
    OUTPUT  ${PROTO_DIR}/xdg-toplevel-icon-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code ${TL_ICON_XML} ${PROTO_DIR}/xdg-toplevel-icon-protocol.c
    DEPENDS ${TL_ICON_XML}
)

# ==============================================================================
# litehtml (QuickJS-free)
# ==============================================================================

add_library(litehtml STATIC)

file(GLOB_RECURSE LITEHTML_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/litehtml/src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/litehtml/src/*.cpp
)
list(FILTER LITEHTML_SRC EXCLUDE REGEX "encodings\\.cpp$")

target_sources(litehtml PRIVATE ${LITEHTML_SRC})

target_include_directories(litehtml
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/litehtml/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/litehtml/include/litehtml
            ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/litehtml/src
            ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/litehtml/src/gumbo/include
            ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/litehtml/src/gumbo/include/gumbo
)

target_compile_definitions(litehtml PUBLIC LITEHTML_UTF8_ONLY)

# ==============================================================================
# lunasvg + plutovg (vendored under thirdparty/lunasvg/)
# Replaces librsvg → libxml2 → libicudata, dropping ~10-15 MB of anon RSS at
# the cost of slightly reduced SVG spec coverage (no filters/animations).
# ==============================================================================

set(_prev_shared_libs ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build lunasvg + plutovg as static libs" FORCE)
set(PLUTOVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/lunasvg EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS ${_prev_shared_libs} CACHE BOOL "" FORCE)

# ==============================================================================
# md4c
# ==============================================================================

add_library(md4c STATIC
    thirdparty/md4c.c
    thirdparty/md4c-html.c
    thirdparty/entity.c
)

target_include_directories(md4c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)

# ==============================================================================
# mdview
# ==============================================================================

add_executable(mdview
    src/main.cpp
    src/window.c
    src/icons_data.c
    ${PROTO_DIR}/xdg-shell-protocol.c
    ${PROTO_DIR}/xdg-shell-protocol.h
    ${PROTO_DIR}/xdg-decoration-protocol.c
    ${PROTO_DIR}/xdg-decoration-protocol.h
    ${PROTO_DIR}/pointer-gestures-protocol.c
    ${PROTO_DIR}/pointer-gestures-protocol.h
    ${PROTO_DIR}/cursor-shape-protocol.c
    ${PROTO_DIR}/cursor-shape-protocol.h
    ${PROTO_DIR}/cursor-shape-stub.c
    ${PROTO_DIR}/xdg-toplevel-icon-protocol.c
    ${PROTO_DIR}/xdg-toplevel-icon-protocol.h
)

target_include_directories(mdview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${WAYLAND_INCLUDE_DIRS}
    ${XKB_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${PANGO_INCLUDE_DIRS}
    ${GDKPIX_INCLUDE_DIRS}
)

target_link_libraries(mdview PRIVATE
    litehtml
    md4c
    lunasvg::lunasvg
    ${WAYLAND_LIBRARIES}
    ${XKB_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${PANGO_LIBRARIES}
    ${GDKPIX_LIBRARIES}
)
