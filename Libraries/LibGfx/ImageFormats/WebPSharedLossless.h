/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCompress/Deflate.h>

namespace Gfx {

constexpr Array kCodeLengthCodeOrder = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

// WebP-lossless's CanonicalCodes are almost identical to deflate's.
// One difference is that codes with a single element in webp-lossless consume 0 bits to produce that single element,
// while they consume 1 bit in Compress::CanonicalCode. This class wraps Compress::CanonicalCode to handle the case
// where the codes contain just a single element, and dispatches to Compress::CanonicalCode else.
class CanonicalCode {
public:
    CanonicalCode() = default;

    static ErrorOr<CanonicalCode> from_bytes(ReadonlyBytes);
    ErrorOr<u32> read_symbol(LittleEndianInputBitStream&) const;
    ErrorOr<void> write_symbol(LittleEndianOutputBitStream&, u32) const;

private:
    explicit CanonicalCode(u32 single_symbol)
        : m_code(single_symbol)
    {
    }

    explicit CanonicalCode(Compress::CanonicalCode code)
        : m_code(move(code))
    {
    }

    Variant<u32, Compress::CanonicalCode> m_code { 0 };
};

ALWAYS_INLINE ErrorOr<void> CanonicalCode::write_symbol(LittleEndianOutputBitStream& bit_stream, u32 symbol) const
{
    TRY(m_code.visit(
        [&](u32 single_code) __attribute__((always_inline))->ErrorOr<void> { VERIFY(symbol == single_code); return {}; },
        [&](Compress::CanonicalCode const& code) __attribute__((always_inline)) { return code.write_symbol(bit_stream, symbol); }));
    return {};
}

// https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification#61_overview
// "From here on, we refer to this set as a prefix code group."
class PrefixCodeGroup {
public:
    PrefixCodeGroup() = default;
    PrefixCodeGroup(PrefixCodeGroup&&) = default;
    PrefixCodeGroup(PrefixCodeGroup const&) = delete;

    CanonicalCode& operator[](int i) { return m_codes[i]; }
    CanonicalCode const& operator[](int i) const { return m_codes[i]; }

private:
    Array<CanonicalCode, 5> m_codes;
};

enum class ImageKind {
    SpatiallyCoded,
    EntropyCoded,
};

enum TransformType {
    // predictor-tx         =  %b00 predictor-image
    PREDICTOR_TRANSFORM = 0,

    // color-tx             =  %b01 color-image
    COLOR_TRANSFORM = 1,

    // subtract-green-tx    =  %b10
    SUBTRACT_GREEN_TRANSFORM = 2,

    // color-indexing-tx    =  %b11 color-indexing-image
    COLOR_INDEXING_TRANSFORM = 3,
};

}
