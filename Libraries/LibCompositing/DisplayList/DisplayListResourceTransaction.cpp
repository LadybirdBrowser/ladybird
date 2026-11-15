/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/Typeface.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

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
    TRY(encoder.encode(font.synthesis().bold));
    TRY(encoder.encode(font.synthesis().oblique_angle));
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
    Gfx::FontSynthesis synthesis;
    synthesis.bold = TRY(decoder.decode<bool>());
    synthesis.oblique_angle = TRY(decoder.decode<float>());
    if (!__builtin_isfinite(synthesis.oblique_angle) || AK::fabs(synthesis.oblique_angle) > 90)
        return Error::from_string_literal("Invalid synthetic oblique angle");
    auto invisible = TRY(decoder.decode<bool>());
    auto font = invisible ? typeface->invisible_font(point_size, variations, features, synthesis) : typeface->font(point_size, variations, features, synthesis);

    return Compositing::DisplayListFontResource {
        .id = id,
        .font = move(font),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DisplayListImageFrameResource const& resource)
{
    TRY(encoder.encode(resource.id));
    TRY(encoder.encode(resource.frame));
    return {};
}

template<>
ErrorOr<Compositing::DisplayListImageFrameResource> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Compositing::ImageFrameResourceId>());
    auto frame = TRY(decoder.decode<Gfx::DecodedImageFrame>());

    return Compositing::DisplayListImageFrameResource {
        .id = id,
        .frame = move(frame),
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
    // Only Rust sends nested display lists, which only the compositor's Rust receives.
    TRY(encoder.encode_size(0));
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

    // Only the compositor's Rust receives nested display lists, with the visual context trees they were recorded
    // against.
    if (TRY(decoder.decode_size()) != 0)
        return Error::from_string_literal("Display list resources only go to the compositor's Rust");

    return Compositing::DisplayListResourceTransaction {
        .fonts = move(fonts),
        .image_frames = move(image_frames),
        .video_sinks = move(video_sinks),
        .font_ids_to_remove = TRY(decoder.decode<Vector<Compositing::FontResourceId>>()),
        .image_frame_ids_to_remove = TRY(decoder.decode<Vector<Compositing::ImageFrameResourceId>>()),
        .video_sink_ids_to_remove = TRY(decoder.decode<Vector<Compositing::VideoSinkResourceId>>()),
        .display_list_ids_to_remove = TRY(decoder.decode<Vector<Compositing::DisplayListResourceId>>()),
    };
}

}
