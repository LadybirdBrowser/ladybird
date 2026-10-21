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
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "RUST_OUTPUT" "")
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
        # NB: The library is defined after its IPC files, so its export macro is only known when the build is generated.
        list(APPEND library_arguments --library ${CMAKE_MATCH_1} "--export-macro=$<TARGET_PROPERTY:${CMAKE_MATCH_1},LADYBIRD_EXPORT_MACRO>")
        set(GENERATED_IPC TRUE PARENT_SCOPE)
    endif()
    set(rust_arguments)
    set(rust_output)
    set(rust_commands)
    if (ARG_RUST_OUTPUT)
        get_filename_component(rust_output "${ARG_RUST_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
        list(APPEND rust_arguments --rust-output ${rust_output}.tmp)
        list(APPEND rust_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${rust_output}.tmp ${rust_output}
            COMMAND "${CMAKE_COMMAND}" -E remove ${rust_output}.tmp)
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(
        OUTPUT ${output} ${implementation} ${rust_output}
        COMMAND "${Python3_EXECUTABLE}" "${LADYBIRD_SOURCE_DIR}/Meta/Generators/generate_ipc_definitions.py"
            --input ${source} --output ${output}.tmp --implementation ${implementation}.tmp
            --header-name ${output_name} ${library_arguments} ${rust_arguments}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${output}.tmp ${output}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${implementation}.tmp ${implementation}
        COMMAND "${CMAKE_COMMAND}" -E remove ${output}.tmp ${implementation}.tmp
        ${rust_commands}
        VERBATIM
        DEPENDS "${LADYBIRD_SOURCE_DIR}/Meta/Generators/generate_ipc_definitions.py"
            "${LADYBIRD_SOURCE_DIR}/Meta/Utils/lexer.py"
            "${LADYBIRD_SOURCE_DIR}/Meta/Utils/utils.py"
        MAIN_DEPENDENCY ${source}
    )
    add_custom_target(generate_${output_name} DEPENDS ${output} ${implementation} ${rust_output})
    add_dependencies(ladybird_codegen_accumulator generate_${output_name})
    list(APPEND SOURCES ${implementation})
    set(SOURCES ${SOURCES} PARENT_SCOPE)

    if (ENABLE_INSTALL_HEADERS)
        # NB: A header generated into a subdirectory of the library is installed into the same subdirectory, where
        #     the library's headers include it from.
        get_filename_component(output_directory ${output} DIRECTORY)
        file(RELATIVE_PATH output_subdirectory ${CMAKE_CURRENT_BINARY_DIR} ${output_directory})
        if (output_subdirectory MATCHES "^\\.\\.")
            set(output_subdirectory "")
        endif()
        install(FILES ${output} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${current_source_dir_relative}/${output_subdirectory}" OPTIONAL)
    endif()
endfunction()
