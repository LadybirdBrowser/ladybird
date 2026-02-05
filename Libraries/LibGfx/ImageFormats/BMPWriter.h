/*
 * Copyright (c) 2020, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Gfx {

class Bitmap;

// This is not a nested struct to work around https://llvm.org/PR36684
struct BMPWriterOptions {
    enum class DibHeader : u32 {
        Info = 40,
        V3 = 56,
        V4 = 108,
        V5 = 124,
    };
    DibHeader dib_header = DibHeader::V5;

    Optional<ReadonlyBytes> icc_data;
};

class BMPWriter {
public:
    using Options = BMPWriterOptions;
    static ErrorOr<ByteBuffer> encode(Bitmap const&, Options options = Options {});

private:
    BMPWriter() = default;

    ErrorOr<ByteBuffer> dump(Bitmap const&, Options options);

    enum class Compression : u32 {
        BI_RGB = 0,
        BI_BITFIELDS = 3,
    };

    static ByteBuffer compress_pixel_data(ByteBuffer, Compression);

    Compression m_compression { Compression::BI_BITFIELDS };

    int m_bytes_per_pixel { 4 };
    bool m_include_alpha_channel { true };
};

}
