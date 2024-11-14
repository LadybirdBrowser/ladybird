#
# Functions for generating sources using host tools
#

function(embed_as_string name source_file output source_variable_name)
    cmake_parse_arguments(PARSE_ARGV 4 EMBED_STRING_VIEW "" "NAMESPACE" "")
    set(namespace_arg "")
    if (EMBED_STRING_VIEW_NAMESPACE)
        set(namespace_arg "-s ${EMBED_STRING_VIEW_NAMESPACE}")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${SerenityOS_SOURCE_DIR}/Meta/embed_as_string.py" "${source_file}" -o "${output}.tmp" -n "${source_variable_name}" ${namespace_arg}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${output}.tmp" "${output}"
        COMMAND "${CMAKE_COMMAND}" -E remove "${output}.tmp"
        VERBATIM
        DEPENDS "${SerenityOS_SOURCE_DIR}/Meta/embed_as_string.py"
        MAIN_DEPENDENCY "${source_file}"
    )

    add_custom_target("generate_${name}" DEPENDS "${output}")
    add_dependencies(all_generated "generate_${name}")
endfunction()

function(generate_clang_module_map target_name)
    cmake_parse_arguments(PARSE_ARGV 1 MODULE_MAP "" "DIRECTORY" "GENERATED_FILES;EXCLUDE_FILES")
    if (NOT MODULE_MAP_DIRECTORY)
        set(MODULE_MAP_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    string(REPLACE "Lib" "" module_name ${target_name})
    set(module_name "${module_name}Cxx")

    set(module_map_file "${CMAKE_CURRENT_BINARY_DIR}/module/module.modulemap")
    set(vfs_overlay_file "${VFS_OVERLAY_DIRECTORY}/${target_name}_vfs_overlay.yaml")

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    # FIXME: Make this depend on the public headers of the target
    add_custom_command(
        OUTPUT "${module_map_file}"
        COMMAND "${Python3_EXECUTABLE}" "${SerenityOS_SOURCE_DIR}/Meta/generate_clang_module_map.py"
                "${MODULE_MAP_DIRECTORY}"
                --module-name "${module_name}"
                --module-map "${module_map_file}"
                --vfs-map ${vfs_overlay_file}
                --exclude-files ${MODULE_MAP_EXCLUDE_FILES}
                --generated-files ${MODULE_MAP_GENERATED_FILES}
        VERBATIM
        DEPENDS "${SerenityOS_SOURCE_DIR}/Meta/generate_clang_module_map.py"
    )

    add_custom_target("generate_${target_name}_module_map" DEPENDS "${module_map_file}")
    add_dependencies(all_generated "generate_${target_name}_module_map")
    add_dependencies("${target_name}" "generate_${target_name}_module_map")

    target_compile_options(${target_name} PUBLIC "SHELL:$<$<COMPILE_LANGUAGE:Swift>:-Xcc -ivfsoverlay${vfs_overlay_file}>")
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
    if (ENABLE_INSTALL_HEADERS)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${output} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${current_source_dir_relative}" OPTIONAL)
    endif()
endfunction()
