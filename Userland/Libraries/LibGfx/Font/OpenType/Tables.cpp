/*
 * Copyright (c) 2020, Srimanta Barua <srimanta.barua1@gmail.com>
 * Copyright (c) 2021-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Jelle Raaijmakers <jelle@gmta.nl>
 * Copyright (c) 2023, Lukas Affolter <git@lukasach.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Debug.h>
#include <AK/MemoryStream.h>
#include <LibGfx/Font/OpenType/Tables.h>
#include <LibTextCodec/Decoder.h>

namespace OpenType {

ErrorOr<Head> Head::from_slice(ReadonlyBytes slice)
{
    if (slice.size() < sizeof(FontHeaderTable))
        return Error::from_string_literal("Could not load Head: Not enough data");

    auto const& font_header_table = *bit_cast<FontHeaderTable const*>(slice.data());

    static constexpr u32 HEADER_TABLE_MAGIC_NUMBER = 0x5F0F3CF5;
    if (font_header_table.major_version != 1)
        return Error::from_string_literal("Unknown major version. Expected 1");
    if (font_header_table.minor_version != 0)
        return Error::from_string_literal("Unknown minor version. Expected 0");
    if (font_header_table.magic_number != HEADER_TABLE_MAGIC_NUMBER)
        return Error::from_string_literal("Invalid magic number");
    if (font_header_table.index_to_loc_format != 0 && font_header_table.index_to_loc_format != 1)
        return Error::from_string_literal("Invalid IndexToLocFormat value");

    return Head(font_header_table);
}

u16 Head::units_per_em() const
{
    return m_data.units_per_em;
}

i16 Head::xmin() const
{
    return m_data.x_min;
}

i16 Head::ymin() const
{
    return m_data.y_min;
}

i16 Head::xmax() const
{
    return m_data.x_max;
}

i16 Head::ymax() const
{
    return m_data.y_max;
}

u16 Head::style() const
{
    return m_data.mac_style;
}

u16 Head::lowest_recommended_ppem() const
{
    return m_data.lowest_rec_ppem;
}

IndexToLocFormat Head::index_to_loc_format() const
{
    switch (m_data.index_to_loc_format) {
    case 0:
        return IndexToLocFormat::Offset16;
    case 1:
        return IndexToLocFormat::Offset32;
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<Hhea> Hhea::from_slice(ReadonlyBytes slice)
{
    if (slice.size() < sizeof(HorizontalHeaderTable))
        return Error::from_string_literal("Could not load Hhea: Not enough data");

    auto const& horizontal_header_table = *bit_cast<HorizontalHeaderTable const*>(slice.data());
    return Hhea(horizontal_header_table);
}

i16 Hhea::ascender() const
{
    return m_data.ascender;
}

i16 Hhea::descender() const
{
    return m_data.descender;
}

i16 Hhea::line_gap() const
{
    return m_data.line_gap;
}

u16 Hhea::advance_width_max() const
{
    return m_data.advance_width_max;
}

u16 Hhea::number_of_h_metrics() const
{
    return m_data.number_of_h_metrics;
}

ErrorOr<Maxp> Maxp::from_slice(ReadonlyBytes slice)
{
    // All Maximum Profile tables begin with a version.
    if (slice.size() < sizeof(Version16Dot16))
        return Error::from_string_literal("Could not load Maxp: Not enough data");
    Version16Dot16 const& version = *bit_cast<Version16Dot16 const*>(slice.data());

    if (version.major == 0 && version.minor == 5) {
        if (slice.size() < sizeof(Version0_5))
            return Error::from_string_literal("Could not load Maxp: Not enough data");
        return Maxp(bit_cast<Version0_5 const*>(slice.data()));
    }

    if (version.major == 1 && version.minor == 0) {
        if (slice.size() < sizeof(Version1_0))
            return Error::from_string_literal("Could not load Maxp: Not enough data");
        return Maxp(bit_cast<Version1_0 const*>(slice.data()));
    }

    return Error::from_string_literal("Could not load Maxp: Unrecognized version");
}

u16 Maxp::num_glyphs() const
{
    return m_data.visit([](auto const* any) { return any->num_glyphs; });
}

ErrorOr<Hmtx> Hmtx::from_slice(ReadonlyBytes slice, u32 num_glyphs, u32 number_of_h_metrics)
{
    if (slice.size() < number_of_h_metrics * sizeof(LongHorMetric) + (num_glyphs - number_of_h_metrics) * sizeof(Int16))
        return Error::from_string_literal("Could not load Hmtx: Not enough data");

    // The Horizontal Metrics table is LongHorMetric[number_of_h_metrics] followed by Int16[num_glyphs - number_of_h_metrics];
    ReadonlySpan<LongHorMetric> long_hor_metrics { bit_cast<LongHorMetric*>(slice.data()), number_of_h_metrics };
    ReadonlySpan<Int16> left_side_bearings {};
    auto number_of_left_side_bearings = num_glyphs - number_of_h_metrics;
    if (number_of_left_side_bearings > 0) {
        left_side_bearings = {
            bit_cast<Int16*>(slice.offset(number_of_h_metrics * sizeof(LongHorMetric))),
            number_of_left_side_bearings
        };
    }
    return Hmtx(long_hor_metrics, left_side_bearings);
}

GlyphHorizontalMetrics Hmtx::get_glyph_horizontal_metrics(u32 glyph_id) const
{
    VERIFY(glyph_id < m_long_hor_metrics.size() + m_left_side_bearings.size());
    if (glyph_id < m_long_hor_metrics.size()) {
        return GlyphHorizontalMetrics {
            .advance_width = m_long_hor_metrics[glyph_id].advance_width,
            .left_side_bearing = m_long_hor_metrics[glyph_id].lsb,
        };
    }

    return GlyphHorizontalMetrics {
        .advance_width = m_long_hor_metrics.last().advance_width,
        .left_side_bearing = m_left_side_bearings[glyph_id - m_long_hor_metrics.size()],
    };
}

ErrorOr<Name> Name::from_slice(ReadonlyBytes slice)
{
    // FIXME: Support version 1 table too.

    if (slice.size() < sizeof(NamingTableVersion0))
        return Error::from_string_literal("Could not load Name: Not enough data");

    auto& naming_table = *bit_cast<NamingTableVersion0 const*>(slice.data());

    auto name_record_data_size = naming_table.count * sizeof(NameRecord);
    if (slice.size() < sizeof(NamingTableVersion0) + name_record_data_size)
        return Error::from_string_literal("Could not load Name: Not enough data");
    ReadonlySpan<NameRecord> name_records = { bit_cast<NameRecord const*>(slice.offset_pointer(sizeof(NamingTableVersion0))), naming_table.count };

    if (slice.size() < naming_table.storage_offset)
        return Error::from_string_literal("Could not load Name: Not enough data");
    ReadonlyBytes string_data = slice.slice(naming_table.storage_offset);

    return Name(naming_table, name_records, string_data);
}

String Name::string_for_id(NameId id) const
{
    auto const count = m_naming_table.count;

    Vector<int> valid_ids;

    for (size_t i = 0; i < count; ++i) {
        auto this_id = m_name_records[i].name_id;
        if (this_id == to_underlying(id))
            valid_ids.append(i);
    }

    if (valid_ids.is_empty())
        return String {};

    auto it = valid_ids.find_if([this](auto const& i) {
        // check if font has naming table for en-US language id
        auto const& name_record = m_name_records[i];
        auto const platform_id = name_record.platform_id;
        auto const language_id = name_record.language_id;
        return (platform_id == to_underlying(Platform::Macintosh) && language_id == to_underlying(MacintoshLanguage::English))
            || (platform_id == to_underlying(Platform::Windows) && language_id == to_underlying(WindowsLanguage::EnglishUnitedStates));
    });
    auto i = it != valid_ids.end() ? *it : valid_ids.first();

    auto const& name_record = m_name_records[i];

    auto const platform_id = name_record.platform_id;
    auto const length = name_record.length;
    auto const offset = name_record.string_offset;
    auto const name_bytes = m_string_data.slice(offset, length);

    if (platform_id == to_underlying(Platform::Windows)) {
        static auto& decoder = *TextCodec::decoder_for("utf-16be"sv);
        return decoder.to_utf8(name_bytes).release_value_but_fixme_should_propagate_errors();
    }

    auto maybe_name = String::from_utf8(name_bytes);
    if (maybe_name.is_error()) {
        static auto& decoder = *TextCodec::decoder_for("utf-16be"sv);
        maybe_name = decoder.to_utf8(name_bytes);
        if (!maybe_name.is_error())
            return maybe_name.release_value_but_fixme_should_propagate_errors();
        dbgln("OpenType::Name: Failed to decode name string as UTF-8 or UTF-16BE");
        return String {};
    }
    return maybe_name.release_value();
}

ErrorOr<Kern> Kern::from_slice(ReadonlyBytes slice)
{
    FixedMemoryStream stream { slice };

    // We only support the old (2x u16) version of the header
    auto const& header = *TRY(stream.read_in_place<Header const>());
    auto version = header.version;
    auto number_of_subtables = header.n_tables;
    if (version != 0)
        return Error::from_string_literal("Unsupported kern table version");
    if (number_of_subtables == 0)
        return Error::from_string_literal("Kern table does not contain any subtables");

    // Read subtables
    Vector<Subtable> subtables;
    TRY(subtables.try_ensure_capacity(number_of_subtables));
    for (size_t i = 0; i < number_of_subtables; ++i) {
        auto const& subtable_header = *TRY(stream.read_in_place<SubtableHeader const>());

        if (subtable_header.version != 0)
            return Error::from_string_literal("Unsupported Kern subtable version");

        if (stream.remaining() + sizeof(SubtableHeader) < subtable_header.length)
            return Error::from_string_literal("Kern subtable is truncated");

        auto subtable_format = (subtable_header.coverage & 0xFF00) >> 8;
        if (subtable_format == 0) {
            auto const& format0_header = *TRY(stream.read_in_place<Format0 const>());
            auto pairs = TRY(stream.read_in_place<Format0Pair const>(format0_header.n_pairs));

            subtables.append(Subtable {
                .header = subtable_header,
                .table = Format0Table {
                    .header = format0_header,
                    .pairs = pairs,
                },
            });
        } else {
            dbgln("OpenType::Kern: FIXME: subtable format {} is unsupported", subtable_format);
            TRY(stream.discard(subtable_header.length - sizeof(SubtableHeader)));
            subtables.append(Subtable {
                .header = subtable_header,
                .table = UnsupportedTable {},
            });
        }
    }

    return Kern(header, move(subtables));
}

i16 Kern::get_glyph_kerning(u16 left_glyph_id, u16 right_glyph_id) const
{
    VERIFY(left_glyph_id > 0 && right_glyph_id > 0);

    i16 glyph_kerning = 0;
    for (auto const& subtable : m_subtables) {
        auto coverage = subtable.header.coverage;

        auto is_horizontal = (coverage & (1 << 0)) > 0;
        auto is_minimum = (coverage & (1 << 1)) > 0;
        auto is_cross_stream = (coverage & (1 << 2)) > 0;
        auto is_override = (coverage & (1 << 3)) > 0;
        auto reserved_bits = (coverage & 0xF0);

        // FIXME: implement support for these features
        if (!is_horizontal || is_minimum || is_cross_stream || (reserved_bits > 0)) {
            dbgln("OpenType::Kern: FIXME: implement missing feature support for subtable");
            continue;
        }

        // FIXME: implement support for subtable formats other than 0
        Optional<i16> subtable_kerning;
        subtable.table.visit(
            [&](Format0Table const& format0) {
                subtable_kerning = read_glyph_kerning_format0(format0, left_glyph_id, right_glyph_id);
            },
            [&](auto&) {});

        if (!subtable_kerning.has_value())
            continue;
        auto kerning_value = subtable_kerning.release_value();

        if (is_override)
            glyph_kerning = kerning_value;
        else
            glyph_kerning += kerning_value;
    }
    return glyph_kerning;
}

Optional<i16> Kern::read_glyph_kerning_format0(Format0Table const& format0, u16 left_glyph_id, u16 right_glyph_id)
{
    u16 number_of_pairs = format0.header.n_pairs;
    u16 search_range = format0.header.search_range;
    u16 entry_selector = format0.header.entry_selector;
    u16 range_shift = format0.header.range_shift;

    // Sanity checks for this table format
    auto pairs_in_search_range = search_range / sizeof(Format0Pair);
    if (number_of_pairs == 0)
        return {};
    if (pairs_in_search_range > number_of_pairs)
        return {};
    if ((1 << entry_selector) * sizeof(Format0Pair) != search_range)
        return {};
    if ((number_of_pairs - pairs_in_search_range) * sizeof(Format0Pair) != range_shift)
        return {};

    // FIXME: implement a possibly slightly more efficient binary search using the parameters above

    // The left and right halves of the kerning pair make an unsigned 32-bit number, which is then used to order the kerning pairs numerically.
    auto needle = (static_cast<u32>(left_glyph_id) << 16u) | static_cast<u32>(right_glyph_id);
    auto* pair = binary_search(format0.pairs, nullptr, nullptr, [&](void*, Format0Pair const& pair) {
        auto as_u32 = (static_cast<u32>(pair.left) << 16u) | static_cast<u32>(pair.right);
        return needle - as_u32;
    });

    if (!pair)
        return 0;
    return pair->value;
}

ErrorOr<OS2> OS2::from_slice(ReadonlyBytes slice)
{
    // All OS2 tables begin with a version.
    if (slice.size() < sizeof(BigEndian<u16>))
        return Error::from_string_literal("Could not load OS2: Not enough data");
    u16 version = *bit_cast<BigEndian<u16> const*>(slice.data());

    // NOTE: We assume that this table only ever has new fields added to the end in future versions.
    switch (version) {
    case 0: {
        if (slice.size() < sizeof(Version0))
            return Error::from_string_literal("Could not load OS2 v0: Not enough data");
        return OS2(bit_cast<Version0 const*>(slice.data()));
    }
    case 1: {
        if (slice.size() < sizeof(Version1))
            return Error::from_string_literal("Could not load OS2 v1: Not enough data");
        return OS2(bit_cast<Version1 const*>(slice.data()));
    }
    case 2:
    default: {
        if (slice.size() < sizeof(Version2))
            return Error::from_string_literal("Could not load OS2 v2: Not enough data");
        return OS2(bit_cast<Version2 const*>(slice.data()));
    }
    }
}

u16 OS2::weight_class() const
{
    return m_data.visit([](auto* any) { return any->us_weight_class; });
}

u16 OS2::width_class() const
{
    return m_data.visit([](auto* any) { return any->us_width_class; });
}

u16 OS2::selection() const
{
    return m_data.visit([](auto* any) { return any->fs_selection; });
}

i16 OS2::typographic_ascender() const
{
    return m_data.visit([](auto* any) { return any->s_typo_ascender; });
}

i16 OS2::typographic_descender() const
{
    return m_data.visit([](auto* any) { return any->s_typo_descender; });
}

i16 OS2::typographic_line_gap() const
{
    return m_data.visit([](auto* any) { return any->s_typo_line_gap; });
}

bool OS2::use_typographic_metrics() const
{
    return m_data.visit([](auto* any) { return any->fs_selection & 0x80; });
}

Optional<i16> OS2::x_height() const
{
    return m_data.visit(
        []<typename T>
        requires(requires { T::sx_height; })(T * data) -> Optional<i16> {
            return data->sx_height;
        },
        [](auto*) { return Optional<i16>(); });
}

ErrorOr<CBLC> CBLC::from_slice(ReadonlyBytes slice)
{
    if (slice.size() < sizeof(CblcHeader))
        return Error::from_string_literal("CBLC table too small");
    auto const& header = *bit_cast<CblcHeader const*>(slice.data());

    size_t num_sizes = header.num_sizes;
    Checked<size_t> size_used_by_bitmap_sizes = num_sizes;
    size_used_by_bitmap_sizes *= sizeof(BitmapSize);
    if (size_used_by_bitmap_sizes.has_overflow())
        return Error::from_string_literal("Integer overflow in CBLC table");

    Checked<size_t> total_size = sizeof(CblcHeader);
    total_size += size_used_by_bitmap_sizes;
    if (total_size.has_overflow())
        return Error::from_string_literal("Integer overflow in CBLC table");

    if (slice.size() < total_size)
        return Error::from_string_literal("CBLC table too small");

    ReadonlySpan<BitmapSize> bitmap_sizes { bit_cast<BitmapSize const*>(slice.data() + sizeof(CblcHeader)), num_sizes };

    return CBLC { slice, header, bitmap_sizes };
}

Optional<CBLC::BitmapSize const&> CBLC::bitmap_size_for_glyph_id(u32 glyph_id) const
{
    for (auto const& bitmap_size : m_bitmap_sizes) {
        if (glyph_id >= bitmap_size.start_glyph_index && glyph_id <= bitmap_size.end_glyph_index) {
            return bitmap_size;
        }
    }
    return {};
}

Optional<EBLC::IndexSubHeader const&> CBLC::index_subtable_for_glyph_id(u32 glyph_id, u16& first_glyph_index, u16& last_glyph_index) const
{
    auto maybe_bitmap_size = bitmap_size_for_glyph_id(glyph_id);
    if (!maybe_bitmap_size.has_value()) {
        return {};
    }
    auto const& bitmap_size = maybe_bitmap_size.value();

    Checked<size_t> required_size = static_cast<u32>(bitmap_size.index_subtable_array_offset);
    required_size += bitmap_size.index_tables_size;

    if (m_slice.size() < required_size) {
        dbgln("CBLC index subtable array goes out of bounds");
        return {};
    }

    auto index_subtables_slice = m_slice.slice(bitmap_size.index_subtable_array_offset, bitmap_size.index_tables_size);
    ReadonlySpan<EBLC::IndexSubTableArray> index_subtable_arrays {
        bit_cast<EBLC::IndexSubTableArray const*>(index_subtables_slice.data()), bitmap_size.number_of_index_subtables
    };

    EBLC::IndexSubTableArray const* index_subtable_array = nullptr;
    for (auto const& array : index_subtable_arrays) {
        if (glyph_id >= array.first_glyph_index && glyph_id <= array.last_glyph_index)
            index_subtable_array = &array;
    }
    if (!index_subtable_array) {
        return {};
    }

    auto index_subtable_slice = m_slice.slice(bitmap_size.index_subtable_array_offset + index_subtable_array->additional_offset_to_index_subtable);
    first_glyph_index = index_subtable_array->first_glyph_index;
    last_glyph_index = index_subtable_array->last_glyph_index;
    return *bit_cast<EBLC::IndexSubHeader const*>(index_subtable_slice.data());
}

ErrorOr<CBDT> CBDT::from_slice(ReadonlyBytes slice)
{
    if (slice.size() < sizeof(CbdtHeader))
        return Error::from_string_literal("CBDT table too small");
    auto const& header = *bit_cast<CbdtHeader*>(slice.data());
    return CBDT { slice, header };
}

ErrorOr<GPOS> GPOS::from_slice(ReadonlyBytes slice)
{
    FixedMemoryStream stream { slice };
    auto const& header = *TRY(stream.read_in_place<Version1_0 const>());
    // FIXME: Detect version 1.1 and support the extra FeatureVariations table.

    TRY(stream.seek(header.script_list_offset, SeekMode::SetPosition));
    auto const& script_list = *TRY(stream.read_in_place<ScriptList const>());
    auto script_records = TRY(stream.read_in_place<ScriptRecord const>(script_list.script_count));

    TRY(stream.seek(header.feature_list_offset, SeekMode::SetPosition));
    auto const& feature_list = *TRY(stream.read_in_place<FeatureList const>());
    auto feature_records = TRY(stream.read_in_place<FeatureRecord const>(feature_list.feature_count));

    TRY(stream.seek(header.lookup_list_offset, SeekMode::SetPosition));
    auto const& lookup_list = *TRY(stream.read_in_place<LookupList const>());
    auto lookup_records = TRY(stream.read_in_place<Offset16 const>(lookup_list.lookup_count));

    return GPOS { slice, header, script_list, script_records, feature_list, feature_records, lookup_list, lookup_records };
}

Optional<i16> GPOS::glyph_kerning(u16 left_glyph_id, u16 right_glyph_id) const
{
    auto read_value_record = [&](u16 value_format, FixedMemoryStream& stream) -> ValueRecord {
        ValueRecord value_record;
        if (value_format & to_underlying(ValueFormat::X_PLACEMENT))
            value_record.x_placement = stream.read_value<BigEndian<i16>>().release_value_but_fixme_should_propagate_errors();
        if (value_format & to_underlying(ValueFormat::Y_PLACEMENT))
            value_record.y_placement = stream.read_value<BigEndian<i16>>().release_value_but_fixme_should_propagate_errors();
        if (value_format & to_underlying(ValueFormat::X_ADVANCE))
            value_record.x_advance = stream.read_value<BigEndian<i16>>().release_value_but_fixme_should_propagate_errors();
        if (value_format & to_underlying(ValueFormat::Y_ADVANCE))
            value_record.y_advance = stream.read_value<BigEndian<i16>>().release_value_but_fixme_should_propagate_errors();
        if (value_format & to_underlying(ValueFormat::X_PLACEMENT_DEVICE))
            value_record.x_placement_device_offset = stream.read_value<Offset16>().release_value_but_fixme_should_propagate_errors();
        if (value_format & to_underlying(ValueFormat::Y_PLACEMENT_DEVICE))
            value_record.y_placement_device_offset = stream.read_value<Offset16>().release_value_but_fixme_should_propagate_errors();
        if (value_format & to_underlying(ValueFormat::X_ADVANCE_DEVICE))
            value_record.x_advance_device_offset = stream.read_value<Offset16>().release_value_but_fixme_should_propagate_errors();
        if (value_format & to_underlying(ValueFormat::Y_ADVANCE_DEVICE))
            value_record.y_advance_device_offset = stream.read_value<Offset16>().release_value_but_fixme_should_propagate_errors();
        return value_record;
    };

    dbgln_if(OPENTYPE_GPOS_DEBUG, "GPOS header:");
    dbgln_if(OPENTYPE_GPOS_DEBUG, "   Version: {}.{}", m_header.major_version, m_header.minor_version);
    dbgln_if(OPENTYPE_GPOS_DEBUG, "   Feature list offset: {}", m_header.feature_list_offset);

    // FIXME: Make sure everything is bounds-checked appropriately.

    auto feature_list_slice = m_slice.slice(m_header.feature_list_offset);
    auto lookup_list_slice = m_slice.slice(m_header.lookup_list_offset);

    Optional<Offset16> kern_feature_offset;
    for (auto const& feature_record : m_feature_records) {
        if (feature_record.feature_tag == Tag("kern")) {
            kern_feature_offset = feature_record.feature_offset;
            break;
        }
    }

    if (!kern_feature_offset.has_value()) {
        dbgln_if(OPENTYPE_GPOS_DEBUG, "No 'kern' feature found in GPOS table");
        return {};
    }

    auto feature_slice = feature_list_slice.slice(kern_feature_offset.value());
    auto const& feature = *bit_cast<Feature const*>(feature_slice.data());

    dbgln_if(OPENTYPE_GPOS_DEBUG, "Feature:");
    dbgln_if(OPENTYPE_GPOS_DEBUG, "   featureParamsOffset: {}", feature.feature_params_offset);
    dbgln_if(OPENTYPE_GPOS_DEBUG, "   lookupIndexCount: {}", feature.lookup_index_count);

    for (size_t i = 0; i < feature.lookup_index_count; ++i) {
        auto lookup_index = feature.lookup_list_indices[i];
        dbgln_if(OPENTYPE_GPOS_DEBUG, "Lookup index: {}", lookup_index);
        auto lookup_slice = lookup_list_slice.slice(m_lookup_offsets[lookup_index]);
        auto const& lookup = *bit_cast<Lookup const*>(lookup_slice.data());

        dbgln_if(OPENTYPE_GPOS_DEBUG, "Lookup:");
        dbgln_if(OPENTYPE_GPOS_DEBUG, "  lookupType: {}", lookup.lookup_type);
        dbgln_if(OPENTYPE_GPOS_DEBUG, "  lookupFlag: {}", lookup.lookup_flag);
        dbgln_if(OPENTYPE_GPOS_DEBUG, "  subtableCount: {}", lookup.subtable_count);

        // NOTE: We only support lookup type 2 (Pair adjustment) at the moment.
        if (lookup.lookup_type != 2) {
            dbgln_if(OPENTYPE_GPOS_DEBUG, "FIXME: Implement GPOS lookup type {}", lookup.lookup_type);
            continue;
        }

        for (size_t j = 0; j < lookup.subtable_count; ++j) {
            auto pair_pos_format_offset = lookup.subtable_offsets[j];
            auto pair_pos_format_slice = lookup_slice.slice(pair_pos_format_offset);

            auto const& pair_pos_format = *bit_cast<BigEndian<u16> const*>(pair_pos_format_slice.data());

            dbgln_if(OPENTYPE_GPOS_DEBUG, "PairPosFormat{}", pair_pos_format);

            if (pair_pos_format == 1) {
                auto const& pair_pos_format1 = *bit_cast<GPOS::PairPosFormat1 const*>(pair_pos_format_slice.data());

                dbgln_if(OPENTYPE_GPOS_DEBUG, "   posFormat: {}", pair_pos_format1.pos_format);
                dbgln_if(OPENTYPE_GPOS_DEBUG, "   valueFormat1: {}", pair_pos_format1.value_format1);
                dbgln_if(OPENTYPE_GPOS_DEBUG, "   valueFormat2: {}", pair_pos_format1.value_format2);
                dbgln_if(OPENTYPE_GPOS_DEBUG, "   pairSetCount: {}", pair_pos_format1.pair_set_count);

                auto get_coverage_index = [&](u16 glyph_id, Offset16 coverage_format_offset) -> Optional<u16> {
                    auto coverage_format_slice = pair_pos_format_slice.slice(coverage_format_offset);
                    auto const& coverage_format = *bit_cast<BigEndian<u16> const*>(coverage_format_slice.data());

                    dbgln_if(OPENTYPE_GPOS_DEBUG, "Coverage table format: {}", coverage_format);

                    if (coverage_format == 1) {
                        auto const& coverage_format1 = *bit_cast<CoverageFormat1 const*>(coverage_format_slice.data());

                        for (size_t k = 0; k < coverage_format1.glyph_count; ++k)
                            if (coverage_format1.glyph_array[k] == glyph_id)
                                return k;

                        dbgln_if(OPENTYPE_GPOS_DEBUG, "Glyph ID {} not covered", glyph_id);
                        return {};
                    }

                    else if (coverage_format == 2) {
                        auto const& coverage_format2 = *bit_cast<CoverageFormat2 const*>(coverage_format_slice.data());

                        for (size_t k = 0; k < coverage_format2.range_count; ++k) {
                            auto range_record = coverage_format2.range_records[k];
                            if ((range_record.start_glyph_id <= glyph_id) && (glyph_id <= range_record.end_glyph_id))
                                return range_record.start_coverage_index + glyph_id - range_record.start_glyph_id;
                        }
                        dbgln_if(OPENTYPE_GPOS_DEBUG, "Glyph ID {} not covered", glyph_id);
                        return {};
                    }

                    dbgln_if(OPENTYPE_GPOS_DEBUG, "No valid coverage table for format {}", coverage_format);
                    return {};
                };

                auto coverage_index = get_coverage_index(left_glyph_id, pair_pos_format1.coverage_offset);

                if (!coverage_index.has_value()) {
                    dbgln_if(OPENTYPE_GPOS_DEBUG, "Glyph ID not covered by table");
                    continue;
                }

                size_t value1_size = popcount(static_cast<u32>(pair_pos_format1.value_format1 & 0xff)) * sizeof(u16);
                size_t value2_size = popcount(static_cast<u32>(pair_pos_format1.value_format2 & 0xff)) * sizeof(u16);
                dbgln_if(OPENTYPE_GPOS_DEBUG, "ValueSizes: {}, {}", value1_size, value2_size);

                // Manually iterate over the PairSet table, as the size of each PairValueRecord is not known at compile time.
                auto pair_set_offset = pair_pos_format1.pair_set_offsets[coverage_index.value()];
                auto pair_set_slice = pair_pos_format_slice.slice(pair_set_offset);

                FixedMemoryStream stream(pair_set_slice);

                auto pair_value_count = stream.read_value<BigEndian<u16>>().release_value_but_fixme_should_propagate_errors();

                bool found_matching_glyph = false;
                for (size_t k = 0; k < pair_value_count; ++k) {
                    auto second_glyph = stream.read_value<BigEndian<u16>>().release_value_but_fixme_should_propagate_errors();

                    if (right_glyph_id == second_glyph) {
                        dbgln_if(OPENTYPE_GPOS_DEBUG, "Found matching second glyph {}", second_glyph);
                        found_matching_glyph = true;
                        break;
                    }

                    (void)stream.discard(value1_size + value2_size).release_value_but_fixme_should_propagate_errors();
                }

                if (!found_matching_glyph) {
                    dbgln_if(OPENTYPE_GPOS_DEBUG, "Did not find second glyph matching {}", right_glyph_id);
                    continue;
                }

                [[maybe_unused]] auto value_record1 = read_value_record(pair_pos_format1.value_format1, stream);
                [[maybe_unused]] auto value_record2 = read_value_record(pair_pos_format1.value_format2, stream);

                dbgln_if(OPENTYPE_GPOS_DEBUG, "Returning x advance {}", value_record1.x_advance);
                return value_record1.x_advance;
            }

            else if (pair_pos_format == 2) {
                auto const& pair_pos_format2 = *bit_cast<GPOS::PairPosFormat2 const*>(pair_pos_format_slice.data());

                dbgln_if(OPENTYPE_GPOS_DEBUG, "   posFormat: {}", pair_pos_format2.pos_format);
                dbgln_if(OPENTYPE_GPOS_DEBUG, "   valueFormat1: {}", pair_pos_format2.value_format1);
                dbgln_if(OPENTYPE_GPOS_DEBUG, "   valueFormat2: {}", pair_pos_format2.value_format2);
                dbgln_if(OPENTYPE_GPOS_DEBUG, "   class1Count: {}", pair_pos_format2.class1_count);
                dbgln_if(OPENTYPE_GPOS_DEBUG, "   class2Count: {}", pair_pos_format2.class2_count);

                auto get_class = [&](u16 glyph_id, Offset16 glyph_def_offset) -> Optional<u16> {
                    auto class_def_format_slice = pair_pos_format_slice.slice(glyph_def_offset);

                    auto const& class_def_format = *bit_cast<BigEndian<u16> const*>(class_def_format_slice.data());
                    if (class_def_format == 1) {
                        dbgln_if(OPENTYPE_GPOS_DEBUG, "FIXME: Implement ClassDefFormat1");
                        return {};
                    }

                    auto const& class_def_format2 = *bit_cast<ClassDefFormat2 const*>(class_def_format_slice.data());
                    dbgln_if(OPENTYPE_GPOS_DEBUG, "ClassDefFormat2:");
                    dbgln_if(OPENTYPE_GPOS_DEBUG, "  classFormat: {}", class_def_format2.class_format);
                    dbgln_if(OPENTYPE_GPOS_DEBUG, "  classRangeCount: {}", class_def_format2.class_range_count);

                    for (size_t i = 0; i < class_def_format2.class_range_count; ++i) {
                        auto const& range = class_def_format2.class_range_records[i];
                        if (glyph_id >= range.start_glyph_id && glyph_id <= range.end_glyph_id) {
                            dbgln_if(OPENTYPE_GPOS_DEBUG, "Found class {} for glyph ID {}", range.class_, glyph_id);
                            return range.class_;
                        }
                    }

                    dbgln_if(OPENTYPE_GPOS_DEBUG, "No class found for glyph {}", glyph_id);
                    return {};
                };

                auto left_class = get_class(left_glyph_id, pair_pos_format2.class_def1_offset);
                auto right_class = get_class(right_glyph_id, pair_pos_format2.class_def2_offset);

                if (!left_class.has_value() || !right_class.has_value()) {
                    dbgln_if(OPENTYPE_GPOS_DEBUG, "Need glyph class for both sides");
                    continue;
                }

                dbgln_if(OPENTYPE_GPOS_DEBUG, "Classes: {}, {}", left_class.value(), right_class.value());

                size_t value1_size = popcount(static_cast<u32>(pair_pos_format2.value_format1 & 0xff)) * sizeof(u16);
                size_t value2_size = popcount(static_cast<u32>(pair_pos_format2.value_format2 & 0xff)) * sizeof(u16);
                dbgln_if(OPENTYPE_GPOS_DEBUG, "ValueSizes: {}, {}", value1_size, value2_size);
                size_t class2_record_size = value1_size + value2_size;
                dbgln_if(OPENTYPE_GPOS_DEBUG, "Class2RecordSize: {}", class2_record_size);
                size_t class1_record_size = pair_pos_format2.class2_count * class2_record_size;
                dbgln_if(OPENTYPE_GPOS_DEBUG, "Class1RecordSize: {}", class1_record_size);
                size_t item_offset = (left_class.value() * class1_record_size) + (right_class.value() * class2_record_size);
                dbgln_if(OPENTYPE_GPOS_DEBUG, "Item offset: {}", item_offset);

                auto item_slice = pair_pos_format_slice.slice(sizeof(PairPosFormat2) + item_offset);
                FixedMemoryStream stream(item_slice);

                [[maybe_unused]] auto value_record1 = read_value_record(pair_pos_format2.value_format1, stream);
                [[maybe_unused]] auto value_record2 = read_value_record(pair_pos_format2.value_format2, stream);

                dbgln_if(OPENTYPE_GPOS_DEBUG, "Returning x advance {}", value_record1.x_advance);
                return value_record1.x_advance;
            }
        }
    }

    (void)left_glyph_id;
    (void)right_glyph_id;
    return {};
}

}
