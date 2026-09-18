set(FUZZER_TARGETS
    ASN1
    Base64Roundtrip
    BMPLoader
    CanvasCommandCodec
    CompressionRoundtrip
    CompressionStreaming
    DNSMessage
    GIFLoader
    HTTPHeaderList
    ICOLoader
    ImageDecoderFrames
    IPCDecoder
    Js
    JsonParser
    MatroskaReader
    MatroskaSamples
    PEM
    PNGLoader
    RegexECMA262
    RSAKeyParsing
    TextDecoder
    TextDecoderStreaming
    URL
    WasmParser
    WasmExecution
    WOFF
    XML
)

set(LOCAL_ONLY_FUZZER_TARGETS
    # Validation interns arbitrary recursive types in an immortal process-wide registry.
    # Keep it out of persistent hosted workers until the registry can be bounded or reset.
    WasmValidation
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND FUZZER_TARGETS IPCSendQueue IPCTransport)
    if (ENABLE_GUI_TARGETS)
        list(APPEND LOCAL_ONLY_FUZZER_TARGETS RequestServerSequence RequestServerRecipients CompositorCanvasSequence CompositorWebGLSequence ImageDecoderLifecycle BrowserSessionState)
    endif()
endif()

if (NOT ENABLE_FUZZERS_OSSFUZZ)
    list(APPEND FUZZER_TARGETS ${LOCAL_ONLY_FUZZER_TARGETS})
endif()

if (TARGET LibWeb)
    list(APPEND FUZZER_TARGETS CSSParser)
endif()

set(FUZZER_DEPENDENCIES_ASN1 LibCrypto LibTLS)
set(FUZZER_DEPENDENCIES_BMPLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_BrowserSessionState LibWebView LibWeb LibHTTP LibCore)
set(FUZZER_DEPENDENCIES_CanvasCommandCodec LibGfx LibIPC)
set(FUZZER_DEPENDENCIES_CompositorCanvasSequence compositorservice LibWeb LibGfx LibIPC LibMedia skia)
set(FUZZER_DEPENDENCIES_CompositorWebGLSequence compositorservice LibWeb LibGfx LibIPC LibMedia skia)
set(FUZZER_DEPENDENCIES_CompressionRoundtrip LibCompress)
set(FUZZER_DEPENDENCIES_CompressionStreaming LibCompress)
set(FUZZER_DEPENDENCIES_CSSParser LibWeb)
set(FUZZER_DEPENDENCIES_DNSMessage LibDNS)
set(FUZZER_DEPENDENCIES_ELF LibELF)
set(FUZZER_DEPENDENCIES_GIFLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_HTTPHeaderList LibHTTP)
set(FUZZER_DEPENDENCIES_ICOLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_ImageDecoderFrames LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_ImageDecoderLifecycle imagedecoderservice LibGfx LibImageDecoders LibIPC LibCore LibThreading)
set(FUZZER_DEPENDENCIES_IPCDecoder LibIPC LibURL)
set(FUZZER_DEPENDENCIES_IPCSendQueue LibIPC)
set(FUZZER_DEPENDENCIES_IPCTransport LibIPC)
set(FUZZER_DEPENDENCIES_Js LibJS LibGC)
set(FUZZER_DEPENDENCIES_MatroskaReader LibMedia)
set(FUZZER_DEPENDENCIES_MatroskaSamples LibMedia)
set(FUZZER_DEPENDENCIES_PEM LibCrypto)
set(FUZZER_DEPENDENCIES_PNGLoader LibGfx LibImageDecoders)
set(FUZZER_DEPENDENCIES_Poly1305 LibCrypto)
set(FUZZER_DEPENDENCIES_RegexECMA262 LibRegex)
set(FUZZER_DEPENDENCIES_RSAKeyParsing LibCrypto)
set(FUZZER_DEPENDENCIES_RequestServerSequence requestserverservice LibIPC)
set(FUZZER_DEPENDENCIES_RequestServerRecipients requestserverservice LibIPC LibCore)
set(FUZZER_DEPENDENCIES_TextDecoder LibTextCodec)
set(FUZZER_DEPENDENCIES_TextDecoderStreaming LibTextCodec)
set(FUZZER_DEPENDENCIES_TTF LibGfx)
set(FUZZER_DEPENDENCIES_URL LibURL)
set(FUZZER_DEPENDENCIES_WasmParser LibWasm)
set(FUZZER_DEPENDENCIES_WasmExecution LibWasm LibGC)
set(FUZZER_DEPENDENCIES_WasmValidation LibWasm LibGC)
set(FUZZER_DEPENDENCIES_WOFF LibGfx)
set(FUZZER_DEPENDENCIES_XML LibXML)
