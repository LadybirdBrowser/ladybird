/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibCompositing/DisplayList/DisplayListResources.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/Typeface.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::DisplayListFontDescription const& description)
{
    TRY(encoder.encode(description.id));
    TRY(encoder.encode(description.typeface_id));
    TRY(encoder.encode(description.point_size));
    TRY(encoder.encode(description.variation_settings));
    TRY(encoder.encode(description.synthesis.bold));
    TRY(encoder.encode(description.synthesis.oblique_angle));
    TRY(encoder.encode(description.is_invisible));
    return {};
}

template<>
ErrorOr<Compositing::DisplayListFontDescription> decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Compositing::FontResourceId>());
    auto typeface_id = TRY(decoder.decode<u64>());
    auto point_size = TRY(decoder.decode<float>());
    auto variation_settings = TRY(decoder.decode<Gfx::FontVariationSettings>());
    Gfx::FontSynthesis synthesis;
    synthesis.bold = TRY(decoder.decode<bool>());
    synthesis.oblique_angle = TRY(decoder.decode<float>());
    if (!__builtin_isfinite(synthesis.oblique_angle) || AK::fabs(synthesis.oblique_angle) > 90)
        return Error::from_string_literal("Invalid synthetic oblique angle");
    auto is_invisible = TRY(decoder.decode<bool>());

    return Compositing::DisplayListFontDescription {
        .id = id,
        .typeface_id = typeface_id,
        .point_size = point_size,
        .variation_settings = move(variation_settings),
        .synthesis = synthesis,
        .is_invisible = is_invisible,
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::TypefaceRegistration const& registration)
{
    TRY(encoder.encode(registration.typeface_id));
    TRY(encoder.encode(*registration.typeface));
    return {};
}

template<>
ErrorOr<Compositing::TypefaceRegistration> decode(Decoder& decoder)
{
    auto typeface_id = TRY(decoder.decode<u64>());
    auto typeface = TRY(decoder.decode<NonnullRefPtr<Gfx::Typeface const>>());

    return Compositing::TypefaceRegistration {
        .typeface_id = typeface_id,
        .typeface = move(typeface),
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

}
