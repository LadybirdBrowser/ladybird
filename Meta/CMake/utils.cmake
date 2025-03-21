
include(${CMAKE_CURRENT_LIST_DIR}/code_generators.cmake)

function(serenity_generated_sources target_name)
    if(DEFINED GENERATED_SOURCES)
        set_source_files_properties(${GENERATED_SOURCES} PROPERTIES GENERATED 1)
        foreach(generated ${GENERATED_SOURCES})
            get_filename_component(generated_name ${generated} NAME)
            add_dependencies(${target_name} generate_${generated_name})
            add_dependencies(all_generated generate_${generated_name})
        endforeach()
    endif()
endfunction()

function(serenity_testjs_test test_src sub_dir)
    cmake_parse_arguments(PARSE_ARGV 2 SERENITY_TEST "" "CUSTOM_MAIN" "LIBS")
    if ("${SERENITY_TEST_CUSTOM_MAIN}" STREQUAL "")
        set(SERENITY_TEST_CUSTOM_MAIN "$<TARGET_OBJECTS:JavaScriptTestRunnerMain>")
    endif()
    list(APPEND SERENITY_TEST_LIBS LibJS LibCore LibFileSystem)
    serenity_test(${test_src} ${sub_dir}
        CUSTOM_MAIN "${SERENITY_TEST_CUSTOM_MAIN}"
        LIBS ${SERENITY_TEST_LIBS})
endfunction()

function(remove_path_if_version_changed version version_file cache_path)
    set(version_differs YES)

    if (EXISTS "${version_file}")
        file(STRINGS "${version_file}" active_version)
        if (version STREQUAL active_version)
            set(version_differs NO)
        endif()
    endif()

    if (version_differs)
        message(STATUS "Removing outdated ${cache_path} for version ${version}")
        file(REMOVE_RECURSE "${cache_path}")
        file(WRITE "${version_file}" "${version}")
    endif()
endfunction()

function(invoke_generator name generator primary_source header implementation)
    cmake_parse_arguments(invoke_generator "" "" "arguments;dependencies" ${ARGN})

    add_custom_command(
        OUTPUT "${header}" "${implementation}"
        COMMAND $<TARGET_FILE:${generator}> -h "${header}.tmp" -c "${implementation}.tmp" ${invoke_generator_arguments}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${header}.tmp" "${header}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${implementation}.tmp" "${implementation}"
        COMMAND "${CMAKE_COMMAND}" -E remove "${header}.tmp" "${implementation}.tmp"
        VERBATIM
        DEPENDS ${generator} ${invoke_generator_dependencies} "${primary_source}"
    )

    add_custom_target("generate_${name}" DEPENDS "${header}" "${implementation}")
    add_dependencies(all_generated "generate_${name}")
    list(APPEND CURRENT_LIB_GENERATED "${name}")
    set(CURRENT_LIB_GENERATED ${CURRENT_LIB_GENERATED} PARENT_SCOPE)
endfunction()

function(invoke_idl_generator cpp_name idl_name generator primary_source header implementation idl)
    cmake_parse_arguments(invoke_idl_generator "" "" "arguments;dependencies" ${ARGN})

    add_custom_command(
        OUTPUT "${header}" "${implementation}" "${idl}"
        COMMAND $<TARGET_FILE:${generator}> -h "${header}.tmp" -c "${implementation}.tmp" -i "${idl}.tmp" ${invoke_idl_generator_arguments}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${header}.tmp" "${header}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${implementation}.tmp" "${implementation}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${idl}.tmp" "${idl}"
        COMMAND "${CMAKE_COMMAND}" -E remove "${header}.tmp" "${implementation}.tmp" "${idl}.tmp"
        VERBATIM
        DEPENDS ${generator} ${invoke_idl_generator_dependencies} "${primary_source}"
    )

    add_custom_target("generate_${cpp_name}" DEPENDS "${header}" "${implementation}" "${idl}")
    add_custom_target("generate_${idl_name}" DEPENDS "generate_${cpp_name}")
    add_dependencies(all_generated "generate_${cpp_name}")
    add_dependencies(all_generated "generate_${idl_name}")
    list(APPEND CURRENT_LIB_GENERATED "${name}")
    set(CURRENT_LIB_GENERATED ${CURRENT_LIB_GENERATED} PARENT_SCOPE)
endfunction()

function(download_file_multisource urls path)
    cmake_parse_arguments(DOWNLOAD "" "SHA256" "" ${ARGN})

    if (NOT "${DOWNLOAD_SHA256}" STREQUAL "")
        set(DOWNLOAD_SHA256 EXPECTED_HASH "SHA256=${DOWNLOAD_SHA256}")
    endif()

    if (NOT EXISTS "${path}")
        if (NOT ENABLE_NETWORK_DOWNLOADS)
            message(FATAL_ERROR "${path} does not exist, and unable to download it")
        endif()

        get_filename_component(file "${path}" NAME)
        set(tmp_path "${path}.tmp")

        foreach(url ${urls})
            message(STATUS "Downloading file ${file} from ${url}")

            file(DOWNLOAD "${url}" "${tmp_path}" INACTIVITY_TIMEOUT 10 STATUS download_result ${DOWNLOAD_SHA256})
            list(GET download_result 0 status_code)
            list(GET download_result 1 error_message)

            if (status_code EQUAL 0)
                file(RENAME "${tmp_path}" "${path}")
                break()
            endif()

            file(REMOVE "${tmp_path}")
            message(WARNING "Failed to download ${url}: ${error_message}")
        endforeach()

        if (NOT status_code EQUAL 0)
            message(FATAL_ERROR "Failed to download ${path} from any source")
        endif()
    endif()
endfunction()

function(download_file url path)
    cmake_parse_arguments(DOWNLOAD "" "SHA256" "" ${ARGN})

    # If the timestamp doesn't match exactly, the Web Archive should redirect to the closest archived file automatically.
    download_file_multisource("${url};https://web.archive.org/web/99991231235959/${url}" "${path}" SHA256 "${DOWNLOAD_SHA256}")
endfunction()

function(extract_path dest_dir zip_path source_path dest_path)
    if (EXISTS "${zip_path}" AND NOT EXISTS "${dest_path}")
        file(ARCHIVE_EXTRACT INPUT "${zip_path}" DESTINATION "${dest_dir}" PATTERNS "${source_path}")
    endif()
endfunction()

function(add_lagom_library_install_rules target_name)
    cmake_parse_arguments(PARSE_ARGV 1 LAGOM_INSTALL_RULES "" "ALIAS_NAME" "")
    if (NOT LAGOM_INSTALL_RULES_ALIAS_NAME)
        set(LAGOM_INSTALL_RULES_ALIAS_NAME ${target_name})
    endif()
     # Don't make alias when we're going to import a previous build for Tools
    # FIXME: Is there a better way to write this?
    if (NOT ENABLE_FUZZERS AND NOT CMAKE_CROSSCOMPILING)
        # alias for parity with exports
        add_library(Lagom::${LAGOM_INSTALL_RULES_ALIAS_NAME} ALIAS ${target_name})
    endif()
    install(TARGETS ${target_name} EXPORT LagomTargets
        RUNTIME COMPONENT Lagom_Runtime
        LIBRARY COMPONENT Lagom_Runtime NAMELINK_COMPONENT Lagom_Development
        ARCHIVE COMPONENT Lagom_Development
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endfunction()

if (NOT COMMAND swizzle_target_properties_for_swift)
    function(swizzle_target_properties_for_swift target)
    endfunction()
endif()
