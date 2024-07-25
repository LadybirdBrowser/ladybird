include(clang_development)
include(AddLLVM)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_property(CLANG_PLUGINS_COMPILE_OPTIONS_FOR_TESTS GLOBAL PROPERTY CLANG_PLUGINS_COMPILE_OPTIONS_FOR_TESTS)
list(APPEND CLANG_PLUGINS_COMPILE_OPTIONS_FOR_TESTS -std=c++23 -Wno-user-defined-literals -Wno-literal-range -Wno-unknown-warning-option)

# Ensure we always check for invalid function field types regardless of the value of ENABLE_CLANG_PLUGINS_INVALID_FUNCTION_MEMBERS
# FIXME: Enabling this with lit and llvm 18 seems to not work as expected
# list(APPEND CLANG_PLUGINS_COMPILE_OPTIONS_FOR_TESTS -fplugin-arg-libjs_gc_scanner-detect-invalid-function-members)

get_property(CLANG_PLUGINS_INCLUDE_DIRECTORIES TARGET AK PROPERTY INCLUDE_DIRECTORIES)
list(APPEND CLANG_PLUGINS_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})

configure_lit_site_cfg(
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
    ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
    MAIN_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
    PATHS
        LLVM_BINARY_DIR
        LLVM_TOOLS_DIR
        LLVM_LIBS_DIR
        CMAKE_LIBRARY_OUTPUT_DIRECTORY
        CMAKE_CURRENT_SOURCE_DIR
)

add_custom_command(
    OUTPUT venv
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/requirements.txt requirements.txt
    COMMAND ${Python3_EXECUTABLE} -m venv venv
    COMMAND ./venv/bin/pip install -r requirements.txt --upgrade
)
add_custom_target(TestClangPluginsDependencies ALL
    DEPENDS venv JSClangPlugin GenericClangPlugin
    SOURCES requirements.txt
)
add_test(
    NAME TestClangPlugins
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/venv/bin/lit -v .
)
