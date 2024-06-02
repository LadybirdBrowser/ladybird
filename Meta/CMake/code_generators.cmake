#
# Functions for generating sources using host tools
#

function(embed_as_string_view name source_file output source_variable_name)
    cmake_parse_arguments(PARSE_ARGV 4 EMBED_STRING_VIEW "" "NAMESPACE" "")
    set(namespace_arg "")
    if (EMBED_STRING_VIEW_NAMESPACE)
        set(namespace_arg "-s ${EMBED_STRING_VIEW_NAMESPACE}")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${SerenityOS_SOURCE_DIR}/Meta/embed_as_string_view.py" "${source_file}" -o "${output}.tmp" -n "${source_variable_name}" ${namespace_arg}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${output}.tmp" "${output}"
        COMMAND "${CMAKE_COMMAND}" -E remove "${output}.tmp"
        VERBATIM
        DEPENDS "${SerenityOS_SOURCE_DIR}/Meta/embed_as_string_view.py"
        MAIN_DEPENDENCY "${source_file}"
    )

    add_custom_target("generate_${name}" DEPENDS "${output}")
    add_dependencies(all_generated "generate_${name}")
endfunction()

function(stringify_gml source output string_name)
    set(source ${CMAKE_CURRENT_SOURCE_DIR}/${source})
    get_filename_component(output_name ${output} NAME)
    embed_as_string_view(${output_name} ${source} ${output} ${string_name})
endfunction()

function(compile_ipc source output)
    if (NOT IS_ABSOLUTE ${source})
        set(source ${CMAKE_CURRENT_SOURCE_DIR}/${source})
    endif()
    add_custom_command(
        OUTPUT ${output}
        COMMAND $<TARGET_FILE:Lagom::IPCCompiler> ${source} -o ${output}.tmp
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${output}.tmp ${output}
        COMMAND "${CMAKE_COMMAND}" -E remove ${output}.tmp
        VERBATIM
        DEPENDS Lagom::IPCCompiler
        MAIN_DEPENDENCY ${source}
    )
    get_filename_component(output_name ${output} NAME)
    add_custom_target(generate_${output_name} DEPENDS ${output})
    add_dependencies(all_generated generate_${output_name})

    cmake_path(RELATIVE_PATH CMAKE_CURRENT_SOURCE_DIR BASE_DIRECTORY ${SerenityOS_SOURCE_DIR} OUTPUT_VARIABLE current_source_dir_relative)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${output} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${current_source_dir_relative}" OPTIONAL)
endfunction()

function(generate_state_machine source header)
    get_filename_component(header_name ${header} NAME)
    set(target_name "generate_${header_name}")
    # Note: This function is called twice with the same header, once in the kernel
    #       and once in Userland/LibVT, this check makes sure that only one target
    #       is generated for that header.
    if(NOT TARGET ${target_name})
        set(source ${CMAKE_CURRENT_SOURCE_DIR}/${source})
        set(output ${CMAKE_CURRENT_BINARY_DIR}/${header})
        add_custom_command(
            OUTPUT ${output}
            COMMAND $<TARGET_FILE:Lagom::StateMachineGenerator> ${source} -o ${output}.tmp
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${output}.tmp ${output}
            COMMAND "${CMAKE_COMMAND}" -E remove ${output}.tmp
            VERBATIM
            DEPENDS Lagom::StateMachineGenerator
            MAIN_DEPENDENCY ${source}
        )
        add_custom_target(${target_name} DEPENDS ${output})
        add_dependencies(all_generated ${target_name})
    endif()
endfunction()
