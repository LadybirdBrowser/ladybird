include(code_generators)

set(SOURCES
    Decoder.cpp
    Encoder.cpp
)
# NOTE: PY_ prefix is used to avoid hooking into the existing C++ generator infrastructure
generate_encoding_indices(PY_GENERATED_SOURCES)
list(APPEND SOURCES ${PY_GENERATED_SOURCES})

serenity_lib(LibTextCodec textcodec)
