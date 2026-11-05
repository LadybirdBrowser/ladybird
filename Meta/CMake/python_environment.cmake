include_guard()

set(python_environment_script "${LADYBIRD_SOURCE_DIR}/Meta/Utils/python_environment.py")
set(python_requirements "${LADYBIRD_SOURCE_DIR}/requirements.txt")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${python_environment_script}" "${python_requirements}")

# CI may configure with the managed interpreter. Use its base Python for setup, so recreating the environment does not
# remove the interpreter running setup.
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import os, sys; print(os.path.realpath(getattr(sys, '_base_executable', sys.executable)))"
    OUTPUT_VARIABLE python_environment_bootstrap
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)

# Resolve the content-addressed path without creating the environment during configuration.
execute_process(
    COMMAND "${python_environment_bootstrap}" "${python_environment_script}" --print-python
    OUTPUT_VARIABLE LADYBIRD_TEST_PYTHON
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
get_filename_component(python_environment_bin "${LADYBIRD_TEST_PYTHON}" DIRECTORY)
get_filename_component(python_environment "${python_environment_bin}" DIRECTORY)

add_custom_command(
    OUTPUT "${python_environment}/.ready"
    COMMAND "${python_environment_bootstrap}" "${python_environment_script}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${python_environment}/.ready"
    DEPENDS "${python_environment_script}" "${python_requirements}"
    COMMENT "Preparing Python environment for tests and linters"
    VERBATIM
)
add_custom_target(ladybird_python_environment DEPENDS "${python_environment}/.ready")
