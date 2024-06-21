/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/AllOf.h>
#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/CharacterTypes.h>
#include <AK/Error.h>
#include <AK/Find.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/QuickSort.h>
#include <AK/SourceGenerator.h>
#include <AK/StringUtils.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibUnicode/CharacterTypes.h>

// https://www.unicode.org/reports/tr44/#PropList.txt
using PropList = HashMap<ByteString, Vector<Unicode::CodePointRange>>;

// https://www.unicode.org/reports/tr44/#UnicodeData.txt
struct CodePointData {
    u32 code_point { 0 };
    ByteString name;
    ByteString bidi_class;
    Optional<i8> numeric_value_decimal;
    Optional<i8> numeric_value_digit;
    Optional<i8> numeric_value_numeric;
    bool bidi_mirrored { false };
    ByteString unicode_1_name;
    ByteString iso_comment;
};

using PropertyTable = Vector<bool>;

static constexpr auto CODE_POINT_TABLES_MSB_COUNT = 16u;
static_assert(CODE_POINT_TABLES_MSB_COUNT < 24u);

static constexpr auto CODE_POINT_TABLES_LSB_COUNT = 24u - CODE_POINT_TABLES_MSB_COUNT;
static constexpr auto CODE_POINT_TABLES_LSB_MASK = NumericLimits<u32>::max() >> (NumericLimits<u32>::digits() - CODE_POINT_TABLES_LSB_COUNT);

template<typename PropertyType>
struct CodePointTables {
    Vector<size_t> stage1;
    Vector<size_t> stage2;
    Vector<PropertyType> unique_properties;
};

struct CodePointBidiClass {
    Unicode::CodePointRange code_point_range;
    ByteString bidi_class;
};

struct UnicodeData {
    Vector<CodePointData> code_point_data;

    // https://www.unicode.org/reports/tr44/#General_Category_Values
    PropList general_categories;
    Vector<Alias> general_category_aliases;

    PropList script_list {
        { "Unknown"sv, {} },
    };
    Vector<Alias> script_aliases;
    PropList script_extensions;

    CodePointTables<PropertyTable> general_category_tables;
    CodePointTables<PropertyTable> script_tables;
    CodePointTables<PropertyTable> script_extension_tables;

    HashTable<ByteString> bidirectional_classes;
    Vector<CodePointBidiClass> code_point_bidirectional_classes;
};

static ByteString sanitize_entry(ByteString const& entry)
{
    auto sanitized = entry.replace("-"sv, "_"sv, ReplaceMode::All);
    sanitized = sanitized.replace(" "sv, "_"sv, ReplaceMode::All);

    StringBuilder builder;
    bool next_is_upper = true;
    for (auto ch : sanitized) {
        if (next_is_upper)
            builder.append_code_point(to_ascii_uppercase(ch));
        else
            builder.append_code_point(ch);
        next_is_upper = ch == '_';
    }

    return builder.to_byte_string();
}

static ErrorOr<void> parse_prop_list(Core::InputBufferedFile& file, PropList& prop_list, bool multi_value_property = false, bool sanitize_property = false)
{
    Array<u8, 1024> buffer;

    while (TRY(file.can_read_line())) {
        auto line = TRY(file.read_line(buffer));

        if (line.is_empty() || line.starts_with('#'))
            continue;

        if (auto index = line.find('#'); index.has_value())
            line = line.substring_view(0, *index);

        auto segments = line.split_view(';', SplitBehavior::KeepEmpty);
        VERIFY(segments.size() == 2 || segments.size() == 3);

        String combined_segment_buffer;

        if (segments.size() == 3) {
            // For example, in DerivedCoreProperties.txt, there are lines such as:
            //
            //     094D          ; InCB; Linker # Mn       DEVANAGARI SIGN VIRAMA
            //
            // These are used in text segmentation to prevent breaking within some extended grapheme clusters.
            // So here, we combine the segments into a single property, which allows us to simply do code point
            // property lookups at runtime for specific Indic Conjunct Break sequences.
            combined_segment_buffer = MUST(String::join('_', Array { segments[1].trim_whitespace(), segments[2].trim_whitespace() }));
            segments[1] = combined_segment_buffer;
        }

        auto code_point_range = parse_code_point_range(segments[0].trim_whitespace());
        Vector<StringView> properties;

        if (multi_value_property)
            properties = segments[1].trim_whitespace().split_view(' ');
        else
            properties = { segments[1].trim_whitespace() };

        for (auto& property : properties) {
            auto& code_points = prop_list.ensure(sanitize_property ? sanitize_entry(property).trim_whitespace() : ByteString { property.trim_whitespace() });
            code_points.append(code_point_range);
        }
    }

    return {};
}

