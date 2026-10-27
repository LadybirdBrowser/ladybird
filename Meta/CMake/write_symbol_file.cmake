# Writes the debug info of the ELF binary BINARY to SYMBOL_FILES_DIR/.build-id/xx/yyyy.debug, where debuggers and
# debuginfod look for it by build ID. The STAMP file remembers that path, so a relinked binary replaces its old file.

execute_process(COMMAND "${READELF}" --notes "${BINARY}" OUTPUT_VARIABLE notes COMMAND_ERROR_IS_FATAL ANY)
if (NOT notes MATCHES "Build ID: ([0-9a-f][0-9a-f])([0-9a-f]+)")
    message(FATAL_ERROR "${BINARY} has no build ID")
endif()
set(symbol_file "${SYMBOL_FILES_DIR}/.build-id/${CMAKE_MATCH_1}/${CMAKE_MATCH_2}.debug")

if (EXISTS "${STAMP}")
    file(READ "${STAMP}" previous_symbol_file)
    if (NOT previous_symbol_file STREQUAL symbol_file)
        file(REMOVE "${previous_symbol_file}")
    endif()
endif()

get_filename_component(symbol_file_dir "${symbol_file}" DIRECTORY)
file(MAKE_DIRECTORY "${symbol_file_dir}")
execute_process(COMMAND "${OBJCOPY}" --only-keep-debug "${BINARY}" "${symbol_file}" COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${STAMP}" "${symbol_file}")
