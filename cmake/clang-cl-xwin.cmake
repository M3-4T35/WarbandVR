# Compilation croisée Linux -> Windows x64 : clang-cl + lld-link, SDK Windows et CRT MSVC
# extraits par xwin (tools/setup_linux_toolchain.sh). Les dépendances (MinHook, chargeur
# OpenXR) sont alors compilées depuis leurs sources (WARBANDVR_FETCH_DEPS), pas par vcpkg.
#
#   cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake
#
# Dossier du SDK : variable XWIN_DIR (cache ou environnement), ~/.local/opt/winsdk par défaut.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(_triple x86_64-pc-windows-msvc)
set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
set(CMAKE_LINKER lld-link)
set(CMAKE_AR ${CMAKE_CURRENT_LIST_DIR}/lld-lib)
set(CMAKE_RC_COMPILER ${CMAKE_CURRENT_LIST_DIR}/no-rc)
set(CMAKE_MT "")
set(CMAKE_USER_MAKE_RULES_OVERRIDE_C ${CMAKE_CURRENT_LIST_DIR}/no-vs-link.cmake)
set(CMAKE_USER_MAKE_RULES_OVERRIDE_CXX ${CMAKE_CURRENT_LIST_DIR}/no-vs-link.cmake)

if(NOT XWIN_DIR)
    if(DEFINED ENV{XWIN_DIR})
        set(XWIN_DIR $ENV{XWIN_DIR})
    else()
        set(XWIN_DIR $ENV{HOME}/.local/opt/winsdk)
    endif()
endif()
set(XWIN_DIR ${XWIN_DIR} CACHE PATH "SDK Windows + CRT MSVC extraits par xwin")
# Transmis aux try_compile de CMake (qui relisent ce fichier)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)

file(GLOB _msvc LIST_DIRECTORIES true "${XWIN_DIR}/VC/Tools/MSVC/*")
file(GLOB _sdk_inc LIST_DIRECTORIES true "${XWIN_DIR}/Windows Kits/10/Include/*")
file(GLOB _sdk_lib LIST_DIRECTORIES true "${XWIN_DIR}/Windows Kits/10/Lib/*")
list(SORT _msvc)
list(SORT _sdk_inc)
list(SORT _sdk_lib)
if(NOT _msvc OR NOT _sdk_inc OR NOT _sdk_lib)
    message(FATAL_ERROR "SDK Windows introuvable dans '${XWIN_DIR}' : lancer tools/setup_linux_toolchain.sh")
endif()
# Versions les plus récentes
list(GET _msvc -1 _msvc)
list(GET _sdk_inc -1 _sdk_inc)
list(GET _sdk_lib -1 _sdk_lib)

set(_flags "")
foreach(dir "${_msvc}/include" "${_sdk_inc}/ucrt" "${_sdk_inc}/um" "${_sdk_inc}/shared" "${_sdk_inc}/winrt")
    string(APPEND _flags " \"/imsvc${dir}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")

# xwin nomme le dossier x86_64 (x64 chez Microsoft)
set(_link "")
foreach(dir "${_msvc}/lib/x86_64" "${_sdk_lib}/ucrt/x86_64" "${_sdk_lib}/um/x86_64")
    string(APPEND _link " \"/libpath:${dir}\"")
endforeach()
# Sans mt.exe : lld-link intègre lui-même le manifeste (contrôles communs v6 de l'outil de config)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_link} /manifest:embed")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_link} /manifest:embed")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_link}")

# xwin n'extrait que le CRT Release (msvcrtd.lib absent) : tests de CMake en Release
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# Bibliothèques et en-têtes cherchés uniquement dans le SDK, jamais dans /usr (Linux)
set(CMAKE_FIND_ROOT_PATH ${XWIN_DIR})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(WARBANDVR_FETCH_DEPS ON CACHE BOOL "MinHook et OpenXR compilés depuis leurs sources")
