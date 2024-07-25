include(clang_development)

function(clang_plugin target_name)
    cmake_parse_arguments(CLANG_PLUGIN "" "" "SOURCES" ${ARGN})
    add_library(${target_name} ${CLANG_PLUGIN_SOURCES})
    target_include_directories(${target_name} SYSTEM PRIVATE ${CLANG_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Werror -Wno-unused -fno-rtti)

    # Building these with fuzzers would require building the compiler with fuzzers, which feels out of scope
    target_compile_options(${target_name} PRIVATE -fno-sanitize=all)
    target_link_options(${target_name} PRIVATE -fno-sanitize=all)

    add_custom_target(${target_name}Target DEPENDS ${target_name})

    set_property(GLOBAL APPEND PROPERTY CLANG_PLUGINS_COMPILE_OPTIONS_FOR_TESTS -fplugin=${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/lib${target_name}.so)

    add_lagom_library_install_rules(${target_name})
endfunction()

function(depend_on_clang_plugin target_name plugin_name)
    if (TARGET ${plugin_name}Target)
        add_dependencies(${target_name} ${plugin_name}Target)
    endif()
    target_compile_options(${target_name} INTERFACE -fplugin=$<TARGET_FILE:Lagom::${plugin_name}>)
    if (${ENABLE_CLANG_PLUGINS_INVALID_FUNCTION_MEMBERS})
        target_compile_options(${target_name} INTERFACE -fplugin-arg-libjs_gc_scanner-detect-invalid-function-members)
    endif()
endfunction()

clang_plugin(LambdaCaptureClangPlugin SOURCES LambdaCapturePluginAction.cpp)
clang_plugin(LibJSGCClangPlugin SOURCES LibJSGCPluginAction.cpp)
