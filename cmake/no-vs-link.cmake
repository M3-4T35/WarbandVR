# Inclus après les règles de la plateforme (CMAKE_USER_MAKE_RULES_OVERRIDE) : sans mt.exe ni
# rc.exe, on appelle lld-link directement au lieu de « cmake -E vs_link_* », et c'est lld-link
# qui intègre le manifeste (/manifest:embed, posé par le toolchain).
foreach(_var CMAKE_C_LINK_EXECUTABLE CMAKE_CXX_LINK_EXECUTABLE
             CMAKE_C_CREATE_SHARED_LIBRARY CMAKE_CXX_CREATE_SHARED_LIBRARY
             CMAKE_C_CREATE_SHARED_MODULE CMAKE_CXX_CREATE_SHARED_MODULE)
    if(DEFINED ${_var})
        string(REGEX REPLACE "<CMAKE_COMMAND> -E vs_link_(exe|dll) .*<MANIFESTS> -- " "" ${_var} "${${_var}}")
    endif()
endforeach()
