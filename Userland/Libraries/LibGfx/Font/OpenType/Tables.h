/*
 * Copyright (c) 2020, Srimanta Barua <srimanta.barua1@gmail.com>
 * Copyright (c) 2022, Jelle Raaijmakers <jelle@gmta.nl>
 * Copyright (c) 2023, Lukas Affolter <git@lukasach.dev>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Endian.h>
#include <AK/Error.h>
#include <AK/FixedArray.h>
#include <AK/Forward.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <LibGfx/Font/OpenType/DataTypes.h>

namespace OpenType {

enum class IndexToLocFormat {
    Offset16,
    Offset32,
};

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

// https://learn.microsoft.com/en-us/typography/opentype/spec/otff#table-directory
struct [[gnu::packed]] TableRecord {
    Tag table_tag;   // Table identifier.
    Uint32 checksum; // CheckSum for this table.
    Offset32 offset; // Offset from beginning of TrueType font file.
    Uint32 length;   // Length of this table.
};
static_assert(AssertSize<TableRecord, 16>());

}

template<>
class AK::Traits<OpenType::TableDirectory> : public DefaultTraits<OpenType::TableDirectory> {
public:
    static constexpr bool is_trivially_serializable() { return true; }
};

template<>
class AK::Traits<OpenType::TableRecord> : public DefaultTraits<OpenType::TableRecord> {
public:
    static constexpr bool is_trivially_serializable() { return true; }
};

namespace OpenType {

// https://learn.microsoft.com/en-us/typography/opentype/spec/head
// head: Font Header Table
class Head {
public:
    static ErrorOr<Head> from_slice(ReadonlyBytes);
    u16 units_per_em() const;
    i16 xmin() const;
    i16 ymin() const;
    i16 xmax() const;
    i16 ymax() const;
    u16 style() const;
    u16 lowest_recommended_ppem() const;
    IndexToLocFormat index_to_loc_format() const;

private:
    struct [[gnu::packed]] FontHeaderTable {
        Uint16 major_version;
        Uint16 minor_version;
        Fixed font_revision;
        Uint32 checksum_adjustment;
        Uint32 magic_number;
        Uint16 flags;
        Uint16 units_per_em;
        LongDateTime created;
        LongDateTime modified;
        Int16 x_min;
        Int16 y_min;
        Int16 x_max;
        Int16 y_max;
        Uint16 mac_style;
        Uint16 lowest_rec_ppem;
        Int16 font_direction_hint;
        Int16 index_to_loc_format;
        Int16 glyph_data_format;
    };
    static_assert(AssertSize<FontHeaderTable, 54>());

    Head(FontHeaderTable const& font_header_table)
        : m_data(font_header_table)
    {
    }

    FontHeaderTable const& m_data;
};

// https://learn.microsoft.com/en-us/typography/opentype/spec/hhea
// hhea - Horizontal Header Table
class Hhea {
public:
    static ErrorOr<Hhea> from_slice(ReadonlyBytes);
    i16 ascender() const;
    i16 descender() const;
    i16 line_gap() const;
    u16 advance_width_max() const;
    u16 number_of_h_metrics() const;

private:
    struct [[gnu::packed]] HorizontalHeaderTable {
        Uint16 major_version;
        Uint16 minor_version;
        FWord ascender;
        FWord descender;
        FWord line_gap;
        UFWord advance_width_max;
        FWord min_left_side_bearing;
        FWord min_right_side_bearing;
        FWord x_max_extent;
        Int16 caret_slope_rise;
        Int16 caret_slope_run;
        Int16 caret_offset;
        Int16 reserved[4];
        Int16 metric_data_format;
        Uint16 number_of_h_metrics;
    };
    static_assert(AssertSize<HorizontalHeaderTable, 36>());

    Hhea(HorizontalHeaderTable const& horizontal_header_table)
        : m_data(horizontal_header_table)
    {
    }

    HorizontalHeaderTable const& m_data;
};

// https://learn.microsoft.com/en-us/typography/opentype/spec/maxp
// Maxp: Maximum Profile
class Maxp {
public:
    static ErrorOr<Maxp> from_slice(ReadonlyBytes);

    u16 num_glyphs() const;

private:
    struct [[gnu::packed]] Version0_5 {
        Version16Dot16 version;
        Uint16 num_glyphs;
    };
    static_assert(AssertSize<Version0_5, 6>());

    struct [[gnu::packed]] Version1_0 : Version0_5 {
        Uint16 max_points;
        Uint16 max_contours;
        Uint16 max_composite_points;
        Uint16 max_composite_contours;
        Uint16 max_zones;
        Uint16 max_twilight_points;
        Uint16 max_storage;
        Uint16 max_function_defs;
        Uint16 max_instruction_defs;
        Uint16 max_stack_elements;
        Uint16 max_size_of_instructions;
        Uint16 max_component_elements;
        Uint16 max_component_depths;
    };
    static_assert(AssertSize<Version1_0, 32>());

    Maxp(Variant<Version0_5 const*, Version1_0 const*> data)
        : m_data(move(data))
    {
        VERIFY(m_data.visit([](auto const* any) { return any != nullptr; }));
    }

    // NOTE: Whichever pointer is present is non-null, but Variant can't contain references.
    Variant<Version0_5 const*, Version1_0 const*> m_data;
};

struct GlyphHorizontalMetrics {
    u16 advance_width;
    i16 left_side_bearing;
};

// https://learn.microsoft.com/en-us/typography/opentype/spec/hmtx
// hmtx: Horizontal Metrics Table
class Hmtx {
public:
    static ErrorOr<Hmtx> from_slice(ReadonlyBytes, u32 num_glyphs, u32 number_of_h_metrics);
    GlyphHorizontalMetrics get_glyph_horizontal_metrics(u32 glyph_id) const;

private:
    struct [[gnu::packed]] LongHorMetric {
        Uint16 advance_width;
        Int16 lsb;
    };
    static_assert(AssertSize<LongHorMetric, 4>());

    Hmtx(ReadonlySpan<LongHorMetric> long_hor_metrics, ReadonlySpan<Int16> left_side_bearings)
        : m_long_hor_metrics(long_hor_metrics)
        , m_left_side_bearings(left_side_bearings)
    {
    }

    ReadonlySpan<LongHorMetric> m_long_hor_metrics;
    ReadonlySpan<Int16> m_left_side_bearings;
};

// https://learn.microsoft.com/en-us/typography/opentype/spec/os2
// OS/2: OS/2 and Windows Metrics Table
class OS2 {
public:
    static ErrorOr<OS2> from_slice(ReadonlyBytes);

    u16 weight_class() const;
    u16 width_class() const;
    u16 selection() const;
    i16 typographic_ascender() const;
    i16 typographic_descender() const;
    i16 typographic_line_gap() const;

    bool use_typographic_metrics() const;

    [[nodiscard]] Optional<i16> x_height() const;

private:
    struct [[gnu::packed]] Version0 {
        Uint16 version;
        Int16 avg_char_width;
        Uint16 us_weight_class;
        Uint16 us_width_class;
        Uint16 fs_type;
        Int16 y_subscript_x_size;
        Int16 y_subscript_y_size;
        Int16 y_subscript_x_offset;
        Int16 y_subscript_y_offset;
        Int16 y_superscript_x_size;
        Int16 y_superscript_y_size;
        Int16 y_superscript_x_offset;
        Int16 y_superscript_y_offset;
        Int16 y_strikeout_size;
        Int16 y_strikeout_position;
        Int16 s_family_class;
        Uint8 panose[10];
        Uint32 ul_unicode_range1;
        Uint32 ul_unicode_range2;
        Uint32 ul_unicode_range3;
        Uint32 ul_unicode_range4;
        Tag ach_vend_id;
        Uint16 fs_selection;
        Uint16 fs_first_char_index;
        Uint16 us_last_char_index;
        Int16 s_typo_ascender;
        Int16 s_typo_descender;
        Int16 s_typo_line_gap;
        Uint16 us_win_ascent;
        Uint16 us_win_descent;
    };
    static_assert(AssertSize<Version0, 78>());

    struct [[gnu::packed]] Version1 : Version0 {
        Uint32 ul_code_page_range1;
        Uint32 ul_code_page_range2;
    };
    static_assert(AssertSize<Version1, 86>());

    struct [[gnu::packed]] Version2 : Version1 {
        Int16 sx_height;
        Int16 s_cap_height;
        Uint16 us_default_char;
        Uint16 us_break_char;
        Uint16 us_max_context;
    };
    static_assert(AssertSize<Version2, 96>());

    explicit OS2(Variant<Version0 const*, Version1 const*, Version2 const*> data)
        : m_data(move(data))
    {
    }

    Variant<Version0 const*, Version1 const*, Version2 const*> m_data;
};

// https://learn.microsoft.com/en-us/typography/opentype/spec/name
// name: Naming Table
class Name {
public:
    enum class Platform : u16 {
        Unicode = 0,
        Macintosh = 1,
        Windows = 3,
    };
    enum class MacintoshLanguage : u16 {
        English = 0,
    };
    enum class WindowsLanguage : u16 {
        EnglishUnitedStates = 0x0409,
    };
    static ErrorOr<Name> from_slice(ReadonlyBytes);

    String family_name() const { return string_for_id(NameId::FamilyName); }
    String subfamily_name() const { return string_for_id(NameId::SubfamilyName); }
    String typographic_family_name() const { return string_for_id(NameId::TypographicFamilyName); }
    String typographic_subfamily_name() const { return string_for_id(NameId::TypographicSubfamilyName); }

private:
    // https://learn.microsoft.com/en-us/typography/opentype/spec/name#name-records
    struct [[gnu::packed]] NameRecord {
        Uint16 platform_id;
        Uint16 encoding_id;
        Uint16 language_id;
        Uint16 name_id;
        Uint16 length;
        Offset16 string_offset;
    };
    static_assert(AssertSize<NameRecord, 12>());

    // https://learn.microsoft.com/en-us/typography/opentype/spec/name#naming-table-version-0
    struct [[gnu::packed]] NamingTableVersion0 {
        Uint16 version;
        Uint16 count;
        Offset16 storage_offset;
        // NameRecords are stored in a separate span.
        // NameRecord name_record[0];
    };
    static_assert(AssertSize<NamingTableVersion0, 6>());

    enum class NameId : u16 {
        Copyright = 0,
        FamilyName = 1,
        SubfamilyName = 2,
        UniqueIdentifier = 3,
        FullName = 4,
        VersionString = 5,
        PostscriptName = 6,
        Trademark = 7,
        Manufacturer = 8,
        Designer = 9,
        Description = 10,
        TypographicFamilyName = 16,
        TypographicSubfamilyName = 17,
    };

    Name(NamingTableVersion0 const& naming_table, ReadonlySpan<NameRecord> name_records, ReadonlyBytes string_data)
        : m_naming_table(naming_table)
        , m_name_records(name_records)
        , m_string_data(string_data)
    {
    }

    [[nodiscard]] String string_for_id(NameId) const;

    NamingTableVersion0 const& m_naming_table;
    ReadonlySpan<NameRecord> m_name_records;
    ReadonlyBytes m_string_data;
};

// https://learn.microsoft.com/en-us/typography/opentype/spec/kern
// kern - Kerning
class Kern {
public:
    static ErrorOr<Kern> from_slice(ReadonlyBytes);
    i16 get_glyph_kerning(u16 left_glyph_id, u16 right_glyph_id) const;

    struct [[gnu::packed]] Header {
        Uint16 version;
        Uint16 n_tables;
    };
    static_assert(AssertSize<Header, 4>());

    struct [[gnu::packed]] SubtableHeader {
        Uint16 version;
        Uint16 length;
        Uint16 coverage;
    };
    static_assert(AssertSize<SubtableHeader, 6>());

    // https://learn.microsoft.com/en-us/typography/opentype/spec/kern#format-0
    struct [[gnu::packed]] Format0 {
        Uint16 n_pairs;
        Uint16 search_range;
        Uint16 entry_selector;
        Uint16 range_shift;
    };
    static_assert(AssertSize<Format0, 8>());

    struct [[gnu::packed]] Format0Pair {
        Uint16 left;
        Uint16 right;
        FWord value;
    };
    static_assert(AssertSize<Format0Pair, 6>());

private:
    // Non-spec structs for easier reference
    struct Format0Table {
        Format0 const& header;
        ReadonlySpan<Format0Pair> pairs;
    };
    struct UnsupportedTable { };
    struct Subtable {
        SubtableHeader const& header;
        Variant<Format0Table, UnsupportedTable> table;
    };

    Kern(Header const& header, Vector<Subtable> subtables)
        : m_header(header)
        , m_subtables(move(subtables))
    {
    }

    static Optional<i16> read_glyph_kerning_format0(Format0Table const& format0, u16 left_glyph_id, u16 right_glyph_id);

    Header const& m_header;
    Vector<Subtable> const m_subtables;
};

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#feature-list-table
struct [[gnu::packed]] FeatureRecord {
    Tag feature_tag;
    Offset16 feature_offset;
};
static_assert(AssertSize<FeatureRecord, 6>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#feature-list-table
struct [[gnu::packed]] FeatureList {
    Uint16 feature_count;
    FeatureRecord feature_records[];
};
static_assert(AssertSize<FeatureList, 2>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#feature-table
struct [[gnu::packed]] Feature {
    Offset16 feature_params_offset;
    Uint16 lookup_index_count;
    Uint16 lookup_list_indices[];
};
static_assert(AssertSize<Feature, 4>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#lookup-table
struct [[gnu::packed]] Lookup {
    Uint16 lookup_type;
    Uint16 lookup_flag;
    Uint16 subtable_count;
    Offset16 subtable_offsets[];
};
static_assert(AssertSize<Lookup, 6>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#lookup-list-table
struct [[gnu::packed]] LookupList {
    Uint16 lookup_count;
    Offset16 lookup_offsets[];
};
static_assert(AssertSize<LookupList, 2>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#coverage-format-1
struct [[gnu::packed]] CoverageFormat1 {
    Uint16 coverage_format;
    Uint16 glyph_count;
    Uint16 glyph_array[];
};
static_assert(AssertSize<CoverageFormat1, 4>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#coverage-format-2
struct [[gnu::packed]] RangeRecord {
    Uint16 start_glyph_id;
    Uint16 end_glyph_id;
    Uint16 start_coverage_index;
};
static_assert(AssertSize<RangeRecord, 6>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#coverage-format-2
struct [[gnu::packed]] CoverageFormat2 {
    Uint16 coverage_format;
    Uint16 range_count;
    RangeRecord range_records[];
};
static_assert(AssertSize<CoverageFormat2, 4>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#class-definition-table-format-2
struct [[gnu::packed]] ClassRangeRecord {
    Uint16 start_glyph_id;
    Uint16 end_glyph_id;
    Uint16 class_;
};
static_assert(AssertSize<ClassRangeRecord, 6>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#class-definition-table-format-2
struct [[gnu::packed]] ClassDefFormat2 {
    Uint16 class_format;
    Uint16 class_range_count;
    ClassRangeRecord class_range_records[];
};
static_assert(AssertSize<ClassDefFormat2, 4>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#script-list-table-and-script-record
struct [[gnu::packed]] ScriptRecord {
    Tag script_tag;
    Offset16 script_offset;
};
static_assert(AssertSize<ScriptRecord, 6>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2#script-list-table-and-script-record
struct [[gnu::packed]] ScriptList {
    Uint16 script_count;
    ScriptRecord script_records[];
};
static_assert(AssertSize<ScriptList, 2>());

// https://learn.microsoft.com/en-us/typography/opentype/spec/gpos#gpos-header
class GPOS {
public:
    // https://learn.microsoft.com/en-us/typography/opentype/spec/gpos#gpos-header
    struct [[gnu::packed]] Version1_0 {
        Uint16 major_version;
        Uint16 minor_version;
        Offset16 script_list_offset;
        Offset16 feature_list_offset;
        Offset16 lookup_list_offset;
    };
    static_assert(AssertSize<Version1_0, 10>());

    // https://learn.microsoft.com/en-us/typography/opentype/spec/gpos#pair-adjustment-positioning-format-1-adjustments-for-glyph-pairs
    struct [[gnu::packed]] PairPosFormat1 {
        Uint16 pos_format;
        Offset16 coverage_offset;
        Uint16 value_format1;
        Uint16 value_format2;
        Uint16 pair_set_count;
        Offset16 pair_set_offsets[];
    };
    static_assert(AssertSize<PairPosFormat1, 10>());

    // https://learn.microsoft.com/en-us/typography/opentype/spec/gpos#value-record
    struct [[gnu::packed]] ValueRecord {
        Int16 x_placement;
        Int16 y_placement;
        Int16 x_advance;
        Int16 y_advance;
        Offset16 x_placement_device_offset;
        Offset16 y_placement_device_offset;
        Offset16 x_advance_device_offset;
        Offset16 y_advance_device_offset;
    };
    static_assert(AssertSize<ValueRecord, 16>());

    // https://learn.microsoft.com/en-us/typography/opentype/spec/gpos#pair-adjustment-positioning-format-2-class-pair-adjustment
    struct [[gnu::packed]] PairPosFormat2 {
        Uint16 pos_format;
        Offset16 coverage_offset;
        Uint16 value_format1;
        Uint16 value_format2;
        Offset16 class_def1_offset;
        Offset16 class_def2_offset;
        Uint16 class1_count;
        Uint16 class2_count;
    };
    static_assert(AssertSize<PairPosFormat2, 16>());

    enum class ValueFormat : u16 {
        X_PLACEMENT = 0x0001,
        Y_PLACEMENT = 0x0002,
        X_ADVANCE = 0x0004,
        Y_ADVANCE = 0x0008,
        X_PLACEMENT_DEVICE = 0x0010,
        Y_PLACEMENT_DEVICE = 0x0020,
        X_ADVANCE_DEVICE = 0x0040,
        Y_ADVANCE_DEVICE = 0x0080,
    };

    Optional<i16> glyph_kerning(u16 left_glyph_id, u16 right_glyph_id) const;

    static ErrorOr<GPOS> from_slice(ReadonlyBytes);

private:
    GPOS(ReadonlyBytes slice, Version1_0 const& header,
        ScriptList const& script_list, ReadonlySpan<ScriptRecord> script_records,
        FeatureList const& feature_list, ReadonlySpan<FeatureRecord> feature_records,
        LookupList const& lookup_list, ReadonlySpan<Offset16> lookup_offsets)
        : m_slice(slice)
        , m_header(header)
        , m_script_list(script_list)
        , m_script_records(script_records)
        , m_feature_list(feature_list)
        , m_feature_records(feature_records)
        , m_lookup_list(lookup_list)
        , m_lookup_offsets(lookup_offsets)
    {
    }

    ReadonlyBytes m_slice;
    Version1_0 const& m_header;

    ScriptList const& m_script_list;
    ReadonlySpan<ScriptRecord> m_script_records;

    FeatureList const& m_feature_list;
    ReadonlySpan<FeatureRecord> m_feature_records;

    LookupList const& m_lookup_list;
    ReadonlySpan<Offset16> m_lookup_offsets;
};
}

namespace AK {
template<>
struct Traits<OpenType::Kern::Header> : public DefaultTraits<OpenType::Kern::Header> {
    static constexpr bool is_trivially_serializable() { return true; }
};
template<>
struct Traits<OpenType::Kern::SubtableHeader> : public DefaultTraits<OpenType::Kern::SubtableHeader> {
    static constexpr bool is_trivially_serializable() { return true; }
};
template<>
struct Traits<OpenType::Kern::Format0> : public DefaultTraits<OpenType::Kern::Format0> {
    static constexpr bool is_trivially_serializable() { return true; }
};
template<>
struct Traits<OpenType::Kern::Format0Pair> : public DefaultTraits<OpenType::Kern::Format0Pair> {
    static constexpr bool is_trivially_serializable() { return true; }
};

template<>
struct Traits<OpenType::GPOS::Version1_0> : public DefaultTraits<OpenType::GPOS::Version1_0> {
    static constexpr bool is_trivially_serializable() { return true; }
};

template<>
struct Traits<OpenType::FeatureList> : public DefaultTraits<OpenType::FeatureList> {
    static constexpr bool is_trivially_serializable() { return true; }
};
template<>
struct Traits<OpenType::FeatureRecord> : public DefaultTraits<OpenType::FeatureRecord> {
    static constexpr bool is_trivially_serializable() { return true; }
};

template<>
struct Traits<OpenType::LookupList> : public DefaultTraits<OpenType::LookupList> {
    static constexpr bool is_trivially_serializable() { return true; }
};

template<>
struct Traits<OpenType::ScriptList> : public DefaultTraits<OpenType::ScriptList> {
    static constexpr bool is_trivially_serializable() { return true; }
};
template<>
struct Traits<OpenType::ScriptRecord> : public DefaultTraits<OpenType::ScriptRecord> {
    static constexpr bool is_trivially_serializable() { return true; }
};
}
