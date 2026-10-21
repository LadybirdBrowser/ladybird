/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/NumericLimits.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGfx/Font/FontTable.h>

#include <harfbuzz/hb-ot.h>
#include <harfbuzz/hb.h>

namespace Gfx {

u8 FontDataReader::u8_at(size_t offset) const
{
    VERIFY(contains(offset, 1));
    return m_bytes[offset];
}

u16 FontDataReader::u16_at(size_t offset) const
{
    VERIFY(contains(offset, 2));
    return static_cast<u16>((m_bytes[offset] << 8) | m_bytes[offset + 1]);
}

u32 FontDataReader::u32_at(size_t offset) const
{
    VERIFY(contains(offset, 4));
    return (static_cast<u32>(m_bytes[offset]) << 24)
        | (static_cast<u32>(m_bytes[offset + 1]) << 16)
        | (static_cast<u32>(m_bytes[offset + 2]) << 8)
        | static_cast<u32>(m_bytes[offset + 3]);
}

struct TableLocation {
    size_t offset { 0 };
    size_t length { 0 };
};

Optional<size_t> table_directory_offset(FontDataReader const& data, u32 face_index)
{
    if (!data.contains(0, 4))
        return {};
    // https://learn.microsoft.com/en-us/typography/opentype/spec/otff#ttc-header
    if (data.u32_at(0) == FourCC { "ttcf" }.to_u32()) {
        if (!data.contains(8, 4) || face_index >= data.u32_at(8) || !data.contains(12 + 4 * static_cast<size_t>(face_index), 4))
            return {};
        return data.u32_at(12 + 4 * face_index);
    }
    if (face_index != 0)
        return {};
    return 0;
}

// https://learn.microsoft.com/en-us/typography/opentype/spec/otff#table-directory
static Optional<TableLocation> find_table(FontDataReader const& data, unsigned face_index, FourCC tag)
{
    auto directory_offset = table_directory_offset(data, face_index);
    if (!directory_offset.has_value() || !data.contains(*directory_offset, 12))
        return {};

    auto table_count = data.u16_at(*directory_offset + 4);
    for (size_t index = 0; index < table_count; ++index) {
        auto record_offset = *directory_offset + 12 + 16 * index;
        if (!data.contains(record_offset, 16))
            return {};
        if (data.u32_at(record_offset) != tag.to_u32())
            continue;
        size_t offset = data.u32_at(record_offset + 8);
        size_t length = data.u32_at(record_offset + 12);
        if (offset > data.bytes().size())
            continue;
        if (length > data.bytes().size() - offset) {
            // Metrics tables are simple enough to cut off at the end of the data.
            if (tag != FourCC { "hmtx" } && tag != FourCC { "vmtx" })
                continue;
            length = (data.bytes().size() - offset) & ~static_cast<size_t>(3);
        }
        if (length == 0)
            return {};
        return TableLocation { offset, length };
    }
    return {};
}

static FontDataReader face_data(hb_blob_t* blob)
{
    unsigned length = 0;
    auto const* data = hb_blob_get_data(blob, &length);
    if (!data || length == 0)
        return {};
    return FontDataReader { { reinterpret_cast<u8 const*>(data), length } };
}

bool face_has_table(hb_face_t* face, FourCC tag)
{
    auto* blob = hb_face_reference_blob(face);
    auto data = face_data(blob);
    bool has_table = false;
    if (!data.is_empty()) {
        has_table = find_table(data, hb_face_get_index(face) & 0xFFFF, tag).has_value();
    } else {
        // A face without data of its own lists its tables without copying them.
        unsigned tag_count = hb_face_get_table_tags(face, 0, nullptr, nullptr);
        Vector<hb_tag_t> tags;
        tags.resize(tag_count);
        hb_face_get_table_tags(face, 0, &tag_count, tags.data());
        has_table = tags.contains_slow(tag.to_u32());
    }
    hb_blob_destroy(blob);
    return has_table;
}

Optional<String> face_name(hb_face_t* face, u16 name_id)
{
    unsigned entry_count = 0;
    auto const* entries = hb_ot_name_list_names(face, &entry_count);
    hb_language_t language = HB_LANGUAGE_INVALID;
    for (unsigned index = 0; index < entry_count; ++index) {
        auto const& entry = entries[index];
        if (entry.name_id != name_id)
            continue;
        if (language == HB_LANGUAGE_INVALID)
            language = entry.language;
        if (entry.language == hb_language_from_string("en", -1) || entry.language == hb_language_from_string("en-us", -1)) {
            language = entry.language;
            break;
        }
    }
    if (language == HB_LANGUAGE_INVALID)
        return {};
    auto length = hb_ot_name_get_utf8(face, name_id, language, nullptr, nullptr);
    if (length == 0 || length == NumericLimits<unsigned>::max())
        return {};
    Vector<char> name;
    if (name.try_resize(length + 1).is_error())
        return {};
    auto capacity = length + 1;
    hb_ot_name_get_utf8(face, name_id, language, &capacity, name.data());
    auto result = String::from_utf8({ name.data(), capacity });
    if (result.is_error())
        return {};
    return result.release_value();
}

FontTable::FontTable(hb_face_t* face, FourCC tag)
{
    m_blob = hb_face_reference_blob(face);
    if (auto data = face_data(m_blob); !data.is_empty()) {
        // The upper 16 bits of the index select a named instance, not a face.
        if (auto location = find_table(data, hb_face_get_index(face) & 0xFFFF, tag); location.has_value()) {
            static_cast<FontDataReader&>(*this) = FontDataReader { data.bytes().slice(location->offset, location->length) };
            m_with_following_data = FontDataReader { data.bytes().slice(location->offset) };
        }
        return;
    }

    // A face without data of its own, such as one that CoreText gives, has only its tables.
    hb_blob_destroy(m_blob);
    m_blob = hb_face_reference_table(face, tag.to_u32());
    unsigned length = 0;
    auto const* table_pointer = hb_blob_get_data(m_blob, &length);
    if (table_pointer && length > 0) {
        FontDataReader table { { reinterpret_cast<u8 const*>(table_pointer), length } };
        static_cast<FontDataReader&>(*this) = table;
        m_with_following_data = table;
    }
}

FontTable::~FontTable()
{
    hb_blob_destroy(m_blob);
}

}
