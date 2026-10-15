function(z_vcpkg_curl_libraries_warning variable access value current_list_file stack)
    if(variable STREQUAL "CURL_LIBRARIES"
       AND access STREQUAL "READ_ACCESS"
       AND NOT z_vcpkg_curl_libraries_warning_issued)
        set(z_vcpkg_curl_libraries_warning_issued 1 PARENT_SCOPE)
        message(WARNING
            "CURL_LIBRARIES is '${CURL_LIBRARIES}'. "
            "When linking imported targets, exported CMake config must use \"find_dependency(CURL)\"."
        )
    endif()
endfunction()

list(REMOVE_ITEM ARGS "NO_MODULE" "CONFIG" "MODULE")
list(GET ARGS 0 _z_vcpg_curl_name)
_find_package(${ARGS} CONFIG)

if(${_z_vcpg_curl_name}_FOUND)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0012 NEW)
    cmake_policy(SET CMP0054 NEW)
    cmake_policy(SET CMP0057 NEW)

    set(_curl_target CURL::libcurl_shared)
    if(TARGET CURL::libcurl_static)
        set(_curl_target CURL::libcurl_static)
    endif()
    get_target_property(_curl_link_libraries ${_curl_target} INTERFACE_LINK_LIBRARIES)
    if(NOT _curl_link_libraries)
        set(_curl_link_libraries "")
    endif()
    if(_curl_link_libraries MATCHES "ZLIB::ZLIB")
        string(REGEX REPLACE "([\$]<[^;]*)?ZLIB::ZLIB([^;]*>)?" "${ZLIB_LIBRARIES}" _curl_link_libraries "${_curl_link_libraries}")
    endif()
    if(_curl_link_libraries MATCHES "OpenSSL::")
        string(REGEX REPLACE "([\$]<[^;]*)?OpenSSL::(SSL|Crypto)([^;]*>)?" "${OPENSSL_LIBRARIES}" _curl_link_libraries "${_curl_link_libraries}")
    endif()
    if(_curl_link_libraries MATCHES "::")
        # leave CURL_LIBRARIES as set by upstream (imported target), but add information.
        variable_watch(CURL_LIBRARIES "z_vcpkg_curl_libraries_warning")
    else()
        get_target_property(CURL_INCLUDE_DIRS ${_curl_target} INTERFACE_INCLUDE_DIRECTORIES)
        # resolve CURL_LIBRARIES to filepaths.
        if(WIN32)
            get_target_property(_curl_location_debug ${_curl_target} IMPORTED_IMPLIB_DEBUG)
            get_target_property(_curl_location_release ${_curl_target} IMPORTED_IMPLIB_RELEASE)
        endif()

        if(NOT _curl_location_debug AND NOT _curl_location_release)
            get_target_property(_curl_location_debug ${_curl_target} IMPORTED_LOCATION_DEBUG)
            get_target_property(_curl_location_release ${_curl_target} IMPORTED_LOCATION_RELEASE)
        endif()

        set(CURL_LIBRARY_DEBUG "${_curl_location_debug}" CACHE INTERNAL "vcpkg")
        set(CURL_LIBRARY_RELEASE "${_curl_location_release}" CACHE INTERNAL "vcpkg")
        include("${CMAKE_ROOT}/Modules/SelectLibraryConfigurations.cmake")
        select_library_configurations(CURL)
        set(CURL_LIBRARIES ${CURL_LIBRARY} ${_curl_link_libraries})

        unset(_curl_link_libraries)
        unset(_curl_location_debug)
        unset(_curl_location_release)
    endif()

    unset(_curl_target)
    cmake_policy(POP)
endif()
