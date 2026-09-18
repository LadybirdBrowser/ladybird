/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageFormats/ImageDecoder.h>

static u64 fingerprint(Gfx::Bitmap const& bitmap)
{
    u64 hash = 0;
    // Touch bounded pixel data, including the final row/column and row strides.
    auto x_step = max(1, bitmap.width() / 8);
    auto y_step = max(1, bitmap.height() / 8);
    for (int y = 0; y < bitmap.height(); y += y_step) {
        for (int x = 0; x < bitmap.width(); x += x_step)
            hash = hash * 131 + bitmap.get_pixel(x, y).value();
    }
    if (bitmap.width() > 0 && bitmap.height() > 0)
        hash = hash * 131 + bitmap.get_pixel(bitmap.width() - 1, bitmap.height() - 1).value();
    return hash;
}

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    if (size > 1024 * 1024)
        return 0;
    auto result = Gfx::ImageDecoder::try_create_for_raw_bytes({ data, size });
    if (result.is_error() || !result.value())
        return 0;
    auto decoder = result.release_value();
    if (decoder->width() <= 0 || decoder->height() <= 0 || decoder->width() > 512 || decoder->height() > 512)
        return 0;
    (void)decoder->is_animated();
    (void)decoder->loop_count();
    (void)decoder->first_animated_frame_index();
    (void)decoder->icc_data();
    (void)decoder->color_space();
    if (auto metadata = decoder->metadata(); metadata.has_value())
        (void)metadata->main_tags();
    if (decoder->natural_frame_format() == Gfx::NaturalFrameFormat::CMYK)
        (void)decoder->cmyk_frame();
    auto frame_count = min(decoder->frame_count(), size_t { 16 });
    Optional<u64> first_hash;
    Optional<Gfx::IntSize> first_size;
    for (size_t i = 0; i < frame_count; ++i) {
        (void)decoder->frame_duration(i);
        if (decoder->natural_frame_format() == Gfx::NaturalFrameFormat::Vector) {
            (void)decoder->vector_frame(i);
            continue;
        }
        auto frame = decoder->frame(i);
        if (frame.is_error())
            break;
        auto const& bitmap = *frame.value().image;
        // Animated subframes need not share the dimensions reported by metadata.
        if (bitmap.width() <= 0 || bitmap.height() <= 0 || bitmap.width() > 512 || bitmap.height() > 512)
            break;
        if (i == 0) {
            first_hash = fingerprint(*frame.value().image);
            first_size = frame.value().image->size();
        } else {
            (void)fingerprint(*frame.value().image);
        }
    }
    // A decoder may reject rewind. Successful rewind must reproduce frame zero.
    if (first_hash.has_value()) {
        auto again = decoder->frame(0);
        if (!again.is_error()) {
            VERIFY(again.value().image->size() == first_size.value());
            VERIFY(fingerprint(*again.value().image) == first_hash.value());
        }
    }
    return 0;
}
