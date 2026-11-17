cmake_minimum_required(VERSION 4.4)
project(GTKdrop VERSION 0.2.0 LANGUAGES C CXX) # C for the generated GResource bundle

set(CMAKE_CXX_STANDARD 26)

add_executable(GTKdrop
        src/main.cpp
        src/main_window.cpp
        src/mod_item.cpp
        src/appearance.cpp
        src/dialogs.cpp
        src/core/json_util.cpp
        src/core/settings.cpp
        src/core/mod.cpp
        src/core/profile.cpp
        src/core/mod_config.cpp
        src/core/selected_mods.cpp
        src/core/steam.cpp
        src/core/stardrop_import.cpp
        src/core/manager.cpp
        src/core/launcher.cpp
        src/core/nexus.cpp
        src/core/payload.cpp
        src/core/http_download.cpp
        src/downloads.cpp
        src/nexus_browser.cpp
        src/core/process.cpp
        src/core/installer.cpp
        src/core/library.cpp
        src/core/nxm_handler.cpp
        src/log.cpp
        src/toast.cpp
)

# Debug builds add a stack trace after every GLib/GTK warning or critical (src/log.cpp).
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS -std=c++26)
set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
check_cxx_source_compiles("#include <stacktrace>
int main() { return std::stacktrace::current().size() > 0 ? 0 : 1; }" GTKDROP_HAVE_STACKTRACE)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LIBRARIES)
if (GTKDROP_HAVE_STACKTRACE)
    target_compile_definitions(GTKdrop PRIVATE $<$<CONFIG:Debug>:GTKDROP_STACKTRACES>)
    target_link_libraries(GTKdrop PRIVATE $<$<CONFIG:Debug>:stdc++exp>)
endif ()

# ---------------------------------------------------------------------------
# GTK4 + libadwaita: use the system installations if pkg-config finds them,
# otherwise download and build them (plus any missing deps, via meson wraps)
# into a shared prefix in the build tree at configure time.
# ---------------------------------------------------------------------------
option(GTKDROP_FORCE_FETCH_DEPS "Ignore system GTK/libadwaita and always build them from source" OFF)
set(GTKDROP_GTK_TAG "4.18.6" CACHE STRING "GTK git tag to build when no local GTK is found")
set(GTKDROP_ADW_TAG "1.7.0" CACHE STRING "libadwaita git tag to build when no local libadwaita is found")

set(GTKDROP_GTK_MIN 4.16)   # CSS color-mix() / var()
set(GTKDROP_ADW_MIN 1.7)    # AdwToggleGroup, Adwaita CSS variables

find_package(PkgConfig REQUIRED)

set(GTKDROP_DEPS_PREFIX "${CMAKE_BINARY_DIR}/_deps/meson-install")
if (EXISTS "${GTKDROP_DEPS_PREFIX}/lib/pkgconfig")
    # Pick up anything a previous configure run already built.
    set(ENV{PKG_CONFIG_PATH} "${GTKDROP_DEPS_PREFIX}/lib/pkgconfig:${GTKDROP_DEPS_PREFIX}/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif ()

# Clones a meson project at `tag` and installs it into GTKDROP_DEPS_PREFIX, unless `pc_name`.pc is already there.
function(gtkdrop_build_meson_dep name repo tag pc_name)
    set(_build_dir "${CMAKE_BINARY_DIR}/_deps/${name}-meson-build")
    if (EXISTS "${GTKDROP_DEPS_PREFIX}/lib/pkgconfig/${pc_name}.pc")
        return()
    endif ()
    message(STATUS "Building ${name} ${tag} from source (this takes a while the first time)")
    find_program(MESON_EXECUTABLE meson REQUIRED)
    find_program(NINJA_EXECUTABLE ninja REQUIRED)

    include(FetchContent)
    FetchContent_Declare(${name}
            GIT_REPOSITORY ${repo}
            GIT_TAG ${tag}
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR _no_cmake_here # a meson project; don't add_subdirectory it
    )
    FetchContent_MakeAvailable(${name})

    set(ENV{PKG_CONFIG_PATH} "${GTKDROP_DEPS_PREFIX}/lib/pkgconfig:${GTKDROP_DEPS_PREFIX}/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    foreach (_step IN ITEMS setup compile install)
        if (_step STREQUAL "setup")
            set(_cmd ${MESON_EXECUTABLE} setup ${_build_dir} ${${name}_SOURCE_DIR}
                    --prefix=${GTKDROP_DEPS_PREFIX} --libdir=lib --buildtype=release
                    --wrap-mode=default # fall back to subprojects for any missing dependency
                    ${ARGN})
        else ()
            set(_cmd ${MESON_EXECUTABLE} ${_step} -C ${_build_dir})
        endif ()
        execute_process(COMMAND ${_cmd} RESULT_VARIABLE _rc)
        if (NOT _rc EQUAL 0)
            message(FATAL_ERROR "meson ${_step} for ${name} failed (${_rc})")
        endif ()
    endforeach ()
endfunction()

set(_built_deps OFF)

if (NOT GTKDROP_FORCE_FETCH_DEPS)
    pkg_check_modules(GTK4 IMPORTED_TARGET gtk4>=${GTKDROP_GTK_MIN})
endif ()
if (NOT GTK4_FOUND)
    gtkdrop_build_meson_dep(gtk https://gitlab.gnome.org/GNOME/gtk.git ${GTKDROP_GTK_TAG} gtk4
            -Dintrospection=disabled -Ddocumentation=false -Dman-pages=false
            -Dbuild-demos=false -Dbuild-examples=false -Dbuild-tests=false -Dbuild-testsuite=false
            -Dmedia-gstreamer=disabled)
    set(ENV{PKG_CONFIG_PATH} "${GTKDROP_DEPS_PREFIX}/lib/pkgconfig:${GTKDROP_DEPS_PREFIX}/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=${GTKDROP_GTK_MIN})
    set(_built_deps ON)
endif ()

if (NOT GTKDROP_FORCE_FETCH_DEPS)
    pkg_check_modules(ADW IMPORTED_TARGET libadwaita-1>=${GTKDROP_ADW_MIN})
endif ()
if (NOT ADW_FOUND)
    gtkdrop_build_meson_dep(libadwaita https://gitlab.gnome.org/GNOME/libadwaita.git ${GTKDROP_ADW_TAG} libadwaita-1
            -Dintrospection=disabled -Dvapi=false -Ddocumentation=false -Dtests=false -Dexamples=false)
    set(ENV{PKG_CONFIG_PATH} "${GTKDROP_DEPS_PREFIX}/lib/pkgconfig:${GTKDROP_DEPS_PREFIX}/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1>=${GTKDROP_ADW_MIN})
    set(_built_deps ON)
endif ()

message(STATUS "Using GTK ${GTK4_VERSION}, libadwaita ${ADW_VERSION}")
if (_built_deps)
    # Make the locally built libs findable at runtime without installing them.
    set_target_properties(GTKdrop PROPERTIES BUILD_RPATH "${GTKDROP_DEPS_PREFIX}/lib")
endif ()

target_link_libraries(GTKdrop PRIVATE PkgConfig::GTK4 PkgConfig::ADW)

# Nexus Mods: HTTP (libsoup) and the API key in the system keyring (libsecret). Both ship with every GNOME desktop.
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(SECRET REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0) # nxm:// handler registration
pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkitgtk-6.0) # the Nexus Mods download window
target_link_libraries(GTKdrop PRIVATE PkgConfig::SOUP PkgConfig::SECRET PkgConfig::GIO_UNIX PkgConfig::WEBKIT)

# ---------------------------------------------------------------------------
# Version: MAJOR.MINOR are set by hand in project(VERSION); the third part is the number of commits since the tag
# v<MAJOR>.<MINOR> (cmake/version.cmake, checked on every build). A local build counter, bumped whenever a source
# changed, tells this machine's builds apart (cmake/local_build.cmake; it lives in the build folder).
# ---------------------------------------------------------------------------
set(GTKDROP_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_target(gtkdrop_version ALL
        COMMAND ${CMAKE_COMMAND} -DMAJOR=${PROJECT_VERSION_MAJOR} -DMINOR=${PROJECT_VERSION_MINOR}
                -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${GTKDROP_GEN_DIR}/version.h
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake
        BYPRODUCTS ${GTKDROP_GEN_DIR}/version.h
        COMMENT "Checking the version"
        VERBATIM
)
file(GLOB_RECURSE _gtkdrop_sources CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/*)
add_custom_command(
        OUTPUT ${GTKDROP_GEN_DIR}/local_build.cpp
        COMMAND ${CMAKE_COMMAND} -DCOUNTER=${CMAKE_CURRENT_BINARY_DIR}/local-build-number
                -DOUT=${GTKDROP_GEN_DIR}/local_build.cpp -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/local_build.cmake
        DEPENDS ${_gtkdrop_sources} ${GTKDROP_GEN_DIR}/version.h ${CMAKE_CURRENT_SOURCE_DIR}/cmake/local_build.cmake
        COMMENT "Bumping the local build number"
        VERBATIM
)
add_dependencies(GTKdrop gtkdrop_version)
target_sources(GTKdrop PRIVATE ${GTKDROP_GEN_DIR}/local_build.cpp)
target_include_directories(GTKdrop PRIVATE ${GTKDROP_GEN_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# nlohmann/json (theme files): system package if present, else fetch the release.
# ---------------------------------------------------------------------------
find_package(nlohmann_json 3.11 QUIET)
if (nlohmann_json_FOUND)
    message(STATUS "Using system nlohmann_json ${nlohmann_json_VERSION}")
else ()
    message(STATUS "nlohmann_json not found - fetching it")
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif ()
target_link_libraries(GTKdrop PRIVATE nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# GResource bundle (UI definitions, CSS, images) compiled into the executable.
# ---------------------------------------------------------------------------
pkg_get_variable(GLIB_COMPILE_RESOURCES gio-2.0 glib_compile_resources)
if (NOT GLIB_COMPILE_RESOURCES)
    find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
endif ()

set(GTKDROP_RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
set(GTKDROP_RES_XML "${GTKDROP_RES_DIR}/gtkdrop.gresource.xml")
set(GTKDROP_RES_C "${CMAKE_CURRENT_BINARY_DIR}/gtkdrop_resources.c")

execute_process(
        COMMAND ${GLIB_COMPILE_RESOURCES} --generate-dependencies --sourcedir=${GTKDROP_RES_DIR} ${GTKDROP_RES_XML}
        OUTPUT_VARIABLE _res_deps OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
)
string(REPLACE "\n" ";" _res_deps "${_res_deps}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GTKDROP_RES_XML})

add_custom_command(
        OUTPUT ${GTKDROP_RES_C}
        COMMAND ${GLIB_COMPILE_RESOURCES} --generate-source --sourcedir=${GTKDROP_RES_DIR}
                --target=${GTKDROP_RES_C} ${GTKDROP_RES_XML}
        DEPENDS ${GTKDROP_RES_XML} ${_res_deps}
        COMMENT "Compiling GResources"
        VERBATIM
)
target_sources(GTKdrop PRIVATE ${GTKDROP_RES_C})
