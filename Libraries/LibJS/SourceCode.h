/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Position.h>

namespace JS {

class JS_API SourceCode : public RefCounted<SourceCode> {
public:
    static NonnullRefPtr<SourceCode const> create(String filename, Utf16String code);

    String const& filename() const { return m_filename; }
    Utf16String const& code() const { return m_code; }
    Utf16View const& code_view() const { return m_code_view; }
    size_t length_in_code_units() const { return m_length_in_code_units; }

    u16 const* utf16_data() const;

    SourceRange range_from_offsets(u32 start_offset, u32 end_offset) const;

private:
    SourceCode(String filename, Utf16String code);

    String m_filename;
    Utf16String m_code;
    Utf16View m_code_view;
    size_t m_length_in_code_units { 0 };

    // For fast mapping of offsets to line/column numbers, we build a list of
    // starting points (with byte offsets into the source string) and which
    // line:column they map to. This can then be binary-searched.
    void fill_position_cache() const;
    Vector<Position> mutable m_cached_positions;

    // Cached UTF-16 widening of ASCII source data, lazily populated by
    // utf16_data() for use by the Rust compilation pipeline.
    Vector<u16> mutable m_utf16_data_cache;
};

}
