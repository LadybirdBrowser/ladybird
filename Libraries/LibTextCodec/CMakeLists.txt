include(libtextcodec_generators)

set(SOURCES
    Decoder.cpp
    Encoder.cpp
)

generate_encoding_indexes()

set(GENERATED_SOURCES
    LookupTables.cpp
)

ladybird_lib(LibTextCodec textcodec EXPLICIT_SYMBOL_EXPORT)
