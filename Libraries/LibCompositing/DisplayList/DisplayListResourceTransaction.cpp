/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Compositing {

static ErrorOr<Gfx::ShareableBitmap> shareable_bitmap_from_image_frame(Gfx::DecodedImageFrame const& frame)
{
    auto bitmap = frame.bitmap().to_shareable_bitmap();
    if (!bitmap.is_valid())
        return Error::from_string_literal("Display-list resource transaction failed to create image-frame bitmap");
    return bitmap;
}

static ErrorOr<Gfx::DecodedImageFrame> create_image_frame_from_ipc(Gfx::ShareableBitmap bitmap, Gfx::ColorSpace color_space)
{
    if (!bitmap.is_valid() || !bitmap.bitmap())
        return Error::from_string_literal("Display-list resource transaction contained invalid image-frame bitmap");
    return Gfx::DecodedImageFrame { *bitmap.bitmap(), move(color_space) };
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DisplayListFontResource const& resource)
{
    auto& font = *resource.font;
    TRY(encoder.encode(resource.id));
    TRY(encoder.encode(font.typeface()));
    TRY(encoder.encode(font.point_size()));
    TRY(encoder.encode(font.variation_settings()));
    TRY(encoder.encode(font.features()));
    TRY(encoder.encode(font.is_invisible()));
    return {};
}

template<>
ErrorOr<Compositing::DisplayListFontResource> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Compositing::FontResourceId>());
    auto typeface = TRY(decoder.decode<NonnullRefPtr<Gfx::Typeface const>>());
    auto point_size = TRY(decoder.decode<float>());
    auto variations = TRY(decoder.decode<Gfx::FontVariationSettings>());
    auto features = TRY(decoder.decode<Gfx::ShapeFeatures>());
    auto invisible = TRY(decoder.decode<bool>());
    auto font = typeface->font(point_size, move(variations), move(features));
    if (invisible)
        font = font->invisible_variant();

    return Compositing::DisplayListFontResource {
        .id = id,
        .font = move(font),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DisplayListImageFrameResource const& resource)
{
    TRY(encoder.encode(resource.id));
    TRY(encoder.encode(TRY(Compositing::shareable_bitmap_from_image_frame(resource.frame))));
    TRY(encoder.encode(resource.frame.color_space()));
    return {};
}

template<>
ErrorOr<Compositing::DisplayListImageFrameResource> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Compositing::ImageFrameResourceId>());
    auto bitmap = TRY(decoder.decode<Gfx::ShareableBitmap>());
    auto color_space = TRY(decoder.decode<Gfx::ColorSpace>());

    return Compositing::DisplayListImageFrameResource {
        .id = id,
        .frame = TRY(Compositing::create_image_frame_from_ipc(move(bitmap), move(color_space))),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DisplayListVideoSinkResource const& resource)
{
    TRY(encoder.encode(resource.id));
    TRY(encoder.encode(resource.sink_handle));
    return {};
}

template<>
ErrorOr<Compositing::DisplayListVideoSinkResource> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Compositing::VideoSinkResourceId>());
    auto sink_handle = TRY(decoder.decode<Media::VideoSinkHandle>());
    return Compositing::DisplayListVideoSinkResource { .id = id, .sink_handle = sink_handle };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DisplayListResourceTransaction const& transaction)
{
    TRY(encoder.encode(transaction.fonts));
    TRY(encoder.encode(transaction.image_frames));
    TRY(encoder.encode(transaction.video_sinks));
    TRY(encoder.encode_size(transaction.display_lists.size()));
    for (auto const& display_list : transaction.display_lists) {
        TRY(encoder.encode(*display_list.display_list));
        TRY(encoder.encode(display_list.visual_context_tree));
    }
    TRY(encoder.encode(transaction.font_ids_to_remove));
    TRY(encoder.encode(transaction.image_frame_ids_to_remove));
    TRY(encoder.encode(transaction.video_sink_ids_to_remove));
    TRY(encoder.encode(transaction.display_list_ids_to_remove));
    return {};
}

template<>
ErrorOr<Compositing::DisplayListResourceTransaction> decode(Decoder& decoder)
{
    auto fonts = TRY(decoder.decode<Vector<Compositing::DisplayListFontResource>>());
    auto image_frames = TRY(decoder.decode<Vector<Compositing::DisplayListImageFrameResource>>());
    auto video_sinks = TRY(decoder.decode<Vector<Compositing::DisplayListVideoSinkResource>>());

    auto display_list_count = TRY(decoder.decode_size());
    Vector<Compositing::DisplayListResource> display_lists;
    TRY(display_lists.try_ensure_capacity(display_list_count));
    for (size_t i = 0; i < display_list_count; ++i) {
        auto display_list = TRY(decoder.decode<NonnullRefPtr<Compositing::DisplayList>>());
        auto visual_context_tree = TRY(decoder.decode<Compositing::AccumulatedVisualContextTree>());
        display_lists.unchecked_append({ move(display_list), move(visual_context_tree) });
    }

    return Compositing::DisplayListResourceTransaction {
        .fonts = move(fonts),
        .image_frames = move(image_frames),
        .video_sinks = move(video_sinks),
        .display_lists = move(display_lists),
        .font_ids_to_remove = TRY(decoder.decode<Vector<Compositing::FontResourceId>>()),
        .image_frame_ids_to_remove = TRY(decoder.decode<Vector<Compositing::ImageFrameResourceId>>()),
        .video_sink_ids_to_remove = TRY(decoder.decode<Vector<Compositing::VideoSinkResourceId>>()),
        .display_list_ids_to_remove = TRY(decoder.decode<Vector<Compositing::DisplayListResourceId>>()),
    };
}

}
