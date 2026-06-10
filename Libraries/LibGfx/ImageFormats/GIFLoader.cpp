/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Memory.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGfx/ImageFormats/GIFLoader.h>
#include <LibGfx/Painter.h>

#define WUFFS_IMPLEMENTATION

#define WUFFS_CONFIG__STATIC_FUNCTIONS
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE__CORE
#define WUFFS_CONFIG__MODULE__BASE__INTERFACES
#define WUFFS_CONFIG__MODULE__BASE__PIXCONV
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__LZW

#include <wuffs/wuffs-v0.3.c>

namespace Gfx {

static Error error_from_wuffs_status(wuffs_base__status const& status)
{
    auto const* message = wuffs_base__status__message(&status);
    return Error::from_string_view(StringView { message, strlen(message) });
}

struct GIFLoadingContext {
    explicit GIFLoadingContext(ReadonlyBytes data)
        : data(data)
    {
    }

    ReadonlyBytes data;
    IntSize size;

    // Frame metadata, gathered without decoding pixel data.
    Optional<bool> metadata_scan_succeeded;
    Vector<int> frame_durations;
    u32 loop_count { 0 };

    OwnPtr<wuffs_gif__decoder> decoder;
    wuffs_base__io_buffer io_buffer {};
    wuffs_base__pixel_buffer pixel_buffer {};
    RefPtr<Bitmap> canvas;
    RefPtr<Bitmap> saved_canvas;
    ByteBuffer work_buffer;
    size_t frames_decoded { 0 };
    wuffs_base__animation_disposal current_frame_disposal { WUFFS_BASE__ANIMATION_DISPOSAL__NONE };
    IntRect current_frame_rect;
};

static ErrorOr<NonnullOwnPtr<wuffs_gif__decoder>> create_decoder(ReadonlyBytes data, wuffs_base__io_buffer& io_buffer, wuffs_base__image_config& out_image_config)
{
    auto decoder = TRY(adopt_nonnull_own_or_enomem(new (nothrow) wuffs_gif__decoder()));

    auto status = wuffs_gif__decoder__initialize(decoder.ptr(), sizeof__wuffs_gif__decoder(), WUFFS_VERSION, WUFFS_INITIALIZE__ALREADY_ZEROED);
    if (!wuffs_base__status__is_ok(&status))
        return error_from_wuffs_status(status);

    wuffs_gif__decoder__set_quirk_enabled(decoder.ptr(), WUFFS_GIF__QUIRK_IGNORE_TOO_MUCH_PIXEL_DATA, true);

    io_buffer = wuffs_base__ptr_u8__reader(const_cast<u8*>(data.data()), data.size(), true);

    status = wuffs_gif__decoder__decode_image_config(decoder.ptr(), &out_image_config, &io_buffer);
    if (!wuffs_base__status__is_ok(&status))
        return error_from_wuffs_status(status);

    return decoder;
}

static ErrorOr<void> scan_frame_metadata(GIFLoadingContext& context)
{
    wuffs_base__io_buffer io_buffer {};
    auto image_config = wuffs_base__null_image_config();
    auto decoder = TRY(create_decoder(context.data, io_buffer, image_config));

    for (;;) {
        auto frame_config = wuffs_base__null_frame_config();
        auto status = wuffs_gif__decoder__decode_frame_config(decoder.ptr(), &frame_config, &io_buffer);
        if (status.repr == wuffs_base__note__end_of_data)
            break;

        // Tolerate corrupted or truncated data after at least one frame has been seen.
        if (!wuffs_base__status__is_ok(&status)) {
            if (context.frame_durations.is_empty())
                return error_from_wuffs_status(status);
            break;
        }

        int duration_ms = static_cast<int>(wuffs_base__frame_config__duration(&frame_config) / WUFFS_BASE__FLICKS_PER_MILLISECOND);
        if (duration_ms <= 10)
            duration_ms = 100;
        TRY(context.frame_durations.try_append(duration_ms));
    }

    if (context.frame_durations.is_empty())
        return Error::from_string_literal("GIFImageDecoderPlugin: No frames could be decoded");

    context.loop_count = wuffs_gif__decoder__num_animation_loops(decoder.ptr());

    return {};
}

static ErrorOr<void> ensure_frame_metadata(GIFLoadingContext& context)
{
    if (!context.metadata_scan_succeeded.has_value())
        context.metadata_scan_succeeded = !scan_frame_metadata(context).is_error();

    if (!*context.metadata_scan_succeeded)
        return Error::from_string_literal("GIFImageDecoderPlugin: Decoding failed");

    return {};
}

static ErrorOr<void> restart_pixel_decoding(GIFLoadingContext& context)
{
    auto image_config = wuffs_base__null_image_config();
    context.decoder = TRY(create_decoder(context.data, context.io_buffer, image_config));

    if (context.canvas) {
        memset(context.canvas->scanline_u8(0), 0, context.canvas->size_in_bytes());
    } else {
        context.canvas = TRY(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Unpremultiplied, context.size));

        auto pixel_config = wuffs_base__null_pixel_config();
        wuffs_base__pixel_config__set(&pixel_config, WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, context.canvas->width(), context.canvas->height());

        auto status = wuffs_base__pixel_buffer__set_from_slice(&context.pixel_buffer, &pixel_config, wuffs_base__make_slice_u8(context.canvas->scanline_u8(0), context.canvas->size_in_bytes()));
        if (!wuffs_base__status__is_ok(&status))
            return error_from_wuffs_status(status);

        auto work_buffer_length = wuffs_gif__decoder__workbuf_len(context.decoder.ptr()).max_incl;
        context.work_buffer = TRY(ByteBuffer::create_uninitialized(work_buffer_length));
    }

