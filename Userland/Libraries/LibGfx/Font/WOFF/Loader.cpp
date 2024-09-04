/*
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/IntegralMath.h>
#include <LibCompress/Zlib.h>
#include <LibCore/Resource.h>
#include <LibGfx/Font/WOFF/Loader.h>
#include <LibGfx/FourCC.h>

namespace WOFF {

// https://www.w3.org/TR/WOFF/#WOFFHeader
struct [[gnu::packed]] Header {
    BigEndian<u32> signature;        // 0x774F4646 'wOFF'
    BigEndian<u32> flavor;           // The "sfnt version" of the input font.
    BigEndian<u32> length;           // Total size of the WOFF file.
    BigEndian<u16> num_tables;       // Number of entries in directory of font tables.
    BigEndian<u16> reserved;         // Reserved; set to zero.
    BigEndian<u32> total_sfnt_size;  // Total size needed for the uncompressed font data, including
                                     // the sfnt header, directory, and font tables (including padding).
    BigEndian<u16> major_version;    // Major version of the WOFF file.
    BigEndian<u16> minor_version;    // Minor version of the WOFF file.
    BigEndian<u32> meta_offset;      // Offset to metadata block, from beginning of WOFF file.
    BigEndian<u32> meta_length;      // Length of compressed metadata block.
    BigEndian<u32> meta_orig_length; // Uncompressed size of metadata block.
    BigEndian<u32> priv_offset;      // Offset to private data block, from beginning of WOFF file.
    BigEndian<u32> priv_length;      // Length of private data block.
};
static_assert(AssertSize<Header, 44>());

// https://www.w3.org/TR/WOFF/#TableDirectory
struct [[gnu::packed]] TableDirectoryEntry {
    Gfx::FourCC tag;              // 4-byte sfnt table identifier.
    BigEndian<u32> offset;        // Offset to the data, from beginning of WOFF file.
    BigEndian<u32> comp_length;   // Length of the compressed data, excluding padding.
    BigEndian<u32> orig_length;   // Length of the uncompressed table, excluding padding.
    BigEndian<u32> orig_checksum; // Checksum of the uncompressed table.
};
static_assert(AssertSize<TableDirectoryEntry, 20>());

}

template<>
class AK::Traits<WOFF::Header> : public DefaultTraits<WOFF::Header> {
public:
    static constexpr bool is_trivially_serializable() { return true; }
};

template<>
class AK::Traits<WOFF::TableDirectoryEntry> : public DefaultTraits<WOFF::TableDirectoryEntry> {
public:
    static constexpr bool is_trivially_serializable() { return true; }
};

namespace WOFF {

static constexpr u32 WOFF_SIGNATURE = 0x774F4646;

static u16 pow_2_less_than_or_equal(u16 x)
{
    VERIFY(x > 0);
    VERIFY(x < 32769);
    return 1 << (sizeof(u16) * 8 - count_leading_zeroes_safe<u16>(x - 1));
}

ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_from_resource(Core::Resource const& resource, unsigned index)
{
    return try_load_from_externally_owned_memory(resource.data(), index);
}

using Uint8 = u8;
using Uint16 = BigEndian<u16>;
using Int16 = BigEndian<i16>;
using Uint32 = BigEndian<u32>;
using Int32 = BigEndian<i32>;

// https://learn.microsoft.com/en-us/typography/opentype/spec/otff#table-directory
// Table Directory (known as the Offset Table in ISO-IEC 14496-22:2019)
struct [[gnu::packed]] TableDirectory {
    Uint32 sfnt_version;
    Uint16 num_tables;     // Number of tables.
    Uint16 search_range;   // (Maximum power of 2 <= numTables) x 16.
    Uint16 entry_selector; // Log2(maximum power of 2 <= numTables).
    Uint16 range_shift;    // NumTables x 16 - searchRange.
};
static_assert(AssertSize<TableDirectory, 12>());

using Offset16 = BigEndian<u16>;
using Offset32 = BigEndian<u32>;

// https://learn.microsoft.com/en-us/typography/opentype/spec/otff#table-directory
struct [[gnu::packed]] TableRecord {
    Gfx::FourCC table_tag; // Table identifier.
    Uint32 checksum;       // CheckSum for this table.
    Offset32 offset;       // Offset from beginning of TrueType font file.
    Uint32 length;         // Length of this table.
};
static_assert(AssertSize<TableRecord, 16>());

ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_from_externally_owned_memory(ReadonlyBytes buffer, unsigned int index)
{
    FixedMemoryStream stream(buffer);
    auto header = TRY(stream.read_value<Header>());

    // The signature field in the WOFF header MUST contain the "magic number" 0x774F4646. If the field does not contain this value, user agents MUST reject the file as invalid.
    if (header.signature != WOFF_SIGNATURE)
        return Error::from_string_literal("Invalid WOFF signature");
    // The flavor field corresponds to the "sfnt version" field found at the beginning of an sfnt file,
    // indicating the type of font data contained. Although only fonts of type 0x00010000 (the version number 1.0 as a 16.16 fixed-point value, indicating TrueType glyph data)
    // and 0x4F54544F (the tag 'OTTO', indicating CFF glyph data) are widely supported at present,
    // it is not an error in the WOFF file if the flavor field contains a different value,
    // indicating a WOFF-packaged version of a different sfnt flavor.
    // (The value 0x74727565 'true' has been used for some TrueType-flavored fonts on Mac OS, for example.)
    // Whether client software will actually support other types of sfnt font data is outside the scope of the WOFF specification, which simply describes how the sfnt is repackaged for Web use.

    auto expected_total_sfnt_size = sizeof(TableDirectory) + header.num_tables * 16;
    if (header.length > buffer.size())
        return Error::from_string_literal("Invalid WOFF length");
    if (header.num_tables == 0 || header.num_tables > NumericLimits<u16>::max() / 16)
        return Error::from_string_literal("Invalid WOFF numTables");
    if (header.reserved != 0)
        return Error::from_string_literal("Invalid WOFF reserved field");
    if (header.meta_length == 0 && header.meta_offset != 0)
        return Error::from_string_literal("Invalid WOFF meta block offset");
    if (header.priv_length == 0 && header.priv_offset != 0)
        return Error::from_string_literal("Invalid WOFF private block offset");
    if (sizeof(Header) + header.num_tables * sizeof(TableDirectoryEntry) > header.length)
        return Error::from_string_literal("Truncated WOFF table directory");
    if (header.total_sfnt_size < expected_total_sfnt_size)
        return Error::from_string_literal("Invalid WOFF total sfnt size");
    if (header.total_sfnt_size > 10 * MiB)
        return Error::from_string_literal("Uncompressed font is more than 10 MiB");
    auto font_buffer = TRY(ByteBuffer::create_zeroed(header.total_sfnt_size));

    u16 search_range = pow_2_less_than_or_equal(header.num_tables);
    TableDirectory table_directory {
        .sfnt_version = header.flavor,
        .num_tables = header.num_tables,
        .search_range = search_range * 16,
        .entry_selector = AK::log2(search_range),
        .range_shift = header.num_tables * 16 - search_range * 16,
    };
    font_buffer.overwrite(0, &table_directory, sizeof(table_directory));

    size_t font_buffer_offset = sizeof(TableDirectory) + header.num_tables * sizeof(TableRecord);
    for (size_t i = 0; i < header.num_tables; ++i) {
        auto entry = TRY(stream.read_value<TableDirectoryEntry>());

        expected_total_sfnt_size += (entry.orig_length + 3) & 0xFFFFFFFC;
        if (expected_total_sfnt_size > header.total_sfnt_size)
            return Error::from_string_literal("Invalid WOFF total sfnt size");
        if ((size_t)entry.offset + entry.comp_length > header.length)
            return Error::from_string_literal("Truncated WOFF table");
        if (font_buffer_offset + entry.orig_length > font_buffer.size())
            return Error::from_string_literal("Uncompressed WOFF table too big");
        if (entry.comp_length < entry.orig_length) {
            auto compressed_data_stream = make<FixedMemoryStream>(buffer.slice(entry.offset, entry.comp_length));
            auto decompressor = TRY(Compress::ZlibDecompressor::create(move(compressed_data_stream)));
            auto decompressed = TRY(decompressor->read_until_eof());
            if (entry.orig_length != decompressed.size())
                return Error::from_string_literal("Invalid decompressed WOFF table length");
            font_buffer.overwrite(font_buffer_offset, decompressed.data(), entry.orig_length);
        } else {
            if (entry.comp_length != entry.orig_length)
                return Error::from_string_literal("Invalid uncompressed WOFF table length");
            font_buffer.overwrite(font_buffer_offset, buffer.data() + entry.offset, entry.orig_length);
        }

        size_t table_directory_offset = sizeof(TableDirectory) + i * sizeof(TableRecord);
        TableRecord table_record {
            .table_tag = entry.tag,
            .checksum = entry.orig_checksum,
            .offset = font_buffer_offset,
            .length = entry.orig_length,
        };
        font_buffer.overwrite(table_directory_offset, &table_record, sizeof(table_record));

        font_buffer_offset += entry.orig_length;
    }

    if (header.total_sfnt_size != expected_total_sfnt_size)
        return Error::from_string_literal("Invalid WOFF total sfnt size");

    auto font_data = Gfx::FontData::create_from_byte_buffer(move(font_buffer));
    return TRY(Gfx::Typeface::try_load_from_font_data(move(font_data), index));
}

}
