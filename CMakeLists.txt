cmake_minimum_required(VERSION 3.20)

# Dépendances vcpkg liées statiquement : la DLL est chargée par LoadLibrary depuis
# build/, Windows ne chercherait pas ses DLL dépendantes à côté d'elle.
set(VCPKG_TARGET_TRIPLET x64-windows-static CACHE STRING "")
project(WarbandVR VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Cible WSE2 x64 : configure avec  cmake -S . -B build -A x64")
endif()

# Numéro de version visible dans le log, l'injecteur et l'outil de configuration
add_compile_definitions(WARBANDVR_VERSION="${PROJECT_VERSION}")

find_package(minhook CONFIG REQUIRED)
find_package(OpenXR CONFIG REQUIRED)

add_library(warband_vr SHARED
    src/dllmain.cpp
    src/util/log.cpp
    src/util/pattern.cpp
    src/util/config.cpp
    src/engine/camera_hook.cpp
    src/engine/skeleton_hook.cpp
    src/engine/combat_hook.cpp
    src/engine/text_input.cpp
    src/engine/orders.cpp
    src/engine/audio_route.cpp
    src/engine/frame_pacing.cpp
    src/render/d3d9_hook.cpp
    src/render/frame_share.cpp
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
add_executable(warband_vr_config WIN32 config_tool/main.cpp src/util/config.cpp)
target_include_directories(warband_vr_config PRIVATE src)
target_compile_definitions(warband_vr_config PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(warband_vr_config PRIVATE /utf-8 /W4)
target_link_libraries(warband_vr_config PRIVATE comctl32)
set_property(TARGET warband_vr_config PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
