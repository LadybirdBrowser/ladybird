/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>

namespace Web::Painting {

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
ErrorOr<void> encode(Encoder& encoder, Web::Painting::DisplayListFontResource const& resource)
{
    auto& font = *resource.font;
    TRY(encoder.encode(resource.id));
    TRY(encoder.encode(font.typeface()));
    TRY(encoder.encode(font.point_size()));
    TRY(encoder.encode(font.variation_settings()));
    TRY(encoder.encode(font.features()));
    return {};
}

template<>
ErrorOr<Web::Painting::DisplayListFontResource> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Web::Painting::FontResourceId>());
    auto typeface = TRY(decoder.decode<NonnullRefPtr<Gfx::Typeface const>>());
    auto point_size = TRY(decoder.decode<float>());
    auto variations = TRY(decoder.decode<Gfx::FontVariationSettings>());
    auto features = TRY(decoder.decode<Gfx::ShapeFeatures>());

    return Web::Painting::DisplayListFontResource {
        .id = id,
        .font = typeface->font(point_size, move(variations), move(features)),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::DisplayListImageFrameResource const& resource)
{
    TRY(encoder.encode(resource.id));
    TRY(encoder.encode(TRY(Web::Painting::shareable_bitmap_from_image_frame(resource.frame))));
    TRY(encoder.encode(resource.frame.color_space()));
    return {};
}

template<>
ErrorOr<Web::Painting::DisplayListImageFrameResource> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Web::Painting::ImageFrameResourceId>());
    auto bitmap = TRY(decoder.decode<Gfx::ShareableBitmap>());
    auto color_space = TRY(decoder.decode<Gfx::ColorSpace>());

    return Web::Painting::DisplayListImageFrameResource {
        .id = id,
        .frame = TRY(Web::Painting::create_image_frame_from_ipc(move(bitmap), move(color_space))),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::DisplayListVideoFrameResource const& resource)
{
    Optional<NonnullRefPtr<Media::VideoFrame const>> frame;
    if (resource.frame)
        frame = NonnullRefPtr<Media::VideoFrame const> { *resource.frame };

    TRY(encoder.encode(resource.id));
    TRY(encoder.encode(frame));
    return {};
}

template<>
ErrorOr<Web::Painting::DisplayListVideoFrameResource> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Web::Painting::VideoFrameResourceId>());
    auto frame = TRY(decoder.decode<Optional<NonnullRefPtr<Media::VideoFrame const>>>());

    RefPtr<Media::VideoFrame const> decoded_frame;
    if (frame.has_value())
        decoded_frame = frame.release_value();

    return Web::Painting::DisplayListVideoFrameResource {
        .id = id,
        .frame = move(decoded_frame),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::DisplayListResourceTransaction const& transaction)
{
    TRY(encoder.encode(transaction.fonts));
    TRY(encoder.encode(transaction.image_frames));
    TRY(encoder.encode(transaction.video_frames));
    TRY(encoder.encode_size(transaction.display_lists.size()));
    for (auto const& display_list : transaction.display_lists) {
        TRY(encoder.encode(*display_list.display_list));
        TRY(encoder.encode(display_list.visual_context_tree));
    }
    TRY(encoder.encode(transaction.font_ids_to_remove));
    TRY(encoder.encode(transaction.image_frame_ids_to_remove));
    TRY(encoder.encode(transaction.video_frame_ids_to_remove));
    TRY(encoder.encode(transaction.display_list_ids_to_remove));
    return {};
}

template<>
ErrorOr<Web::Painting::DisplayListResourceTransaction> decode(Decoder& decoder)
{
    auto fonts = TRY(decoder.decode<Vector<Web::Painting::DisplayListFontResource>>());
    auto image_frames = TRY(decoder.decode<Vector<Web::Painting::DisplayListImageFrameResource>>());
    auto video_frames = TRY(decoder.decode<Vector<Web::Painting::DisplayListVideoFrameResource>>());

    auto display_list_count = TRY(decoder.decode_size());
    Vector<Web::Painting::DisplayListResource> display_lists;
    TRY(display_lists.try_ensure_capacity(display_list_count));
    for (size_t i = 0; i < display_list_count; ++i) {
        auto display_list = TRY(decoder.decode<NonnullRefPtr<Web::Painting::DisplayList>>());
        auto visual_context_tree = TRY(decoder.decode<Web::Painting::AccumulatedVisualContextTree>());
        display_lists.unchecked_append({ move(display_list), move(visual_context_tree) });
    }

    return Web::Painting::DisplayListResourceTransaction {
        .fonts = move(fonts),
        .image_frames = move(image_frames),
        .video_frames = move(video_frames),
        .display_lists = move(display_lists),
        .font_ids_to_remove = TRY(decoder.decode<Vector<Web::Painting::FontResourceId>>()),
        .image_frame_ids_to_remove = TRY(decoder.decode<Vector<Web::Painting::ImageFrameResourceId>>()),
        .video_frame_ids_to_remove = TRY(decoder.decode<Vector<Web::Painting::VideoFrameResourceId>>()),
        .display_list_ids_to_remove = TRY(decoder.decode<Vector<Web::Painting::DisplayListResourceId>>()),
    };
}

}
