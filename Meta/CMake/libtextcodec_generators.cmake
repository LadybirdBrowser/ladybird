function(generate_encoding_indexes)
    set(LIBTEXTCODEC_INPUT_FOLDER "${CMAKE_CURRENT_SOURCE_DIR}")

    # indexes.json can be found at https://encoding.spec.whatwg.org/indexes.json
    invoke_py_generator(
            "LookupTables.cpp"
            "generate_encoding_indexes.py"
            "${LIBTEXTCODEC_INPUT_FOLDER}/indexes.json"
            "LookupTables.h"
            "LookupTables.cpp"
            arguments -j "${LIBTEXTCODEC_INPUT_FOLDER}/indexes.json"
    )

    if(ENABLE_INSTALL_HEADERS)
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LookupTables.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/LibTextCodec/")
    endif()
endfunction()
