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

struct CodePointBidiClass {
    Unicode::CodePointRange code_point_range;
    ByteString bidi_class;
};

struct UnicodeData {
    Vector<CodePointData> code_point_data;

    HashTable<ByteString> bidirectional_classes;
    Vector<CodePointBidiClass> code_point_bidirectional_classes;
};

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

    TRY(append_from_string("BidirectionalClass"sv, "bidirectional_class"sv, unicode_data.bidirectional_classes, {}));

    generator.append(R"~~~(
}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView unicode_data_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the Unicode Data header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Unicode Data implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(unicode_data_path, "Path to UnicodeData.txt file", "unicode-data-path", 'u', "unicode-data-path");
    args_parser.parse(arguments);

    auto generated_header_file = TRY(open_file(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(open_file(generated_implementation_path, Core::File::OpenMode::Write));
    auto unicode_data_file = TRY(open_file(unicode_data_path, Core::File::OpenMode::Read));

    UnicodeData unicode_data {};

    TRY(parse_unicode_data(*unicode_data_file, unicode_data));

    TRY(generate_unicode_data_header(*generated_header_file, unicode_data));
    TRY(generate_unicode_data_implementation(*generated_implementation_file, unicode_data));

    return 0;
}
