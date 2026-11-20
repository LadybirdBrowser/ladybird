include_guard()

# CMake collects runtime dependencies only on Windows, Linux and macOS.
# Windows is left out too, as its vcpkg DLLs live in bin/ and install as RUNTIME artifacts.
set(LADYBIRD_INSTALLS_VCPKG_LIBRARIES OFF)
if (CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" AND NOT "${VCPKG_INSTALLED_DIR}" STREQUAL "")
    set(LADYBIRD_INSTALLS_VCPKG_LIBRARIES ON)
endif()

function(ladybird_vcpkg_library_directory output_variable)
    set(vcpkg_library_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    if (NOT CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
        set(vcpkg_library_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib")
    endif()
    set(${output_variable} "${vcpkg_library_dir}" PARENT_SCOPE)
endfunction()

# Installs the shared libraries from vcpkg that the binaries in the runtime dependency set link. Call it after every
# install() that adds to the set.
function(ladybird_install_vcpkg_libraries dependency_set)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "DESTINATION;COMPONENT" "")

    ladybird_vcpkg_library_directory(vcpkg_library_dir)
    string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" vcpkg_library_dir_regex "${vcpkg_library_dir}")

    set(system_library_regexes "")
    if (APPLE)
        # These are in the dyld shared cache rather than on disk.
        set(system_library_regexes "^/usr/lib/" "^/System/")
    endif()

    install(RUNTIME_DEPENDENCY_SET ${dependency_set}
        PRE_EXCLUDE_REGEXES ${system_library_regexes}
        POST_INCLUDE_REGEXES "^${vcpkg_library_dir_regex}/"
        POST_EXCLUDE_REGEXES ".*"
        DIRECTORIES "${vcpkg_library_dir}"
        LIBRARY DESTINATION ${ARG_DESTINATION} COMPONENT ${ARG_COMPONENT}
    )

    # The libraries are installed under the names of the files they resolve to, yet binaries may link them through the
    # versioned links next to those files.
    install(CODE "
    set(library_install_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${ARG_DESTINATION}\")
    file(GLOB links LIST_DIRECTORIES false \"${vcpkg_library_dir}/*\")
    foreach (link IN LISTS links)
      cmake_path(GET link FILENAME link_name)
      if (NOT IS_SYMLINK \"\${link}\" OR NOT link_name MATCHES \"\\\\.[0-9]+\\\\.dylib\$|\\\\.so\\\\.[0-9]+\")
        continue()
      endif()
      file(REAL_PATH \"\${link}\" target)
      cmake_path(GET target FILENAME target_name)
      if (EXISTS \"\${library_install_dir}/\${target_name}\" AND NOT link_name STREQUAL target_name)
        file(CREATE_LINK \"\${target_name}\" \"\${library_install_dir}/\${link_name}\" SYMBOLIC)
      endif()
    endforeach()
  "
            COMPONENT ${ARG_COMPONENT})
endfunction()
