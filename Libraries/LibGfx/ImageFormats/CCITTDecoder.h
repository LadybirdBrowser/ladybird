/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Gfx::CCITT {

// You can find a great overview of CCITT compression schemes here:
// https://www.fileformat.info/mirror/egff/ch09_05.htm

// The CCITT3 specification is accessible at this page:
// https://www.itu.int/rec/T-REC-T.4/en

// And CCITT4's specification is available here:
// https://www.itu.int/rec/T-REC-T.6/en

// The unidimensional scheme is originally described in:
// 4.1 One-dimensional coding scheme
// However, this function implements the TIFF variant (see TIFFLoader.h for a spec link),
// differences are detailed in section:
// Section 10: Modified Huffman Compression
ErrorOr<ByteBuffer> decode_ccitt_rle(ReadonlyBytes bytes, u32 image_width, u32 image_height);

// While this is named for a CCITT context, this struct holds data like TIFF's T4Options tag
struct Group3Options {
    enum class Mode : u8 {
        OneDimension,
        TwoDimensions,
    };

    enum class Compression : u8 {
        Uncompressed,
        Compressed,
    };

    enum class UseFillBits : u8 {
        No = 0,
        Yes = 1,
    };

    // Addition from the PDF specification
    enum class RequireEndOfLine : u8 {
        No = 0,
        Yes = 1,
    };

    enum class EncodedByteAligned : u8 {
        No = 0,
        Yes = 1,
    };

    Mode dimensions = Mode::OneDimension;
    Compression compression = Compression::Compressed;
    UseFillBits use_fill_bits = UseFillBits::No;

    // Default values are set to be compatible with the CCITT specification
    RequireEndOfLine require_end_of_line = RequireEndOfLine::Yes;
    EncodedByteAligned encoded_byte_aligned = EncodedByteAligned::No;
};

ErrorOr<ByteBuffer> decode_ccitt_group3(ReadonlyBytes bytes, u32 image_width, u32 image_height, Group3Options const& options);

ErrorOr<ByteBuffer> decode_ccitt_group4(ReadonlyBytes bytes, u32 image_width, u32 image_height);

}
