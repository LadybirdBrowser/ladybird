/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <LibGfx/FourCC.h>

struct hb_blob_t;
struct hb_face_t;

namespace Gfx {

// Reads big-endian values, as font data stores them.
class FontDataReader {
public:
    FontDataReader() = default;
    explicit FontDataReader(ReadonlyBytes bytes)
        : m_bytes(bytes)
    {
    }

    ReadonlyBytes bytes() const { return m_bytes; }
    bool is_empty() const { return m_bytes.is_empty(); }
    bool contains(size_t offset, size_t length) const { return offset <= m_bytes.size() && length <= m_bytes.size() - offset; }

    u8 u8_at(size_t offset) const;
    u16 u16_at(size_t offset) const;
    i16 i16_at(size_t offset) const { return static_cast<i16>(u16_at(offset)); }
    u32 u32_at(size_t offset) const;
    i32 i32_at(size_t offset) const { return static_cast<i32>(u32_at(offset)); }

private:
    ReadonlyBytes m_bytes;
};

// The offset of the table directory of a face, if the data is an SFNT font or a collection of them.
Optional<size_t> table_directory_offset(FontDataReader const&, u32 face_index);

// Whether a face has a table, by the same rules as FontTable, without reading the table.
bool face_has_table(hb_face_t*, FourCC tag);

// A name of a face from its name table, in US English, or else in the first language that the face has it in.
Optional<String> face_name(hb_face_t*, u16 name_id);

// One table of a face, kept while it is read. Like FreeType, it counts a table as missing if its length is 0 or if
// it ends after the end of the font data, and it uses the first entry of the table directory with the tag.
class FontTable : public FontDataReader {
    AK_MAKE_NONCOPYABLE(FontTable);
    AK_MAKE_NONMOVABLE(FontTable);

public:
    FontTable(hb_face_t*, FourCC tag);
    ~FontTable();

    // The table and the font data after it. FreeType reads the fixed-size fields of a table from here, so a field
    // that a table which is too short cuts off comes from the data that follows the table.
    FontDataReader const& with_following_data() const { return m_with_following_data; }

private:
    hb_blob_t* m_blob { nullptr };
    FontDataReader m_with_following_data;
};

}
