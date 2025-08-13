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

    SourceRange range_from_offsets(u32 start_offset, u32 end_offset) const;

private:
    SourceCode(String filename, Utf16String code);

    String m_filename;
    Utf16String m_code;

    // For fast mapping of offsets to line/column numbers, we build a list of
    // starting points (with byte offsets into the source string) and which
    // line:column they map to. This can then be binary-searched.
    void fill_position_cache() const;
    Vector<Position> mutable m_cached_positions;
};

}
