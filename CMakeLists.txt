cmake_minimum_required(VERSION 3.20)

# Dépendances vcpkg liées statiquement : la DLL est chargée par LoadLibrary depuis
# build/, Windows ne chercherait pas ses DLL dépendantes à côté d'elle.
set(VCPKG_TARGET_TRIPLET x64-windows-static CACHE STRING "")
project(WarbandVR VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Cible WSE2 x64 : configure avec  cmake -S . -B build -A x64")
endif()

# Numéro de version visible dans le log, l'injecteur et l'outil de configuration
add_compile_definitions(WARBANDVR_VERSION="${PROJECT_VERSION}")

option(WARBANDVR_FETCH_DEPS "MinHook et OpenXR compilés depuis leurs sources (sans vcpkg)" OFF)
if(WARBANDVR_FETCH_DEPS)
    # Compilation croisée depuis Linux (cmake/clang-cl-xwin.cmake) : mêmes versions que la
    # baseline vcpkg, liées statiquement avec le CRT statique.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    include(FetchContent)
    FetchContent_Declare(minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG v1.3.4
        GIT_SHALLOW TRUE)
    FetchContent_Declare(openxr
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.63
        GIT_SHALLOW TRUE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
    set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(minhook openxr)
    if(NOT TARGET minhook::minhook)
        add_library(minhook::minhook ALIAS minhook)
    endif()
    if(NOT TARGET OpenXR::openxr_loader)
        add_library(OpenXR::openxr_loader ALIAS openxr_loader)
    endif()
    # Le chargeur statique force le CRT dynamique (vcpkg le corrige par un patch), et son
    # loader.rc (simple numéro de version) demanderait llvm-rc.
    set_property(TARGET openxr_loader PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    get_target_property(_xr_sources openxr_loader SOURCES)
    list(FILTER _xr_sources EXCLUDE REGEX "\\.rc$")
    set_property(TARGET openxr_loader PROPERTY SOURCES ${_xr_sources})
else()
    find_package(minhook CONFIG REQUIRED)
    find_package(OpenXR CONFIG REQUIRED)
endif()

add_library(warband_vr SHARED
    src/dllmain.cpp
    src/util/log.cpp
    src/util/pattern.cpp
    src/util/hook.cpp
    src/util/config.cpp
    src/engine/camera_hook.cpp
    src/engine/skeleton_hook.cpp
    src/engine/combat_hook.cpp
    src/engine/text_input.cpp
    src/engine/orders.cpp
    src/engine/audio_route.cpp
    src/engine/frame_pacing.cpp
    src/engine/true_stereo.cpp
    src/render/d3d9_hook.cpp
    src/render/frame_share.cpp
    src/render/frame_profile.cpp
    src/render/game_window.cpp
    src/render/ui_redirect.cpp
    src/xr/xr_session.cpp
    src/xr/xr_controls.cpp
    src/xr/xr_hands.cpp
    src/xr/xr_keyboard.cpp
    src/xr/xr_radial.cpp
    src/xr/gdi_layer.cpp
    src/input/pointer.cpp
    src/input/dinput_hook.cpp
)
target_include_directories(warband_vr PRIVATE src)
target_compile_options(warband_vr PRIVATE /utf-8 /W4)
target_link_libraries(warband_vr PRIVATE minhook::minhook OpenXR::openxr_loader d3d11 dxgi dxguid)
target_compile_definitions(warband_vr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# Runtime statique : pas besoin d'installer les redistribuables VC++ à côté du jeu
set_property(TARGET warband_vr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Injecteur CLI : lance mb_warband_wse2_x64.exe et y injecte warband_vr.dll
add_executable(warband_vr_injector injector/main.cpp src/util/config.cpp)
target_include_directories(warband_vr_injector PRIVATE src)
target_compile_definitions(warband_vr_injector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(warband_vr_injector PRIVATE /utf-8 /W4)
target_link_libraries(warband_vr_injector PRIVATE psapi)
set_property(TARGET warband_vr_injector PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# L'injecteur cherche la DLL à côté de lui
add_dependencies(warband_vr_injector warband_vr)

# Outil de configuration : édite warband_vr.ini (réglages partagés avec la DLL)
add_executable(warband_vr_config WIN32 config_tool/main.cpp config_tool/game_settings.cpp src/util/config.cpp)
target_include_directories(warband_vr_config PRIVATE src)
target_compile_definitions(warband_vr_config PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(warband_vr_config PRIVATE /utf-8 /W4)
target_link_libraries(warband_vr_config PRIVATE comctl32 winhttp comdlg32 shell32)
set_property(TARGET warband_vr_config PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# clang-cl (compilation croisée) : idiomes MSVC voulus, que cl.exe ne signale pas
# (pointeurs de fonction <-> void* pour MinHook et GetProcAddress, STRUCT s{sizeof(s)}).
# Après /W4, qui les réactiverait.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    foreach(t warband_vr warband_vr_injector warband_vr_config)
        target_compile_options(${t} PRIVATE -Wno-microsoft-cast -Wno-cast-function-type-mismatch
                                            -Wno-missing-field-initializers)
    endforeach()
endif()
