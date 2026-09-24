/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Debug.h>
#include <AK/NeverDestroyed.h>
#include <AK/OwnPtr.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Environment.h>
#include <LibMedia/FFmpeg/FFmpegFunctions.h>
#include <LibMedia/FFmpeg/FFmpegHelpers.h>
#include <LibMedia/FFmpeg/SystemFFmpeg.h>
#include <dlfcn.h>

namespace Media::FFmpeg {

// Confirms the library lays out what the decoders touch directly the way the bundled headers say.
static DecoderErrorOr<void> verify_library_layout(FFmpegFunctions const& functions)
{
    auto* frame = functions.av_frame_alloc();
    ScopeGuard free_frame = [&] { functions.av_frame_free(&frame); };
    if (frame == nullptr || frame->pts != AV_NOPTS_VALUE || frame->format != -1)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "AVFrame is not laid out as expected"sv);

    auto* packet = functions.av_packet_alloc();
    ScopeGuard free_packet = [&] { functions.av_packet_free(&packet); };
    if (packet == nullptr || packet->pts != AV_NOPTS_VALUE || packet->dts != AV_NOPTS_VALUE || packet->size != 0)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "AVPacket is not laid out as expected"sv);
    constexpr int probe_size = 1234;
    if (functions.av_new_packet(packet, probe_size) < 0 || packet->size != probe_size || packet->data == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "AVPacket is not laid out as expected"sv);

    auto* codec_context = functions.avcodec_alloc_context3(nullptr);
    ScopeGuard free_codec_context = [&] { functions.avcodec_free_context(&codec_context); };
    if (codec_context == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate an FFmpeg codec context"sv);
    for (auto const* option : { "threads", "ar", "colorspace", "color_primaries", "color_trc", "color_range" }) {
        i64 value = 0;
        if (functions.av_opt_get_int(codec_context, option, 0, &value) != 0)
            return DecoderError::format(DecoderErrorCategory::NotImplemented, "AVCodecContext has no {} option", option);
    }
    AVChannelLayout channel_layout;
    if (functions.av_opt_get_chlayout(codec_context, "ch_layout", 0, &channel_layout) != 0)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "AVCodecContext has no ch_layout option"sv);
    functions.av_channel_layout_uninit(&channel_layout);
    return {};
}

DecoderErrorOr<NonnullOwnPtr<SystemFFmpeg>> SystemFFmpeg::try_load(ByteString const& library_path)
{
    auto* library_handle = dlopen(library_path.characters(), RTLD_NOW | RTLD_LOCAL);
    if (library_handle == nullptr)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not load {}: {}", library_path, dlerror());
    ArmedScopeGuard close_library = [&] { dlclose(library_handle); };

    auto functions = DECODER_TRY_ALLOC(try_make<FFmpegFunctions>());
#define RESOLVE_FUNCTION(name)                                                           \
    functions->name = reinterpret_cast<decltype(&::name)>(dlsym(library_handle, #name)); \
    if (functions->name == nullptr)                                                      \
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "{} does not provide {}", library_path, #name##sv);
    FFMPEG_ENUMERATE_FUNCTIONS(RESOLVE_FUNCTION)
#undef RESOLVE_FUNCTION

    auto major = functions->avcodec_version() >> 16;
    if (major < LOWEST_SUPPORTED_LIBAVCODEC_MAJOR || major > HIGHEST_VERIFIED_LIBAVCODEC_MAJOR)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "libavcodec major {} provided by {} is outside the supported range", major, library_path);
    TRY(verify_library_layout(*functions));

    close_library.disarm();
    return DECODER_TRY_ALLOC(try_make<SystemFFmpeg>(library_handle, move(functions), major));
}

static OwnPtr<SystemFFmpeg> load_preferred_library()
{
    if (auto library_path = Core::Environment::get("LADYBIRD_SYSTEM_FFMPEG_LIBAVCODEC"sv); library_path.has_value()) {
        auto library = SystemFFmpeg::try_load(*library_path);
        if (library.is_error()) {
            dbgln("SystemFFmpeg: {}", library.error().description());
            return {};
        }
        return library.release_value();
    }

    for (auto major = HIGHEST_VERIFIED_LIBAVCODEC_MAJOR; major >= LOWEST_SUPPORTED_LIBAVCODEC_MAJOR; major--) {
        auto library = SystemFFmpeg::try_load(ByteString::formatted("libavcodec.so.{}", major));
        if (library.is_error()) {
            dbgln_if(SYSTEM_FFMPEG_DEBUG, "SystemFFmpeg: {}", library.error().description());
            continue;
        }
        if (!library.value()->provides_decoder_missing_from_bundle()) {
            dbgln_if(SYSTEM_FFMPEG_DEBUG, "SystemFFmpeg: libavcodec.so.{} decodes nothing the bundled FFmpeg cannot", major);
            continue;
        }
        dbgln_if(SYSTEM_FFMPEG_DEBUG, "SystemFFmpeg: using libavcodec.so.{}", major);
        return library.release_value();
    }
    return {};
}

SystemFFmpeg const* SystemFFmpeg::the()
{
    static NeverDestroyed<OwnPtr<SystemFFmpeg> const> library { load_preferred_library() };
    return library->ptr();
}

SystemFFmpeg::SystemFFmpeg(void* library_handle, NonnullOwnPtr<FFmpegFunctions> functions, unsigned major)
    : m_library_handle(library_handle)
    , m_functions(move(functions))
    , m_major(major)
{
}

SystemFFmpeg::~SystemFFmpeg()
{
    dlclose(m_library_handle);
}

bool SystemFFmpeg::has_decoder(CodecID codec_id) const
{
    return m_functions->avcodec_find_decoder(ffmpeg_codec_id_from_media_codec_id(codec_id)) != nullptr;
}

bool SystemFFmpeg::provides_decoder_missing_from_bundle() const
{
    auto const& bundled = FFmpegFunctions::bundled();
    return any_of(all_codec_ids, [&](CodecID codec_id) {
        auto ffmpeg_codec_id = ffmpeg_codec_id_from_media_codec_id(codec_id);
        return bundled.avcodec_find_decoder(ffmpeg_codec_id) == nullptr && m_functions->avcodec_find_decoder(ffmpeg_codec_id) != nullptr;
    });
}

}
