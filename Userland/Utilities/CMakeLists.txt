set(CMD_SOURCES
    abench.cpp
    aconv.cpp
    animation.cpp
    gzip.cpp
    headless-browser.cpp
    icc.cpp
    image.cpp
    isobmff.cpp
    js.cpp
    lzcat.cpp
    shred.cpp
    sql.cpp
    tar.cpp
    test-jpeg-roundtrip.cpp
    ttfdisasm.cpp
    unzip.cpp
    wasm.cpp
    xml.cpp
    xzcat.cpp
    zip.cpp
)

foreach(CMD_SRC IN LISTS CMD_SOURCES)
    get_filename_component(CMD_NAME ${CMD_SRC} NAME_WLE)

    add_executable(${CMD_NAME} ${CMD_SRC})
    target_link_libraries(${CMD_NAME} PRIVATE LibCore LibMain)
    serenity_set_implicit_links(${CMD_NAME})
    set_target_properties(${CMD_NAME} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    install(TARGETS ${CMD_NAME} RUNTIME DESTINATION bin OPTIONAL)
endforeach()

install(CODE "file(CREATE_LINK gzip ${CMAKE_INSTALL_PREFIX}/bin/gunzip SYMBOLIC)")
install(CODE "file(CREATE_LINK gzip ${CMAKE_INSTALL_PREFIX}/bin/zcat SYMBOLIC)")

target_link_libraries(abench PRIVATE LibAudio LibFileSystem)
target_link_libraries(aconv PRIVATE LibAudio LibFileSystem)
target_link_libraries(animation PRIVATE LibGfx)
target_link_libraries(gzip PRIVATE LibCompress)
target_link_libraries(headless-browser PRIVATE LibCrypto LibFileSystem LibGfx LibHTTP LibImageDecoderClient LibTLS LibWeb LibWebView LibWebSocket LibIPC LibJS LibDiff LibURL)
target_link_libraries(icc PRIVATE LibGfx LibVideo LibURL)
target_link_libraries(image PRIVATE LibGfx)
target_link_libraries(isobmff PRIVATE LibGfx)
target_link_libraries(js PRIVATE LibCrypto LibJS LibLine LibLocale LibTextCodec)
target_link_libraries(lzcat PRIVATE LibCompress)
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
