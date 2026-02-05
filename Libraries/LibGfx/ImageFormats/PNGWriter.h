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

namespace Gfx {

// This is not a nested struct to work around https://llvm.org/PR36684
struct PNGWriterOptions {
    // Data for the iCCP chunk.
    // FIXME: Allow writing cICP, sRGB, or gAMA instead too.
    Optional<ReadonlyBytes> icc_data;
};

class PNGWriter {
public:
    using Options = PNGWriterOptions;

    static ErrorOr<ByteBuffer> encode(Gfx::Bitmap const&, Options options = Options {});

private:
    PNGWriter() = default;
};

}
