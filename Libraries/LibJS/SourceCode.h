/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibCore/ImmutableBytes.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Position.h>

namespace JS {

class JS_API SourceCode : public RefCounted<SourceCode> {
public:
    static NonnullRefPtr<SourceCode const> create(Utf16String filename, Utf16String code);
    static NonnullRefPtr<SourceCode const> create(Utf16String filename, size_t length_in_code_units, ByteString source_encoding, Core::ImmutableBytes source_bytes);

    Utf16String const& filename() const { return m_filename; }
    Utf16String const& code() const;
    Utf16View const& code_view() const;
    size_t length_in_code_units() const { return m_length_in_code_units; }

    u16 const* utf16_data() const;
    Utf16String source_text_from_offsets(size_t start_offset, size_t length) const;

    SourceRange range_from_offsets(u32 start_offset, u32 end_offset) const;

private:
    SourceCode(Utf16String filename, Utf16String code);
    SourceCode(Utf16String filename, size_t length_in_code_units, ByteString source_encoding, Core::ImmutableBytes source_bytes);
    void ensure_code() const;
    Utf16String decode_source_range(size_t start_offset, size_t length) const;
    bool source_bytes_can_be_sliced_by_code_unit_offsets() const;

    Utf16String m_filename;
    Optional<Utf16String> mutable m_code;
    ByteString m_source_encoding;
    Core::ImmutableBytes mutable m_source_bytes;
    Utf16View mutable m_code_view;
    size_t m_length_in_code_units { 0 };

    // For fast mapping of offsets to line/column numbers, we build a list of
    // starting points (with byte offsets into the source string) and which
    // line:column they map to. This can then be binary-searched.
    void fill_position_cache() const;
    struct CachedPosition {
        Position position;
        u32 offset { 0 };
    };
    Vector<CachedPosition> mutable m_cached_positions;

    // Cached UTF-16 widening of ASCII source data, lazily populated by
    // utf16_data() for use by the Rust compilation pipeline.
    Vector<u16> mutable m_utf16_data_cache;
    Optional<bool> mutable m_source_bytes_can_be_sliced_by_code_unit_offsets;
};

}
