/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaCodecs/FFmpeg/FFmpegFunctions.h>
#include <LibMediaCodecs/FFmpeg/FFmpegHelpers.h>

#ifndef LADYBIRD_SYSTEM_FFMPEG_NAMESPACE
#    include <AK/OwnPtr.h>
#    include <LibMediaCodecs/FFmpeg/BundledFFmpeg.h>
#endif

#if defined(LADYBIRD_SYSTEM_FFMPEG_NAMESPACE) || defined(LADYBIRD_BUNDLED_LIBAVCODEC)
#    include <dlfcn.h>
#endif

namespace FFMPEG_ABI_NAMESPACE {

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

static FFmpegFunctions const* s_bundled_functions = nullptr;

#    ifdef LADYBIRD_BUNDLED_LIBAVCODEC

// musl binds each symbol to the first definition of it in the global scope, whichever version of the library the
// caller was linked against. So that a system libavcodec loaded later still binds to its own libavutil, the bundled
// FFmpeg is loaded with RTLD_LOCAL rather than linked, which keeps it out of that scope.
static ErrorOr<FFmpegFunctions const*> load_bundled_ffmpeg_libraries()
{
    auto open_library = [](char const* name) -> ErrorOr<void*> {
        auto* library_handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (library_handle == nullptr) {
            warnln("Could not load the bundled {}: {}", name, dlerror());
            return Error::from_string_literal("Could not load the bundled FFmpeg");
        }
        return library_handle;
    };
    auto* libavcodec = TRY(open_library(LADYBIRD_BUNDLED_LIBAVCODEC));
    auto* libswresample = TRY(open_library(LADYBIRD_BUNDLED_LIBSWRESAMPLE));

    auto functions = adopt_own_if_nonnull(new (nothrow) FFmpegFunctions);
    if (!functions)
        return Error::from_errno(ENOMEM);
#        define RESOLVE_FUNCTION(library_handle, name)                                                   \
            functions->name = reinterpret_cast<decltype(functions->name)>(dlsym(library_handle, #name)); \
            if (functions->name == nullptr) {                                                            \
                warnln("The bundled FFmpeg has no function {}", #name);                                  \
                return Error::from_string_literal("The bundled FFmpeg lacks a function");                \
            }
#        define RESOLVE_FROM_LIBAVCODEC(name) RESOLVE_FUNCTION(libavcodec, name)
#        define RESOLVE_FROM_LIBSWRESAMPLE(name) RESOLVE_FUNCTION(libswresample, name)
    FFMPEG_ENUMERATE_FUNCTIONS(RESOLVE_FROM_LIBAVCODEC)
    FFMPEG_ENUMERATE_BUNDLED_FUNCTIONS(RESOLVE_FROM_LIBSWRESAMPLE)
#        undef RESOLVE_FROM_LIBSWRESAMPLE
#        undef RESOLVE_FROM_LIBAVCODEC
#        undef RESOLVE_FUNCTION
    functions->codec_ids = codec_ids_of_these_headers();
    return functions.leak_ptr();
}

#    else

static constexpr FFmpegFunctions linked_functions {
#        define FFMPEG_LINKED_FUNCTION(name) .name = &::name,
    FFMPEG_ENUMERATE_FUNCTIONS(FFMPEG_LINKED_FUNCTION)
        FFMPEG_ENUMERATE_BUNDLED_FUNCTIONS(FFMPEG_LINKED_FUNCTION)
#        undef FFMPEG_LINKED_FUNCTION
            .codec_ids = codec_ids_of_these_headers(),
};

#    endif

ErrorOr<void> load_bundled_ffmpeg()
{
    if (s_bundled_functions != nullptr)
        return {};
#    ifdef LADYBIRD_BUNDLED_LIBAVCODEC
    s_bundled_functions = TRY(load_bundled_ffmpeg_libraries());
#    else
    s_bundled_functions = &linked_functions;
#    endif
    return {};
}

FFmpegFunctions const& FFmpegFunctions::bundled()
{
    VERIFY(s_bundled_functions != nullptr);
    return *s_bundled_functions;
}

#endif

}
