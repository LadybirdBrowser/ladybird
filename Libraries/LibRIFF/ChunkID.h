/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Endian.h>
#include <AK/Format.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace RIFF {

static constexpr size_t const chunk_id_size = 4;

// Also referred to as "FourCC" (four character code) in the context of some formats.
struct ChunkID {
    constexpr ChunkID(char const name[4])
    {
        id_data[0] = static_cast<u8>(name[0]);
        id_data[1] = static_cast<u8>(name[1]);
        id_data[2] = static_cast<u8>(name[2]);
        id_data[3] = static_cast<u8>(name[3]);
    }
    constexpr ChunkID(Array<u8, chunk_id_size> data)
        : id_data(data)
    {
    }
    constexpr ChunkID(ChunkID const&) = default;
    constexpr ChunkID(ChunkID&&) = default;
    constexpr ChunkID& operator=(ChunkID const&) = default;
    static constexpr ChunkID from_number(u32 number)
    {
        return Array<u8, chunk_id_size> { {
            static_cast<u8>((number >> 24) & 0xff),
            static_cast<u8>((number >> 16) & 0xff),
            static_cast<u8>((number >> 8) & 0xff),
            static_cast<u8>(number & 0xff),
        } };
    }

    static ErrorOr<ChunkID> read_from_stream(Stream& stream);

    StringView as_ascii_string() const;
    constexpr u32 as_number() const
    {
        return (id_data[0] << 24) | (id_data[1] << 16) | (id_data[2] << 8) | id_data[3];
    }

    bool operator==(ChunkID const&) const = default;
    bool operator==(StringView) const;

    Array<u8, chunk_id_size> id_data;
};
static_assert(AssertSize<ChunkID, chunk_id_size>());

}

template<>
struct AK::Formatter<RIFF::ChunkID> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder& builder, RIFF::ChunkID const& chunk_id)
    {
        TRY(builder.put_padding('\'', 1));
        TRY(builder.put_literal(chunk_id.as_ascii_string()));
        TRY(builder.put_padding('\'', 1));
        return {};
    }
};
