include(${CMAKE_CURRENT_LIST_DIR}/utils.cmake)

function(ladybird_generate_export_header name fs_name)
    # Temporary helper to allow libraries to opt-in to using X_API macros
    # to export symbols required by external consumers. This allows the codebase
    # to gradually slowly migrate instead of an all-or-nothing approach.
    if (NOT WIN32)
        target_compile_options(${name}
            PRIVATE
                "$<$<COMPILE_LANGUAGE:CXX>:-fvisibility=hidden>"
                "$<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>"
        )
    else()
        set_target_properties(${name} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    endif()
    include(GenerateExportHeader)
    string(TOUPPER ${fs_name} fs_name_upper)
    generate_export_header(${name} EXPORT_MACRO_NAME ${fs_name_upper}_API EXPORT_FILE_NAME "Export.h")
endfunction()

function(ladybird_generate_dsym target_name)
    if (APPLE AND LADYBIRD_GENERATE_DSYM)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND dsymutil -o "$<TARGET_FILE:${target_name}>.dSYM" "$<TARGET_FILE:${target_name}>"
            COMMENT "Generating dSYM for ${target_name}"
        )
    endif()
endfunction()

function(ladybird_copy_runtime_dlls target_name)
    if (WIN32 AND BUILD_SHARED_LIBS)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target_name}>
                $<TARGET_FILE_DIR:${target_name}>
            COMMAND_EXPAND_LISTS
        )
    endif()
endfunction()

# https://learn.microsoft.com/en-us/cpp/build/reference/subsystem-specify-subsystem?view=msvc-170
# Add /SUBSYSTEM:WINDOWS linker flag and defines the default WinMain. This makes the executable target not launch with a console
function(ladybird_subsystem_windows target_name)
    if(WIN32)
        set_target_properties(${target_name} PROPERTIES
            WIN32_EXECUTABLE TRUE
            LINK_FLAGS "/ENTRY:mainCRTStartup"
        )
    endif()
endfunction()

function(ladybird_windows_bin target_name)
    cmake_parse_arguments(LAGOM_WINDOWS_BIN "CONSOLE" "" "" ${ARGN})
    ladybird_copy_runtime_dlls(${target_name})
    if (NOT LAGOM_WINDOWS_BIN_CONSOLE)
        ladybird_subsystem_windows(${target_name})
    else()
        set_target_properties(${target_name} PROPERTIES
            WIN32_EXECUTABLE FALSE
        )
    endif()
endfunction()

