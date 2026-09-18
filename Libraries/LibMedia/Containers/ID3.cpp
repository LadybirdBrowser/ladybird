/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibMedia/BitReader.h>
#include <LibMedia/Containers/ID3.h>

namespace Media::ID3 {

// https://mutagen-specs.readthedocs.io/en/latest/id3/id3v2.4.0-structure.html
Optional<size_t> version_2_tag_size(ReadonlyBytes bytes)
{
    static constexpr u8 UNKNOWN_VERSION = 0xFF;
    static constexpr u8 FIRST_VERSION_WITH_FOOTER = 4;
    static constexpr u8 FOOTER_PRESENT_FLAG = 0x10;
    static constexpr size_t FOOTER_SIZE = 10;
    static constexpr size_t SIZE_BYTE_COUNT = 4;

    BitReader reader { bytes };

    for (auto character : "ID3"sv) {
        if (reader.read_bits<u8>(8) != static_cast<u8>(character))
            return {};
    }

    auto version = reader.read_bits<u8>(8);
    auto revision = reader.read_bits<u8>(8);
    if (version == UNKNOWN_VERSION || revision == UNKNOWN_VERSION)
        return {};
    auto flags = reader.read_bits<u8>(8);

    size_t size = 0;
    for (size_t index = 0; index < SIZE_BYTE_COUNT; index++) {
        // Each size byte leaves its most significant bit clear so that a tag cannot be mistaken for a sync code.
        if (reader.read_bit())
            return {};
        size = (size << 7) | reader.read_bits<u8>(7);
    }
    if (reader.has_overrun())
        return {};
    size += VERSION_2_HEADER_SIZE;

    if (version >= FIRST_VERSION_WITH_FOOTER && (flags & FOOTER_PRESENT_FLAG) != 0)
        size += FOOTER_SIZE;

    return size;
}

}
