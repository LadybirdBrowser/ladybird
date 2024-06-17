set(FUZZER_TARGETS
    ASN1
    Base64Roundtrip
    BLAKE2b
    BMPLoader
    Brotli
    DeflateCompression
    DeflateDecompression
    FlacLoader
    GIFLoader
    GzipDecompression
    GzipRoundtrip
    HttpRequest
    ICCProfile
    ICOLoader
    JPEG2000Loader
    JPEGLoader
    Js
    JsonParser
    LzmaDecompression
    LzmaRoundtrip
    MatroskaReader
    MD5
    MP3Loader
    PEM
    PNGLoader
    Poly1305
    QOALoader
    RegexECMA262
    RegexPosixBasic
    RegexPosixExtended
    RSAKeyParsing
    SHA1
    SHA256
    SHA384
    SHA512
    Tar
    TextDecoder
    TIFFLoader
    TTF
    TinyVGLoader
    URL
    VP9Decoder
    WasmParser
    WAVLoader
    WebPLoader
    WOFF
    WOFF2
    XML
    Zip
    ZlibDecompression
)

if (TARGET LibWeb)
    list(APPEND FUZZER_TARGETS CSSParser)
endif()

set(FUZZER_DEPENDENCIES_ASN1 LibCrypto LibTLS)
set(FUZZER_DEPENDENCIES_BLAKE2b LibCrypto)
set(FUZZER_DEPENDENCIES_BMPLoader LibGfx)
set(FUZZER_DEPENDENCIES_Brotli LibCompress)
set(FUZZER_DEPENDENCIES_CSSParser LibWeb)
set(FUZZER_DEPENDENCIES_DeflateCompression LibCompress)
set(FUZZER_DEPENDENCIES_DeflateDecompression LibCompress)
set(FUZZER_DEPENDENCIES_ELF LibELF)
set(FUZZER_DEPENDENCIES_FlacLoader LibAudio)
set(FUZZER_DEPENDENCIES_GIFLoader LibGfx)
set(FUZZER_DEPENDENCIES_GzipDecompression LibCompress)
set(FUZZER_DEPENDENCIES_GzipRoundtrip LibCompress)
set(FUZZER_DEPENDENCIES_HttpRequest LibHTTP)
set(FUZZER_DEPENDENCIES_ICCProfile LibGfx)
set(FUZZER_DEPENDENCIES_ICOLoader LibGfx)
set(FUZZER_DEPENDENCIES_JPEG2000Loader LibGfx)
set(FUZZER_DEPENDENCIES_JPEGLoader LibGfx)
set(FUZZER_DEPENDENCIES_Js LibJS)
set(FUZZER_DEPENDENCIES_LzmaDecompression LibArchive LibCompress)
set(FUZZER_DEPENDENCIES_LzmaRoundtrip LibCompress)
set(FUZZER_DEPENDENCIES_MatroskaReader LibVideo)
set(FUZZER_DEPENDENCIES_MD5 LibCrypto)
set(FUZZER_DEPENDENCIES_MP3Loader LibAudio)
set(FUZZER_DEPENDENCIES_PEM LibCrypto)
set(FUZZER_DEPENDENCIES_PNGLoader LibGfx)
set(FUZZER_DEPENDENCIES_Poly1305 LibCrypto)
set(FUZZER_DEPENDENCIES_QOALoader LibAudio)
set(FUZZER_DEPENDENCIES_RegexECMA262 LibRegex)
set(FUZZER_DEPENDENCIES_RegexPosixBasic LibRegex)
set(FUZZER_DEPENDENCIES_RegexPosixExtended LibRegex)
set(FUZZER_DEPENDENCIES_RSAKeyParsing LibCrypto)
set(FUZZER_DEPENDENCIES_SHA1 LibCrypto)
set(FUZZER_DEPENDENCIES_SHA256 LibCrypto)
set(FUZZER_DEPENDENCIES_SHA384 LibCrypto)
set(FUZZER_DEPENDENCIES_SHA512 LibCrypto)
set(FUZZER_DEPENDENCIES_Tar LibArchive)
set(FUZZER_DEPENDENCIES_TextDecoder LibTextCodec)
set(FUZZER_DEPENDENCIES_TIFFLoader LibGfx)
set(FUZZER_DEPENDENCIES_TTF LibGfx)
set(FUZZER_DEPENDENCIES_TinyVGLoader LibGfx)
set(FUZZER_DEPENDENCIES_URL LibURL)
set(FUZZER_DEPENDENCIES_VP9Decoder LibVideo)
set(FUZZER_DEPENDENCIES_WasmParser LibWasm)
set(FUZZER_DEPENDENCIES_WAVLoader LibAudio)
set(FUZZER_DEPENDENCIES_WebPLoader LibGfx)
set(FUZZER_DEPENDENCIES_WOFF LibGfx)
set(FUZZER_DEPENDENCIES_WOFF2 LibGfx)
set(FUZZER_DEPENDENCIES_XML LibXML)
set(FUZZER_DEPENDENCIES_Zip LibArchive)
set(FUZZER_DEPENDENCIES_ZlibDecompression LibCompress)
