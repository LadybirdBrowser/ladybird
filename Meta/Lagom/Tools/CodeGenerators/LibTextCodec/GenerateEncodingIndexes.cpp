/*
 * Copyright (c) 2024, Simon Wanner <simon@skyrising.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/JsonObject.h>
#include <AK/NumericLimits.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>

struct LookupTable {
    u32 first_pointer;
    u32 max_code_point;
    Vector<u32> code_points;
};

struct LookupTables {
    JsonArray const& gb18030_ranges;
    OrderedHashMap<StringView, LookupTable> indexes;
};

LookupTable prepare_table(JsonArray const& data)
{
    Vector<u32> code_points;
    code_points.ensure_capacity(data.size());
    u32 max = 0;
    u32 first_pointer = 0;
    for (auto const& entry : data.values()) {
        if (entry.is_null()) {
            if (code_points.is_empty()) {
                first_pointer++;
            } else {
                code_points.append(0xfffd);
                max = AK::max(max, code_points.last());
            }
        } else {
            code_points.append(entry.as_integer<u32>());
            max = AK::max(max, code_points.last());
        }
    }
    while (code_points.last() == 0xfffd)
        code_points.take_last();
    return { first_pointer, max, move(code_points) };
}

void generate_table(SourceGenerator generator, StringView name, LookupTable& table)
{
    generator.set("name", name);
    generator.set("value_type", table.max_code_point > NumericLimits<u16>::max() ? "u32" : "u16");
    generator.set("first_pointer", MUST(String::number(table.first_pointer)));
    generator.set("size", MUST(String::number(table.code_points.size())));

    if (table.first_pointer > 0) {
        generator.appendln("static constexpr u32 s_@name@_index_first_pointer = @first_pointer@;");
    }

    generator.append("static constexpr Array<@value_type@, @size@> s_@name@_index {\n    ");
    for (size_t i = 0; i < table.code_points.size(); i++) {
        generator.append(MUST(String::formatted("{:#04x}", table.code_points[i])));
        if (i != table.code_points.size() - 1)
            generator.append(i % 16 == 15 ? ",\n    "sv : ", "sv);
    }
    generator.appendln("\n};");
    generator.appendln("Optional<u32> index_@name@_code_point(u32 pointer);");
}

ErrorOr<void> generate_header_file(LookupTables& tables, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.set("gb18030_ranges_size", MUST(String::number(tables.gb18030_ranges.size())));

    generator.append(R"~~~(
#pragma once

#include <AK/Array.h>
#include <AK/Types.h>

namespace TextCodec {

struct Gb18030RangeEntry {
    u32 pointer;
    u32 code_point;
};

static constexpr Array<Gb18030RangeEntry, @gb18030_ranges_size@> s_gb18030_ranges { {
)~~~");

    for (auto const& range : tables.gb18030_ranges.values()) {
        generator.appendln(MUST(String::formatted("    {{ {}, {:#04x} }},", range.as_array()[0].as_integer<u32>(), range.as_array()[1].as_integer<u32>())));
    }
    generator.appendln("} };\n");

    for (auto e : tables.indexes) {
        generate_table(generator.fork(), e.key, e.value);
    }

    generator.append("\n");
    generator.appendln("}");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

void generate_table_implementation(SourceGenerator generator, StringView name, LookupTable& table)
{
    generator.set("name", name);
    generator.set("first_pointer", MUST(String::number(table.first_pointer)));
    generator.set("size", MUST(String::number(table.code_points.size())));

    if (table.first_pointer > 0) {
        generator.append(R"~~~(
Optional<u32> index_@name@_code_point(u32 pointer)
{
    if (pointer < s_@name@_index_first_pointer || pointer - s_@name@_index_first_pointer >= s_@name@_index.size())
        return {};
    auto value = s_@name@_index[pointer - s_@name@_index_first_pointer];
    if (value == 0xfffd)
        return {};
    return value;
}
)~~~");
    } else {
        generator.append(R"~~~(
Optional<u32> index_@name@_code_point(u32 pointer)
{
    if (pointer >= s_@name@_index.size())
        return {};
    auto value = s_@name@_index[pointer];
    if (value == 0xfffd)
        return {};
    return value;
}
)~~~");
    }
}

ErrorOr<void> generate_implementation_file(LookupTables& tables, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <LibTextCodec/LookupTables.h>

namespace TextCodec {
)~~~");

    for (auto e : tables.indexes) {
        generate_table_implementation(generator.fork(), e.key, e.value);
    }

    generator.appendln("\n}");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the lookup table header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the lookup table implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(json_path, "Path to the JSON file to read from", "json-path", 'j', "json-path");
    args_parser.parse(arguments);

    auto json_file = TRY(Core::File::open(json_path, Core::File::OpenMode::Read));
    auto json_data = TRY(json_file->read_until_eof());
    auto data = TRY(JsonValue::from_string(json_data)).as_object();

    auto gb18030_table = prepare_table(data.get("gb18030"sv)->as_array());

    // FIXME: Encoding specification is not updated to GB-18030-2022 yet (https://github.com/whatwg/encoding/issues/312)
    // NOTE: See https://commits.webkit.org/264918@main
    gb18030_table.code_points[7182] = 0xfe10;
    gb18030_table.code_points[7183] = 0xfe12;
    gb18030_table.code_points[7184] = 0xfe11;
    gb18030_table.code_points[7185] = 0xfe13;
    gb18030_table.code_points[7186] = 0xfe14;
    gb18030_table.code_points[7187] = 0xfe15;
    gb18030_table.code_points[7188] = 0xfe16;
    gb18030_table.code_points[7201] = 0xfe17;
    gb18030_table.code_points[7202] = 0xfe18;
    gb18030_table.code_points[7208] = 0xfe19;
    gb18030_table.code_points[23775] = 0x9fb4;
    gb18030_table.code_points[23783] = 0x9fb5;
    gb18030_table.code_points[23788] = 0x9fb6;
    gb18030_table.code_points[23789] = 0x9fb7;
    gb18030_table.code_points[23795] = 0x9fb8;
    gb18030_table.code_points[23812] = 0x9fb9;
    gb18030_table.code_points[23829] = 0x9fba;
    gb18030_table.code_points[23845] = 0x9fbb;

    LookupTables tables {
        .gb18030_ranges = data.get("gb18030-ranges"sv)->as_array(),
        .indexes = {
            { "gb18030"sv, move(gb18030_table) },
            { "big5"sv, prepare_table(data.get("big5"sv)->as_array()) },
            { "jis0208"sv, prepare_table(data.get("jis0208"sv)->as_array()) },
            { "jis0212"sv, prepare_table(data.get("jis0212"sv)->as_array()) },
            { "euc_kr"sv, prepare_table(data.get("euc-kr"sv)->as_array()) },
            { "windows_1252"sv, prepare_table(data.get("windows-1252"sv)->as_array()) },
        },
    };

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(tables, *generated_header_file));
    TRY(generate_implementation_file(tables, *generated_implementation_file));

    return 0;
}
