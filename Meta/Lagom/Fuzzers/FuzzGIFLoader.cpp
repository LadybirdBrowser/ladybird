/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Format.h>
#include <LibGfx/ImageFormats/GIFLoader.h>
#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    if constexpr (!GIF_DEBUG) {
        AK::set_debug_enabled(false);
    }
    auto decoder_or_error = Gfx::GIFImageDecoderPlugin::create({ data, size });
    if (decoder_or_error.is_error())
        return 0;
    auto decoder = decoder_or_error.release_value();
    auto& gif_decoder = *decoder;
    auto bitmap_or_error = decoder->frame(0);
    if (!bitmap_or_error.is_error()) {
        auto const& bitmap = bitmap_or_error.value().image;
        // Looks like a valid GIF. Try to load the other frames:
        dbgln_if(GIF_DEBUG, "bitmap size: {}", bitmap->size());
        dbgln_if(GIF_DEBUG, "codec size: {}", gif_decoder.size());
        dbgln_if(GIF_DEBUG, "is_animated: {}", gif_decoder.is_animated());
        dbgln_if(GIF_DEBUG, "loop_count: {}", gif_decoder.loop_count());
        dbgln_if(GIF_DEBUG, "frame_count: {}", gif_decoder.frame_count());
        for (size_t i = 0; i < gif_decoder.frame_count(); ++i) {
            auto ifd_or_error = gif_decoder.frame(i);
            if (ifd_or_error.is_error()) {
                dbgln_if(GIF_DEBUG, "frame #{} error: {}", i, ifd_or_error.release_error());
            } else {
                auto ifd = ifd_or_error.release_value();
                dbgln_if(GIF_DEBUG, "frame #{} size: {}", i, ifd.image->size());
                dbgln_if(GIF_DEBUG, "frame #{} duration: {}", i, ifd.duration);
            }
        }
        dbgln_if(GIF_DEBUG, "Done.");
    }

    return 0;
}