static ErrorOr<void> parse_value_alias_list(Core::InputBufferedFile& file, StringView desired_category, Vector<ByteString> const& value_list, Vector<Alias>& prop_aliases, bool primary_value_is_first = true, bool sanitize_alias = false)
{
    TRY(file.seek(0, SeekMode::SetPosition));
    Array<u8, 1024> buffer;

    auto append_alias = [&](auto alias, auto value) {
        // Note: The value alias file contains lines such as "Ahom = Ahom", which we should just skip.
        if (alias == value)
            return;

        // FIXME: We will, eventually, need to find where missing properties are located and parse them.
        if (!value_list.contains_slow(value))
            return;

        prop_aliases.append({ value, alias });
    };

    while (TRY(file.can_read_line())) {
        auto line = TRY(file.read_line(buffer));

        if (line.is_empty() || line.starts_with('#'))
            continue;

        if (auto index = line.find('#'); index.has_value())
            line = line.substring_view(0, *index);

        auto segments = line.split_view(';', SplitBehavior::KeepEmpty);
        auto category = segments[0].trim_whitespace();

        if (category != desired_category)
            continue;

        VERIFY((segments.size() == 3) || (segments.size() == 4));
        auto value = primary_value_is_first ? segments[1].trim_whitespace() : segments[2].trim_whitespace();
        auto alias = primary_value_is_first ? segments[2].trim_whitespace() : segments[1].trim_whitespace();
        append_alias(sanitize_alias ? sanitize_entry(alias) : ByteString { alias }, value);

        if (segments.size() == 4) {
            alias = segments[3].trim_whitespace();
            append_alias(sanitize_alias ? sanitize_entry(alias) : ByteString { alias }, value);
        }
    }

    return {};
}

static ErrorOr<void> parse_unicode_data(Core::InputBufferedFile& file, UnicodeData& unicode_data)
{
    Optional<u32> code_point_range_start;
    Array<u8, 1024> buffer;

    while (TRY(file.can_read_line())) {
        auto line = TRY(file.read_line(buffer));

        if (line.is_empty())
            continue;

        auto segments = line.split_view(';', SplitBehavior::KeepEmpty);
        VERIFY(segments.size() == 15);

        CodePointData data {};
        data.code_point = AK::StringUtils::convert_to_uint_from_hex<u32>(segments[0]).value();
        data.name = segments[1];
        data.bidi_class = segments[4];
        data.numeric_value_decimal = AK::StringUtils::convert_to_int<i8>(segments[6]);
        data.numeric_value_digit = AK::StringUtils::convert_to_int<i8>(segments[7]);
        data.numeric_value_numeric = AK::StringUtils::convert_to_int<i8>(segments[8]);
        data.bidi_mirrored = segments[9] == "Y"sv;
        data.unicode_1_name = segments[10];
        data.iso_comment = segments[11];

        if (data.name.starts_with("<"sv) && data.name.ends_with(", First>"sv)) {
            VERIFY(!code_point_range_start.has_value());
            code_point_range_start = data.code_point;

            data.name = data.name.substring(1, data.name.length() - 9);
        } else if (data.name.starts_with("<"sv) && data.name.ends_with(", Last>"sv)) {
            VERIFY(code_point_range_start.has_value());

            Unicode::CodePointRange code_point_range { *code_point_range_start, data.code_point };

            data.name = data.name.substring(1, data.name.length() - 8);
            code_point_range_start.clear();

            unicode_data.code_point_bidirectional_classes.append({ code_point_range, data.bidi_class });
        } else {
            unicode_data.code_point_bidirectional_classes.append({ { data.code_point, data.code_point }, data.bidi_class });
        }

        unicode_data.bidirectional_classes.set(data.bidi_class, AK::HashSetExistingEntryBehavior::Keep);
        unicode_data.code_point_data.append(move(data));
    }

    return {};
}