    context.frames_decoded = 0;
    context.current_frame_disposal = WUFFS_BASE__ANIMATION_DISPOSAL__NONE;

    return {};
}

static ErrorOr<void> decode_next_frame(GIFLoadingContext& context)
{
    if (context.frames_decoded > 0) {
        switch (context.current_frame_disposal) {
        case WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND:
            Painter::create(*context.canvas)->clear_rect(context.current_frame_rect.to_type<float>(), Color::Transparent);
            break;
        case WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS:
            memcpy(context.canvas->scanline_u8(0), context.saved_canvas->scanline_u8(0), context.canvas->size_in_bytes());
            break;
        default:
            break;
        }
    }

    auto frame_config = wuffs_base__null_frame_config();
    auto status = wuffs_gif__decoder__decode_frame_config(context.decoder.ptr(), &frame_config, &context.io_buffer);
    if (!wuffs_base__status__is_ok(&status))
        return error_from_wuffs_status(status);

    auto disposal = wuffs_base__frame_config__disposal(&frame_config);
    if (disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS && context.current_frame_disposal != WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) {
        if (context.saved_canvas) {
            memcpy(context.saved_canvas->scanline_u8(0), context.canvas->scanline_u8(0), context.canvas->size_in_bytes());
        } else {
            context.saved_canvas = TRY(context.canvas->clone());
        }
    }

    auto blend = wuffs_base__frame_config__overwrite_instead_of_blend(&frame_config)
        ? WUFFS_BASE__PIXEL_BLEND__SRC
        : WUFFS_BASE__PIXEL_BLEND__SRC_OVER;
    status = wuffs_gif__decoder__decode_frame(context.decoder.ptr(), &context.pixel_buffer, &context.io_buffer, blend, wuffs_base__make_slice_u8(context.work_buffer.data(), context.work_buffer.size()), nullptr);

    if (!wuffs_base__status__is_ok(&status) && context.frames_decoded > 0)
        return error_from_wuffs_status(status);

    ++context.frames_decoded;

    auto frame_bounds = wuffs_base__frame_config__bounds(&frame_config);
    context.current_frame_rect = {
        static_cast<int>(frame_bounds.min_incl_x),
        static_cast<int>(frame_bounds.min_incl_y),
        static_cast<int>(wuffs_base__rect_ie_u32__width(&frame_bounds)),
        static_cast<int>(wuffs_base__rect_ie_u32__height(&frame_bounds)),
    };
    context.current_frame_disposal = disposal;

    return {};
}

GIFImageDecoderPlugin::GIFImageDecoderPlugin(ReadonlyBytes data)
    : m_context(make<GIFLoadingContext>(data))
{
}

GIFImageDecoderPlugin::~GIFImageDecoderPlugin() = default;

IntSize GIFImageDecoderPlugin::size()
{
    return m_context->size;
}

bool GIFImageDecoderPlugin::sniff(ReadonlyBytes data)
{
    return data.starts_with("GIF87a"sv.bytes()) || data.starts_with("GIF89a"sv.bytes());
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> GIFImageDecoderPlugin::create(ReadonlyBytes data)
{
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) GIFImageDecoderPlugin(data)));
    auto& context = *plugin->m_context;

    wuffs_base__io_buffer io_buffer {};
    auto image_config = wuffs_base__null_image_config();
    auto decoder = TRY(create_decoder(context.data, io_buffer, image_config));

    context.size = {
        static_cast<int>(wuffs_base__pixel_config__width(&image_config.pixcfg)),
        static_cast<int>(wuffs_base__pixel_config__height(&image_config.pixcfg)),
    };

    return plugin;
}

bool GIFImageDecoderPlugin::is_animated()
{
    if (ensure_frame_metadata(*m_context).is_error())
        return false;

    return m_context->frame_durations.size() > 1;
}

size_t GIFImageDecoderPlugin::loop_count()
{
    if (ensure_frame_metadata(*m_context).is_error())
        return 0;

    return m_context->loop_count;
}

size_t GIFImageDecoderPlugin::frame_count()
{
    if (ensure_frame_metadata(*m_context).is_error())
        return 1;

    return m_context->frame_durations.size();
}

size_t GIFImageDecoderPlugin::first_animated_frame_index()
{
    return 0;
}

int GIFImageDecoderPlugin::frame_duration(size_t index)
{
    if (ensure_frame_metadata(*m_context).is_error() || index >= m_context->frame_durations.size())
        return 0;

    return m_context->frame_durations[index];
}

ErrorOr<ImageFrameDescriptor> GIFImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    auto& context = *m_context;
    TRY(ensure_frame_metadata(context));

    if (index >= context.frame_durations.size())
        return Error::from_string_literal("GIFImageDecoderPlugin: Invalid frame index");

    if (!context.canvas || index + 1 < context.frames_decoded)
        TRY(restart_pixel_decoding(context));

    while (context.frames_decoded <= index)
        TRY(decode_next_frame(context));

    return ImageFrameDescriptor { TRY(context.canvas->clone()), context.frame_durations[index] };
}

}
