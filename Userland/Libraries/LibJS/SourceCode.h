/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Position.h>

namespace JS {

class SourceCode : public RefCounted<SourceCode> {
public:
    static NonnullRefPtr<SourceCode const> create(String filename, String code);

    String const& filename() const;
    String const& code() const;

    SourceRange range_from_offsets(u32 start_offset, u32 end_offset) const;

private:
    SourceCode(String filename, String code);

    String m_filename;
    String m_code;

    // For fast mapping of offsets to line/column numbers, we build a list of
    // starting points (with byte offsets into the source string) and which
    // line:column they map to. This can then be binary-searched.
    void fill_position_cache() const;
    Vector<Position> mutable m_cached_positions;
};

}
