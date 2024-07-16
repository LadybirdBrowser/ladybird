function(lagom_tool tool)
    cmake_parse_arguments(LAGOM_TOOL "" "INSTALL" "SOURCES;LIBS" ${ARGN})
    add_executable(${tool} ${SOURCES} ${LAGOM_TOOL_SOURCES})
    # alias for parity with exports
    add_executable(Lagom::${tool} ALIAS ${tool})
    target_link_libraries(${tool} AK LibCoreMinimal LibFileSystem GenericClangPlugin ${LAGOM_TOOL_LIBS})
    if (NOT DEFINED LAGOM_TOOL_INSTALL)
        set(LAGOM_TOOL_INSTALL ${INSTALL_LAGOM_TOOLS})
    endif()
    if (DEFINED LAGOM_TOOL_INSTALL AND LAGOM_TOOL_INSTALL)
        install(
            TARGETS ${tool}
            EXPORT LagomTargets
            RUNTIME COMPONENT Lagom_Runtime
        )
    endif()
endfunction()

add_subdirectory(CodeGenerators)
add_subdirectory(IPCMagicLinter)
