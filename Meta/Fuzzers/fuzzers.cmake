set(FUZZER_TARGETS
    Base64Roundtrip
    BMPLoader
    GIFLoader
    ICOLoader
    Js
    JsonParser
    MatroskaReader
    PNGLoader
    RegexECMA262
    TextDecoder
    URL
    WasmParser
    WOFF
    XML
)

if (TARGET LibWeb)
    list(APPEND FUZZER_TARGETS CSSParser)
endif()

set(FUZZER_DEPENDENCIES_BMPLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_CSSParser LibWeb)
set(FUZZER_DEPENDENCIES_GIFLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_ICOLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_Js LibJSStandalone LibGC)
set(FUZZER_DEPENDENCIES_MatroskaReader LibMedia)
set(FUZZER_DEPENDENCIES_PNGLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_RegexECMA262 LibRegex)
set(FUZZER_DEPENDENCIES_TextDecoder LibTextCodec)
set(FUZZER_DEPENDENCIES_URL LibURL)
set(FUZZER_DEPENDENCIES_WasmParser LibWasm)
set(FUZZER_DEPENDENCIES_WOFF LibGfx)
set(FUZZER_DEPENDENCIES_XML LibXML)