function(ladybird_bin name)
    add_executable(${name} ${SOURCES} ${GENERATED_SOURCES})
    add_executable(Lagom::${name} ALIAS ${name})
    target_link_libraries(${name} PUBLIC GenericClangPlugin)
    ladybird_windows_bin(${name})
    ladybird_generate_dsym(${name})
    install(
            TARGETS ${target_name}
            EXPORT LagomTargets
            RUNTIME #
            COMPONENT Lagom_Runtime
            LIBRARY #
            COMPONENT Lagom_Runtime
            NAMELINK_COMPONENT Lagom_Development
            ARCHIVE #
            COMPONENT Lagom_Development
            INCLUDES #
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    ladybird_generated_sources(${name})
endfunction()

function(ladybird_lib name fs_name)
    cmake_parse_arguments(PARSE_ARGV 2 LADYBIRD_LIB "EXPLICIT_SYMBOL_EXPORT" "TYPE" "")
    string(REPLACE "Lib" "" library ${name})
    if (NOT LADYBIRD_LIB_TYPE)
        set(LADYBIRD_LIB_TYPE "")
    endif()

    add_library(${name} ${LADYBIRD_LIB_TYPE} ${SOURCES} ${GENERATED_SOURCES})
    set_target_properties(
            ${name} PROPERTIES
            VERSION "${PROJECT_VERSION}"
            SOVERSION "${PROJECT_VERSION_MAJOR}"
            EXPORT_NAME ${library}
            OUTPUT_NAME lagom-${fs_name}
    )
    target_link_libraries(${name} PRIVATE ${LIBS})
    target_link_libraries(${name} PUBLIC GenericClangPlugin)

    if (NOT "${name}" STREQUAL "AK")
        target_link_libraries(${name} PRIVATE AK)
    endif()

    if (WIN32)
        target_include_directories(${name} PRIVATE ${PTHREAD_INCLUDE_DIR})
        target_link_libraries(${name} PRIVATE ${PTHREAD_LIBRARY})

        target_include_directories(${name} PRIVATE ${MMAN_INCLUDE_DIR})
        target_link_libraries(${name} PRIVATE ${MMAN_LIBRARY})
    endif()

    if (NOT LADYBIRD_LIB_TYPE STREQUAL "STATIC")
        ladybird_generate_dsym(${name})
    endif()

    # FIXME: Clean these up so that we don't need so many include dirs
    if (ENABLE_INSTALL_HEADERS)
        target_include_directories(${name} INTERFACE
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Services>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Libraries>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Services>
        )
    endif()
    add_lagom_library_install_rules(${name} ALIAS_NAME ${library})
    if (ENABLE_INSTALL_HEADERS)
        install(
                DIRECTORY "${LADYBIRD_SOURCE_DIR}/Libraries/Lib${library}"
                COMPONENT Lagom_Development
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
                FILES_MATCHING PATTERN "*.h"
        )
    endif()
    ladybird_generated_sources(${name})
    if (LADYBIRD_LIB_EXPLICIT_SYMBOL_EXPORT)
        ladybird_generate_export_header(${name} ${fs_name})
    endif()
endfunction()

function(ladybird_test test_src sub_dir)
    cmake_parse_arguments(PARSE_ARGV 2 LADYBIRD_TEST "" "CUSTOM_MAIN;NAME" "LIBS")
    if (NOT LADYBIRD_TEST_NAME)
        get_filename_component(LADYBIRD_TEST_NAME ${test_src} NAME_WE)
    endif()
    if (NOT LADYBIRD_TEST_CUSTOM_MAIN)
        set(LADYBIRD_TEST_CUSTOM_MAIN "$<TARGET_OBJECTS:LibTestMain>")
    endif()
    add_executable(${LADYBIRD_TEST_NAME} ${test_src})
    target_link_libraries(${LADYBIRD_TEST_NAME} PRIVATE AK LibCore LibFileSystem LibTest ${LADYBIRD_TEST_CUSTOM_MAIN} ${LADYBIRD_TEST_LIBS})
    ladybird_windows_bin(${LADYBIRD_TEST_NAME} CONSOLE)
    ladybird_generate_dsym(${LADYBIRD_TEST_NAME})

    if (WIN32)
        target_include_directories(${LADYBIRD_TEST_NAME} PRIVATE ${PTHREAD_INCLUDE_DIR})
        target_link_libraries(${LADYBIRD_TEST_NAME} PRIVATE ${PTHREAD_LIBRARY})

        target_include_directories(${LADYBIRD_TEST_NAME} PRIVATE ${MMAN_INCLUDE_DIR})
        target_link_libraries(${LADYBIRD_TEST_NAME} PRIVATE ${MMAN_LIBRARY})
    endif()

    add_test(
            NAME ${LADYBIRD_TEST_NAME}
            COMMAND ${LADYBIRD_TEST_NAME}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endfunction()

function(ladybird_utility name)
    cmake_parse_arguments(LADYBIRD_UTILITY "" "" "SOURCES;LIBS" ${ARGN})

    add_executable("${name}" ${LADYBIRD_UTILITY_SOURCES})
    target_link_libraries("${name}" PRIVATE AK LibCore ${LADYBIRD_UTILITY_LIBS})
    ladybird_generate_dsym(${name})
endfunction()

if (NOT TARGET ladybird_codegen_accumulator)
    # Meta target to run all code-gen steps in the build.
    add_custom_target(ladybird_codegen_accumulator)
endif()
