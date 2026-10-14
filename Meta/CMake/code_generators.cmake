#
# Functions for generating sources using host tools
#

function(embed_as_string name source_file output source_variable_name)
    cmake_parse_arguments(PARSE_ARGV 4 EMBED_STRING_VIEW "" "NAMESPACE" "")
    set(namespace_arg "")
    if (EMBED_STRING_VIEW_NAMESPACE)
        set(namespace_arg "-s ${EMBED_STRING_VIEW_NAMESPACE}")
    endif()
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${LADYBIRD_SOURCE_DIR}/Meta/Generators/embed_as_string.py" "${source_file}" -o "${output}.tmp" -n "${source_variable_name}" ${namespace_arg}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${output}.tmp" "${output}"
        COMMAND "${CMAKE_COMMAND}" -E remove "${output}.tmp"
        VERBATIM
        DEPENDS "${LADYBIRD_SOURCE_DIR}/Meta/Generators/embed_as_string.py"
        MAIN_DEPENDENCY "${source_file}"
    )

    add_custom_target("generate_${name}" DEPENDS "${output}")
    add_dependencies(ladybird_codegen_accumulator "generate_${name}")
endfunction()

function(compile_ipc source output)
    if (NOT IS_ABSOLUTE ${source})
        set(source ${CMAKE_CURRENT_SOURCE_DIR}/${source})
    endif()
    if (NOT IS_ABSOLUTE ${output})
        set(output ${CMAKE_CURRENT_BINARY_DIR}/${output})
    endif()
    cmake_path(REPLACE_EXTENSION output LAST_ONLY .cpp OUTPUT_VARIABLE implementation)
    get_filename_component(output_name ${output} NAME)
    cmake_path(RELATIVE_PATH CMAKE_CURRENT_SOURCE_DIR BASE_DIRECTORY ${LADYBIRD_SOURCE_DIR} OUTPUT_VARIABLE current_source_dir_relative)
    set(library_arguments)
    if (current_source_dir_relative MATCHES "^Libraries/(Lib[^/]+)(/|$)")
        list(APPEND library_arguments --library ${CMAKE_MATCH_1})
        set(GENERATED_IPC TRUE PARENT_SCOPE)
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(
        OUTPUT ${output} ${implementation}
        COMMAND "${Python3_EXECUTABLE}" "${LADYBIRD_SOURCE_DIR}/Meta/Generators/generate_ipc_definitions.py"
            --input ${source} --output ${output}.tmp --implementation ${implementation}.tmp
            --header-name ${output_name} ${library_arguments}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${output}.tmp ${output}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${implementation}.tmp ${implementation}
        COMMAND "${CMAKE_COMMAND}" -E remove ${output}.tmp ${implementation}.tmp
        VERBATIM
        DEPENDS "${LADYBIRD_SOURCE_DIR}/Meta/Generators/generate_ipc_definitions.py"
        MAIN_DEPENDENCY ${source}
    )
    add_custom_target(generate_${output_name} DEPENDS ${output} ${implementation})
    add_dependencies(ladybird_codegen_accumulator generate_${output_name})
    list(APPEND SOURCES ${implementation})
    set(SOURCES ${SOURCES} PARENT_SCOPE)

    if (ENABLE_INSTALL_HEADERS)
        install(FILES ${output} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${current_source_dir_relative}" OPTIONAL)
    endif()
endfunction()
