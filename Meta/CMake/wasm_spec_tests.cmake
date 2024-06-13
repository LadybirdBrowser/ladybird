#
# Download and compile the WebAssembly testsuite into the WebAssembly binary format
#

if(INCLUDE_WASM_SPEC_TESTS)
    set(WASM_SPEC_TEST_GZ_URL https://github.com/WebAssembly/testsuite/archive/refs/heads/main.tar.gz)
    set(WASM_SPEC_TEST_GZ_PATH ${CMAKE_BINARY_DIR}/wasm-spec-testsuite.tar.gz CACHE PATH "")
    set(WASM_SPEC_TEST_PATH ${CMAKE_CURRENT_BINARY_DIR}/Tests/Fixtures/SpecTests CACHE PATH "")

    download_file(${WASM_SPEC_TEST_GZ_URL} ${WASM_SPEC_TEST_GZ_PATH})

    set(SKIP_PRETTIER false)
    if (WASM_SPEC_TEST_SKIP_FORMATTING)
        set(SKIP_PRETTIER true)
    endif()

    find_program(WAT2WASM wat2wasm REQUIRED)
    find_program(PRETTIER prettier OPTIONAL)
    if (NOT SKIP_PRETTIER AND PRETTIER EQUAL "PRETTIER-NOTFOUND")
       message(FATAL_ERROR "Prettier required to format Wasm spec tests! Install prettier or set WASM_SPEC_TEST_SKIP_FORMATTING to ON")
    endif()

    if(EXISTS ${WASM_SPEC_TEST_GZ_PATH} AND NOT EXISTS ${WASM_SPEC_TEST_PATH}/const_0.wasm)
        message(STATUS "Extracting the WebAssembly testsuite from ${WASM_SPEC_TEST_GZ_PATH}...")
        extract_path("${CMAKE_CURRENT_BINARY_DIR}" "${WASM_SPEC_TEST_GZ_PATH}" "testsuite-main/*.wast" "${WASM_SPEC_TEST_PATH}")
        file(MAKE_DIRECTORY ${WASM_SPEC_TEST_PATH})
        file(GLOB WASM_TESTS "${CMAKE_CURRENT_BINARY_DIR}/testsuite-main/*.wast")
        foreach(PATH ${WASM_TESTS})
            get_filename_component(NAME ${PATH} NAME_WLE)
            message(STATUS "Generating test cases for WebAssembly test ${NAME}...")
            execute_process(
                COMMAND env SKIP_PRETTIER=${SKIP_PRETTIER} bash ${SerenityOS_SOURCE_DIR}/Meta/generate-libwasm-spec-test.sh "${PATH}" "${CMAKE_CURRENT_BINARY_DIR}/Tests/Spec" "${NAME}" "${WASM_SPEC_TEST_PATH}")
        endforeach()
        file(REMOVE testsuite-main)
    endif()

    # FIXME: Install these into usr/Tests/LibWasm
    if (SERENITYOS)
        install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Tests/ DESTINATION home/anon/Tests/wasm-tests)
    endif()
endif()
