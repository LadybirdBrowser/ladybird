/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibGfx/Forward.h>
#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>

namespace Gfx {

enum class PNGCompression : u8 {
    Default,
    // Encodes much faster, at the cost of a larger file.
    Fast,
};

// This is not a nested struct to work around https://llvm.org/PR36684
struct PNGWriterOptions {
    // Data for the iCCP chunk.
    // FIXME: Allow writing sRGB or gAMA instead too.
    Optional<ReadonlyBytes> icc_data;
    // Data for the cICP chunk, which describes the color space in place of an ICC profile.
    Optional<Media::CodingIndependentCodePoints> cicp {};
    PNGCompression compression { PNGCompression::Default };
};

class PNGWriter {
public:
    using Options = PNGWriterOptions;

    static ErrorOr<ByteBuffer> encode(Gfx::Bitmap const&, Options options = Options {});

private:
    PNGWriter() = default;
};

}
