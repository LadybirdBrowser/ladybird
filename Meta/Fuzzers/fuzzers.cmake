set(FUZZER_TARGETS
    ASN1
    Base64Roundtrip
    BMPLoader
    GIFLoader
    ICOLoader
    Js
    JsonParser
    MatroskaReader
    PEM
    PNGLoader
    RegexECMA262
    RSAKeyParsing
    TextDecoder
    URL
    WasmParser
    WOFF
    XML
)

if (TARGET LibWeb)
    list(APPEND FUZZER_TARGETS CSSParser)
endif()

set(FUZZER_DEPENDENCIES_ASN1 LibCrypto LibTLS)
set(FUZZER_DEPENDENCIES_BMPLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_CSSParser LibWeb)
set(FUZZER_DEPENDENCIES_ELF LibELF)
set(FUZZER_DEPENDENCIES_GIFLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_ICOLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_Js LibJS LibGC)
set(FUZZER_DEPENDENCIES_MatroskaReader LibMedia)
set(FUZZER_DEPENDENCIES_PEM LibCrypto)
set(FUZZER_DEPENDENCIES_PNGLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_Poly1305 LibCrypto)
set(FUZZER_DEPENDENCIES_RegexECMA262 LibRegex)
set(FUZZER_DEPENDENCIES_RSAKeyParsing LibCrypto)
set(FUZZER_DEPENDENCIES_TextDecoder LibTextCodec)
set(FUZZER_DEPENDENCIES_TTF LibGfx)
set(FUZZER_DEPENDENCIES_URL LibURL)
set(FUZZER_DEPENDENCIES_WasmParser LibWasm)
set(FUZZER_DEPENDENCIES_WOFF LibGfx)
set(FUZZER_DEPENDENCIES_XML LibXML)
