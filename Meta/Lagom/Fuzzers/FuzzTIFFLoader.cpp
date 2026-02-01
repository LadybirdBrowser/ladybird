/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageFormats/ImageDecoderStream.h>
#include <LibGfx/ImageFormats/TIFFLoader.h>
#include <stdio.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    auto stream = adopt_ref(*new Gfx::ImageDecoderStream());
    auto buffer_or_error = ByteBuffer::copy(data, size);
    if (buffer_or_error.is_error())
        return 0;

    stream->append_chunk(buffer_or_error.release_value());
    stream->close();

    auto decoder_or_error = Gfx::TIFFImageDecoderPlugin::create(move(stream));
    if (decoder_or_error.is_error())
        return 0;
    auto decoder = decoder_or_error.release_value();
    (void)decoder->frame(0);
    return 0;
}
