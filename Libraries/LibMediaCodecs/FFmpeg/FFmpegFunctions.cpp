/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>
#include <LibMediaCodecs/FFmpeg/FFmpegHelpers.h>

#ifdef LADYBIRD_SYSTEM_FFMPEG_NAMESPACE
#    include <dlfcn.h>
#endif

namespace FFMPEG_ABI_NAMESPACE {

static constexpr FFmpegFunctions::CodecIDTable codec_ids_of_these_headers()
{
    FFmpegFunctions::CodecIDTable codec_ids {};
    for (auto codec_id : all_codec_ids)
        codec_ids[to_underlying(codec_id)] = ffmpeg_codec_id_from_media_codec_id(codec_id);
    return codec_ids;
}

#ifdef LADYBIRD_SYSTEM_FFMPEG_NAMESPACE

FFmpegFunctions const* FFmpegFunctions::resolved_from_library(void* library_handle)
{
    auto* functions = new (nothrow) FFmpegFunctions;
    if (functions == nullptr)
        return nullptr;
#    define RESOLVE_FUNCTION(name)                                                                   \
        functions->name = reinterpret_cast<decltype(functions->name)>(dlsym(library_handle, #name)); \
        if (functions->name == nullptr) {                                                            \
            delete functions;                                                                        \
            return nullptr;                                                                          \
        }
    FFMPEG_ENUMERATE_FUNCTIONS(RESOLVE_FUNCTION)
#    undef RESOLVE_FUNCTION
    functions->codec_ids = codec_ids_of_these_headers();
    return functions;
}

#else

FFmpegFunctions const& FFmpegFunctions::bundled()
{
    static constexpr FFmpegFunctions functions {
#    define FFMPEG_LINKED_FUNCTION(name) .name = &::name,
        FFMPEG_ENUMERATE_FUNCTIONS(FFMPEG_LINKED_FUNCTION)
#    undef FFMPEG_LINKED_FUNCTION
            .codec_ids = codec_ids_of_these_headers(),
    };
    return functions;
}

#endif

}
