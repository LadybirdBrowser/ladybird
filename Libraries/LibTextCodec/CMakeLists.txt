include(libtextcodec_generators)

set(SOURCES
    Decoder.cpp
    Encoder.cpp
)

generate_encoding_indexes()

set(GENERATED_SOURCES
    LookupTables.cpp
)

serenity_lib(LibTextCodec textcodec)
