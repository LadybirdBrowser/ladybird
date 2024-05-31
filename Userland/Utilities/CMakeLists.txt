file(GLOB CMD_SOURCES  CONFIGURE_DEPENDS "*.cpp")
list(APPEND REQUIRED_TARGETS
    file
)
list(APPEND RECOMMENDED_TARGETS
    aconv adjtime abench gzip lzcat tar unzip xzcat zip
)

# FIXME: Support specifying component dependencies for utilities (e.g. WebSocket for telws)

foreach(CMD_SRC ${CMD_SOURCES})
    get_filename_component(CMD_NAME ${CMD_SRC} NAME_WLE)
    if (CMD_NAME IN_LIST SPECIAL_TARGETS)
        set(TARGET_NAME "${CMD_NAME}-bin")
    else()
        set(TARGET_NAME "${CMD_NAME}")
    endif()
    if(CMD_NAME IN_LIST REQUIRED_TARGETS)
        serenity_component(
            ${CMD_NAME}
            REQUIRED
            TARGETS ${TARGET_NAME}
        )
    elseif(CMD_NAME IN_LIST RECOMMENDED_TARGETS)
        serenity_component(
            ${CMD_NAME}
            RECOMMENDED
            TARGETS ${TARGET_NAME}
        )
    else()
        serenity_component(
            ${CMD_NAME}
            TARGETS ${TARGET_NAME}
        )
    endif()

    add_executable(${TARGET_NAME} ${CMD_SRC})
    target_link_libraries(${TARGET_NAME} PRIVATE LibCore LibMain)
    serenity_set_implicit_links(${TARGET_NAME})
    set_target_properties(${TARGET_NAME} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    install(TARGETS ${TARGET_NAME} RUNTIME DESTINATION bin OPTIONAL)

    if (CMD_NAME IN_LIST SPECIAL_TARGETS)
        install(CODE
            "if (EXISTS ${CMAKE_INSTALL_PREFIX}/bin/${CMD_NAME}-bin)
                file(RENAME ${CMAKE_INSTALL_PREFIX}/bin/${CMD_NAME}-bin ${CMAKE_INSTALL_PREFIX}/bin/${CMD_NAME})
            endif()")
    endif()
endforeach()

if (ENABLE_JAKT)
    add_jakt_executable(hello-jakt
        MAIN_SOURCE hello-world.jakt
    )
    target_link_libraries(hello-jakt PRIVATE LibC)
    set_target_properties(hello-jakt PROPERTIES EXCLUDE_FROM_ALL TRUE)
    install(TARGETS hello-jakt RUNTIME DESTINATION bin OPTIONAL)
    serenity_component(
        hello-jakt
        RECOMMENDED
        TARGETS hello-jakt
    )
endif()

install(CODE "file(CREATE_LINK grep ${CMAKE_INSTALL_PREFIX}/bin/egrep SYMBOLIC)")
install(CODE "file(CREATE_LINK grep ${CMAKE_INSTALL_PREFIX}/bin/fgrep SYMBOLIC)")
install(CODE "file(CREATE_LINK grep ${CMAKE_INSTALL_PREFIX}/bin/rgrep SYMBOLIC)")
install(CODE "file(CREATE_LINK gzip ${CMAKE_INSTALL_PREFIX}/bin/gunzip SYMBOLIC)")
install(CODE "file(CREATE_LINK gzip ${CMAKE_INSTALL_PREFIX}/bin/zcat SYMBOLIC)")
install(CODE "file(CREATE_LINK /usr/lib/Loader.so ${CMAKE_INSTALL_PREFIX}/bin/ldd SYMBOLIC)")

target_link_libraries(abench PRIVATE LibAudio LibFileSystem)
target_link_libraries(aconv PRIVATE LibAudio LibFileSystem)
target_link_libraries(animation PRIVATE LibGfx)
target_link_libraries(file PRIVATE LibELF LibGfx LibIPC LibArchive LibCompress LibAudio)
target_link_libraries(gzip PRIVATE LibCompress)
target_link_libraries(headless-browser PRIVATE LibCrypto LibFileSystem LibGemini LibGfx LibHTTP LibImageDecoderClient LibTLS LibWeb LibWebView LibWebSocket LibIPC LibJS LibDiff LibURL)
target_link_libraries(icc PRIVATE LibGfx LibVideo LibURL)
target_link_libraries(image PRIVATE LibGfx)
target_link_libraries(isobmff PRIVATE LibGfx)
target_link_libraries(js PRIVATE LibCrypto LibJS LibLine LibLocale LibTextCodec)
target_link_libraries(lzcat PRIVATE LibCompress)
target_link_libraries(pdf PRIVATE LibGfx LibPDF)
target_link_libraries(sql PRIVATE LibFileSystem LibIPC LibLine LibSQL)
target_link_libraries(tar PRIVATE LibArchive LibCompress LibFileSystem)
target_link_libraries(test-jpeg-roundtrip PRIVATE LibGfx)
target_link_libraries(ttfdisasm PRIVATE LibGfx)
target_link_libraries(unzip PRIVATE LibArchive LibCompress LibCrypto LibFileSystem)
target_link_libraries(wasm PRIVATE LibFileSystem LibJS LibLine LibWasm)
target_link_libraries(xml PRIVATE LibFileSystem LibXML LibURL)
target_link_libraries(xzcat PRIVATE LibCompress)
target_link_libraries(zip PRIVATE LibArchive LibFileSystem)

# FIXME: Link this file into headless-browser without compiling it again.
target_sources(headless-browser PRIVATE "${SerenityOS_SOURCE_DIR}/Userland/Services/WebContent/WebDriverConnection.cpp")

include("${SerenityOS_SOURCE_DIR}/Meta/Lagom/Fuzzers/fuzzers.cmake")

foreach(name IN LISTS FUZZER_TARGETS)
    set(fuzz_source_file "${SerenityOS_SOURCE_DIR}/Meta/Lagom/Fuzzers/Fuzz${name}.cpp")
    target_sources(test-fuzz PRIVATE "${fuzz_source_file}")
    set_source_files_properties("${fuzz_source_file}" PROPERTIES COMPILE_DEFINITIONS "LLVMFuzzerTestOneInput=Test${name}")
    set_source_files_properties("${fuzz_source_file}" PROPERTIES COMPILE_OPTIONS "-Wno-missing-declarations")
    target_link_libraries(test-fuzz PRIVATE "${FUZZER_DEPENDENCIES_${name}}")
endforeach()