static ErrorOr<void> generate_unicode_data_header(Core::InputBufferedFile& file, UnicodeData& unicode_data)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    auto generate_enum = [&](StringView name, StringView default_, auto values, Vector<Alias> aliases = {}) {
        quick_sort(values);
        quick_sort(aliases, [](auto& alias1, auto& alias2) { return alias1.alias < alias2.alias; });

        generator.set("name", name);
        generator.set("underlying", ByteString::formatted("{}UnderlyingType", name));
        generator.set("type", ((values.size() + !default_.is_empty()) < 256) ? "u8"sv : "u16"sv);

        generator.append(R"~~~(
using @underlying@ = @type@;

enum class @name@ : @underlying@ {)~~~");

        if (!default_.is_empty()) {
            generator.set("default", default_);
            generator.append(R"~~~(
    @default@,)~~~");
        }

        for (auto const& value : values) {
            generator.set("value", value);
            generator.append(R"~~~(
    @value@,)~~~");
        }

        for (auto const& alias : aliases) {
            generator.set("alias", alias.alias);
            generator.set("value", alias.name);
            generator.append(R"~~~(
    @alias@ = @value@,)~~~");
        }

        generator.append(R"~~~(
};
)~~~");
    };

    generator.append(R"~~~(
#pragma once

#include <AK/Types.h>
#include <LibUnicode/Forward.h>

namespace Unicode {
)~~~");

    generate_enum("GeneralCategory"sv, {}, unicode_data.general_categories.keys(), unicode_data.general_category_aliases);
    generate_enum("Script"sv, {}, unicode_data.script_list.keys(), unicode_data.script_aliases);
    generate_enum("BidirectionalClass"sv, {}, unicode_data.bidirectional_classes.values());

    generator.append(R"~~~(
}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

static ErrorOr<void> generate_unicode_data_implementation(Core::InputBufferedFile& file, UnicodeData const& unicode_data)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.set("CODE_POINT_TABLES_LSB_COUNT", TRY(String::number(CODE_POINT_TABLES_LSB_COUNT)));
    generator.set("CODE_POINT_TABLES_LSB_MASK", TRY(String::formatted("{:#x}", CODE_POINT_TABLES_LSB_MASK)));

    generator.append(R"~~~(
#include <AK/Array.h>
#include <AK/BinarySearch.h>
#include <AK/CharacterTypes.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/UnicodeData.h>

namespace Unicode {
)~~~");

    generator.append(R"~~~(
struct BidiClassData {
    CodePointRange code_point_range {};
    BidirectionalClass bidi_class {};
};

struct CodePointBidiClassComparator : public CodePointRangeComparator {
    constexpr int operator()(u32 code_point, BidiClassData const& bidi_class)
    {
        return CodePointRangeComparator::operator()(code_point, bidi_class.code_point_range);
    }
};

)~~~");

    auto append_property_table = [&](auto collection_snake, auto const& unique_properties) -> ErrorOr<void> {
        generator.set("name", TRY(String::formatted("{}_unique_properties", collection_snake)));
        generator.set("outer_size", TRY(String::number(unique_properties.size())));
        generator.set("inner_size", TRY(String::number(unique_properties[0].size())));

        generator.append(R"~~~(
static constexpr Array<Array<bool, @inner_size@>, @outer_size@> @name@ { {)~~~");

        for (auto const& property_set : unique_properties) {
            generator.append(R"~~~(
    { )~~~");

            for (auto value : property_set) {
                generator.set("value", TRY(String::formatted("{}", value)));
                generator.append("@value@, ");
            }

            generator.append(" },");
        }

        generator.append(R"~~~(
} };
)~~~");

        return {};
    };

    auto append_code_point_tables = [&](StringView collection_snake, auto const& tables, auto& append_unique_properties) -> ErrorOr<void> {
        auto append_stage = [&](auto const& stage, auto name, auto type) -> ErrorOr<void> {
            generator.set("name", TRY(String::formatted("{}_{}", collection_snake, name)));
            generator.set("size", TRY(String::number(stage.size())));
            generator.set("type", type);

            generator.append(R"~~~(
static constexpr Array<@type@, @size@> @name@ { {
    )~~~");

            static constexpr size_t max_values_per_row = 300;
            size_t values_in_current_row = 0;

            for (auto value : stage) {
                if (values_in_current_row++ > 0)
                    generator.append(", ");

                generator.set("value", TRY(String::number(value)));
                generator.append("@value@");

                if (values_in_current_row == max_values_per_row) {
                    values_in_current_row = 0;
                    generator.append(",\n    ");
                }
            }

            generator.append(R"~~~(
} };
)~~~");

            return {};
        };

        TRY(append_stage(tables.stage1, "stage1"sv, "u16"sv));
        TRY(append_stage(tables.stage2, "stage2"sv, "u16"sv));
        TRY(append_unique_properties(collection_snake, tables.unique_properties));
        return {};
    };

    TRY(append_code_point_tables("s_general_categories"sv, unicode_data.general_category_tables, append_property_table));
    TRY(append_code_point_tables("s_scripts"sv, unicode_data.script_tables, append_property_table));
    TRY(append_code_point_tables("s_script_extensions"sv, unicode_data.script_extension_tables, append_property_table));

    {
        constexpr size_t max_bidi_classes_per_row = 20;
        size_t bidi_classes_in_current_row = 0;

        generator.set("size"sv, ByteString::number(unicode_data.code_point_bidirectional_classes.size()));
        generator.append(R"~~~(
static constexpr Array<BidiClassData, @size@> s_bidirectional_classes { {
)~~~");
        for (auto const& data : unicode_data.code_point_bidirectional_classes) {
            if (bidi_classes_in_current_row++ > 0)
                generator.append(", ");

            generator.set("first", ByteString::formatted("{:#x}", data.code_point_range.first));
            generator.set("last", ByteString::formatted("{:#x}", data.code_point_range.last));
            generator.set("bidi_class", data.bidi_class);
            generator.append("{ { @first@, @last@ }, BidirectionalClass::@bidi_class@ }");

            if (bidi_classes_in_current_row == max_bidi_classes_per_row) {
                bidi_classes_in_current_row = 0;
                generator.append(",\n    ");
            }
        }
        generator.append(R"~~~(
} };
)~~~");
    }

    generator.append(R"~~~(
Optional<BidirectionalClass> bidirectional_class(u32 code_point)
{
    if (auto const* entry = binary_search(s_bidirectional_classes, code_point, nullptr, CodePointBidiClassComparator {}))
        return entry->bidi_class;

    return {};
}
)~~~");

    auto append_prop_search = [&](StringView enum_title, StringView enum_snake, StringView collection_name) -> ErrorOr<void> {
        generator.set("enum_title", enum_title);
        generator.set("enum_snake", enum_snake);
        generator.set("collection_name", collection_name);

        generator.append(R"~~~(
bool code_point_has_@enum_snake@(u32 code_point, @enum_title@ @enum_snake@)
{
    auto stage1_index = code_point >> @CODE_POINT_TABLES_LSB_COUNT@;
    auto stage2_index = @collection_name@_stage1[stage1_index] + (code_point & @CODE_POINT_TABLES_LSB_MASK@);
    auto unique_properties_index = @collection_name@_stage2[stage2_index];

    auto const& property_set = @collection_name@_unique_properties[unique_properties_index];
    return property_set[to_underlying(@enum_snake@)];
}
)~~~");

        return {};
    };

    auto append_from_string = [&](StringView enum_title, StringView enum_snake, auto const& prop_list, Vector<Alias> const& aliases) -> ErrorOr<void> {
        HashValueMap<StringView> hashes;
        TRY(hashes.try_ensure_capacity(prop_list.size() + aliases.size()));

        ValueFromStringOptions options {};

        for (auto const& prop : prop_list) {
            if constexpr (IsSame<RemoveCVReference<decltype(prop)>, ByteString>) {
                hashes.set(CaseInsensitiveASCIIStringViewTraits::hash(prop), prop);
                options.sensitivity = CaseSensitivity::CaseInsensitive;
            } else {
                hashes.set(prop.key.hash(), prop.key);
            }
        }

        for (auto const& alias : aliases)
            hashes.set(alias.alias.hash(), alias.alias);

        generate_value_from_string(generator, "{}_from_string"sv, enum_title, enum_snake, move(hashes), options);

        return {};
    };

    TRY(append_prop_search("GeneralCategory"sv, "general_category"sv, "s_general_categories"sv));
    TRY(append_from_string("GeneralCategory"sv, "general_category"sv, unicode_data.general_categories, unicode_data.general_category_aliases));

    TRY(append_prop_search("Script"sv, "script"sv, "s_scripts"sv));
    TRY(append_prop_search("Script"sv, "script_extension"sv, "s_script_extensions"sv));
    TRY(append_from_string("Script"sv, "script"sv, unicode_data.script_list, unicode_data.script_aliases));

    TRY(append_from_string("BidirectionalClass"sv, "bidirectional_class"sv, unicode_data.bidirectional_classes, {}));

    generator.append(R"~~~(
}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

static Vector<u32> flatten_code_point_ranges(Vector<Unicode::CodePointRange> const& code_points)
{
    Vector<u32> flattened;

    for (auto const& range : code_points) {
        flattened.grow_capacity(range.last - range.first);
        for (u32 code_point = range.first; code_point <= range.last; ++code_point)
            flattened.append(code_point);
    }

    return flattened;
}

static Vector<Unicode::CodePointRange> form_code_point_ranges(Vector<u32> code_points)
{
    Vector<Unicode::CodePointRange> ranges;

    u32 range_start = code_points[0];
    u32 range_end = range_start;

    for (size_t i = 1; i < code_points.size(); ++i) {
        u32 code_point = code_points[i];

        if ((code_point - range_end) == 1) {
            range_end = code_point;
        } else {
            ranges.append({ range_start, range_end });
            range_start = code_point;
            range_end = code_point;
        }
    }

    ranges.append({ range_start, range_end });
    return ranges;
}

static void sort_and_merge_code_point_ranges(Vector<Unicode::CodePointRange>& code_points)
{
    quick_sort(code_points, [](auto const& range1, auto const& range2) {
        return range1.first < range2.first;
    });

    for (size_t i = 0; i < code_points.size() - 1;) {
        if (code_points[i].last >= code_points[i + 1].first) {
            code_points[i].last = max(code_points[i].last, code_points[i + 1].last);
            code_points.remove(i + 1);
        } else {
            ++i;
        }
    }

    auto all_code_points = flatten_code_point_ranges(code_points);
    code_points = form_code_point_ranges(all_code_points);
}

static void populate_general_category_unions(PropList& general_categories)
{
    // The Unicode standard defines General Category values which are not in any UCD file. These
    // values are simply unions of other values.
    // https://www.unicode.org/reports/tr44/#GC_Values_Table
    auto populate_union = [&](auto alias, auto categories) {
        auto& code_points = general_categories.ensure(alias);
        for (auto const& category : categories)
            code_points.extend(general_categories.find(category)->value);

        sort_and_merge_code_point_ranges(code_points);
    };

    populate_union("LC"sv, Array { "Ll"sv, "Lu"sv, "Lt"sv });
    populate_union("L"sv, Array { "Lu"sv, "Ll"sv, "Lt"sv, "Lm"sv, "Lo"sv });
    populate_union("M"sv, Array { "Mn"sv, "Mc"sv, "Me"sv });
    populate_union("N"sv, Array { "Nd"sv, "Nl"sv, "No"sv });
    populate_union("P"sv, Array { "Pc"sv, "Pd"sv, "Ps"sv, "Pe"sv, "Pi"sv, "Pf"sv, "Po"sv });
    populate_union("S"sv, Array { "Sm"sv, "Sc"sv, "Sk"sv, "So"sv });
    populate_union("Z"sv, Array { "Zs"sv, "Zl"sv, "Zp"sv });
    populate_union("C"sv, Array { "Cc"sv, "Cf"sv, "Cs"sv, "Co"sv, "Cn"sv });
}

static ErrorOr<void> normalize_script_extensions(PropList& script_extensions, PropList const& script_list, Vector<Alias> const& script_aliases)
{
    // The ScriptExtensions UCD file lays out its code point ranges rather uniquely compared to
    // other files. The Script listed on each line may either be a full Script string or an aliased
    // abbreviation. Further, the extensions may or may not include the base Script list. Normalize
    // the extensions here to be keyed by the full Script name and always include the base list.
    auto extensions = move(script_extensions);
    script_extensions = TRY(script_list.clone());

    for (auto const& extension : extensions) {
        auto it = find_if(script_aliases.begin(), script_aliases.end(), [&](auto const& alias) { return extension.key == alias.alias; });
        auto const& key = (it == script_aliases.end()) ? extension.key : it->name;

        auto& code_points = script_extensions.find(key)->value;
        code_points.extend(extension.value);

        sort_and_merge_code_point_ranges(code_points);
    }

    // Lastly, the Common and Inherited script extensions are special. They must not contain any
    // code points which appear in other script extensions. The ScriptExtensions UCD file does not
    // list these extensions, therefore this peculiarity must be handled programmatically.
    // https://www.unicode.org/reports/tr24/#Assignment_ScriptX_Values
    auto code_point_has_other_extension = [&](StringView key, u32 code_point) {
        for (auto const& extension : extensions) {
            if (extension.key == key)
                continue;
            if (any_of(extension.value, [&](auto const& r) { return (r.first <= code_point) && (code_point <= r.last); }))
                return true;
        }

        return false;
    };

    auto get_code_points_without_other_extensions = [&](StringView key) {
        auto code_points = flatten_code_point_ranges(script_list.find(key)->value);
        code_points.remove_all_matching([&](u32 c) { return code_point_has_other_extension(key, c); });
        return code_points;
    };

    auto common_code_points = get_code_points_without_other_extensions("Common"sv);
    script_extensions.set("Common"sv, form_code_point_ranges(common_code_points));

    auto inherited_code_points = get_code_points_without_other_extensions("Inherited"sv);
    script_extensions.set("Inherited"sv, form_code_point_ranges(inherited_code_points));
    return {};
}

struct PropertyMetadata {
    static ErrorOr<PropertyMetadata> create(PropList& property_list)
    {
        PropertyMetadata data;
        TRY(data.property_values.try_ensure_capacity(property_list.size()));
        TRY(data.property_set.try_ensure_capacity(property_list.size()));

        auto property_names = property_list.keys();
        quick_sort(property_names);

        for (auto& property_name : property_names) {
            auto& code_point_ranges = property_list.get(property_name).value();
            data.property_values.unchecked_append(move(code_point_ranges));
        }

        return data;
    }

    Vector<typename PropList::ValueType> property_values;
    PropertyTable property_set;

    Vector<size_t> current_block;
    HashMap<decltype(current_block), size_t> unique_blocks;
};

// The goal here is to produce a set of tables that represent a category of code point properties for every code point.
// The most naive method would be to generate a single table per category, each with one entry per code point. Each of
// those tables would have a size of 0x10ffff though, which is a non-starter. Instead, we create a set of 2-stage lookup
// tables per category.
//
// To do so, it's important to note that Unicode tends to organize code points with similar properties together. This
// leads to long series of code points with identical properties. Therefore, if we divide the 0x10ffff code points into
// fixed-size blocks, many of those blocks will also be identical.
//
// So we iterate over every code point, classifying each one for the category of interest. We represent a classification
// as a list of booleans. We store the classification in the CodePointTables::unique_properties list for this category.
// As the name implies, this list is de-duplicated; we store the index into this list in a separate list, which we call
// a "block".
//
// As we iterate, we "pause" every BLOCK_SIZE code points to examine the block. If the block is unique so far, we extend
// CodePointTables::stage2 with the entries of that block (so CodePointTables::stage2 is also a list of indices into
// CodePointTables::unique_properties). We then append the index of the start of that block in CodePointTables::stage2
// to CodePointTables::stage1.
//
// The value of BLOCK_SIZE is determined by CodePointTables::MSB_COUNT and CodePointTables::LSB_COUNT. We need 24 bits
// to describe all code points; the blocks we create are based on splitting these bits into 2 segments. We currently use
// a 16:8 bit split. So when perform a runtime lookup of a code point in the 2-stage tables, we:
//
//     1. Use most-significant 16 bits of the code point as the index into CodePointTables::stage1. That value is the
//        index into CodePointTables::stage2 of the start of the block that contains properties for this code point.
//
//     2. Add the least-significant 8 bits of the code point to that value, to use as the index into
//        CodePointTables::stage2. As described above, that value is the index into CodePointTables::unique_properties,
//        which contains the classification for this code point.
//
// Using the code point GeneralCategory as an example, we end up with a CodePointTables::stage1 with a size of ~4000,
// a CodePointTables::stage2 with a size of ~40,000, and a CodePointTables::unique_properties with a size of ~30. So
// this process reduces over 1 million entries (0x10ffff) to ~44,030.
//
// For much more in-depth reading, see: https://icu.unicode.org/design/struct/utrie
static constexpr auto MAX_CODE_POINT = 0x10ffffu;

template<typename T>
static ErrorOr<void> update_tables(u32 code_point, CodePointTables<T>& tables, auto& metadata, auto const& values)
{
    static constexpr auto BLOCK_SIZE = CODE_POINT_TABLES_LSB_MASK + 1;

    size_t unique_properties_index = 0;
    if (auto block_index = tables.unique_properties.find_first_index(values); block_index.has_value()) {
        unique_properties_index = *block_index;
    } else {
        unique_properties_index = tables.unique_properties.size();
        TRY(tables.unique_properties.try_append(values));
    }

    TRY(metadata.current_block.try_append(unique_properties_index));

    if (metadata.current_block.size() == BLOCK_SIZE || code_point == MAX_CODE_POINT) {
        size_t stage2_index = 0;
        if (auto block_index = metadata.unique_blocks.get(metadata.current_block); block_index.has_value()) {
            stage2_index = *block_index;
        } else {
            stage2_index = tables.stage2.size();
            TRY(tables.stage2.try_extend(metadata.current_block));

            TRY(metadata.unique_blocks.try_set(metadata.current_block, stage2_index));
        }

        TRY(tables.stage1.try_append(stage2_index));
        metadata.current_block.clear_with_capacity();
    }

    return {};
}

static ErrorOr<void> create_code_point_tables(UnicodeData& unicode_data)
{
    auto update_property_tables = [&]<typename T>(u32 code_point, CodePointTables<T>& tables, PropertyMetadata& metadata) -> ErrorOr<void> {
        static Unicode::CodePointRangeComparator comparator {};

        for (auto& property_values : metadata.property_values) {
            size_t ranges_to_remove = 0;
            auto has_property = false;

            for (auto const& range : property_values) {
                if (auto comparison = comparator(code_point, range); comparison <= 0) {
                    has_property = comparison == 0;
                    break;
                }

                ++ranges_to_remove;
            }

            metadata.property_set.unchecked_append(has_property);
            property_values.remove(0, ranges_to_remove);
        }

        TRY(update_tables(code_point, tables, metadata, metadata.property_set));
        metadata.property_set.clear_with_capacity();

        return {};
    };

    auto general_category_metadata = TRY(PropertyMetadata::create(unicode_data.general_categories));
    auto script_metadata = TRY(PropertyMetadata::create(unicode_data.script_list));
    auto script_extension_metadata = TRY(PropertyMetadata::create(unicode_data.script_extensions));

    for (u32 code_point = 0; code_point <= MAX_CODE_POINT; ++code_point) {
        TRY(update_property_tables(code_point, unicode_data.general_category_tables, general_category_metadata));
        TRY(update_property_tables(code_point, unicode_data.script_tables, script_metadata));
        TRY(update_property_tables(code_point, unicode_data.script_extension_tables, script_extension_metadata));
    }

    return {};
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView unicode_data_path;
    StringView derived_general_category_path;
    StringView prop_value_alias_path;
    StringView scripts_path;
    StringView script_extensions_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the Unicode Data header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Unicode Data implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(unicode_data_path, "Path to UnicodeData.txt file", "unicode-data-path", 'u', "unicode-data-path");
    args_parser.add_option(derived_general_category_path, "Path to DerivedGeneralCategory.txt file", "derived-general-category-path", 'g', "derived-general-category-path");
    args_parser.add_option(prop_value_alias_path, "Path to PropertyValueAliases.txt file", "prop-value-alias-path", 'v', "prop-value-alias-path");
    args_parser.add_option(scripts_path, "Path to Scripts.txt file", "scripts-path", 'r', "scripts-path");
    args_parser.add_option(script_extensions_path, "Path to ScriptExtensions.txt file", "script-extensions-path", 'x', "script-extensions-path");
    args_parser.parse(arguments);

    auto generated_header_file = TRY(open_file(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(open_file(generated_implementation_path, Core::File::OpenMode::Write));
    auto unicode_data_file = TRY(open_file(unicode_data_path, Core::File::OpenMode::Read));
    auto derived_general_category_file = TRY(open_file(derived_general_category_path, Core::File::OpenMode::Read));
    auto prop_value_alias_file = TRY(open_file(prop_value_alias_path, Core::File::OpenMode::Read));
    auto scripts_file = TRY(open_file(scripts_path, Core::File::OpenMode::Read));
    auto script_extensions_file = TRY(open_file(script_extensions_path, Core::File::OpenMode::Read));

    UnicodeData unicode_data {};
    TRY(parse_prop_list(*derived_general_category_file, unicode_data.general_categories));
    TRY(parse_prop_list(*scripts_file, unicode_data.script_list));
    TRY(parse_prop_list(*script_extensions_file, unicode_data.script_extensions, true));

    populate_general_category_unions(unicode_data.general_categories);
    TRY(parse_unicode_data(*unicode_data_file, unicode_data));
    TRY(parse_value_alias_list(*prop_value_alias_file, "gc"sv, unicode_data.general_categories.keys(), unicode_data.general_category_aliases));
    TRY(parse_value_alias_list(*prop_value_alias_file, "sc"sv, unicode_data.script_list.keys(), unicode_data.script_aliases, false));
    TRY(normalize_script_extensions(unicode_data.script_extensions, unicode_data.script_list, unicode_data.script_aliases));

    TRY(create_code_point_tables(unicode_data));

    TRY(generate_unicode_data_header(*generated_header_file, unicode_data));
    TRY(generate_unicode_data_implementation(*generated_implementation_file, unicode_data));

    return 0;
}
