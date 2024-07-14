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

namespace {
struct LookupTable {
    u32 first_pointer;
    u32 max_code_point;
    Vector<u32> code_points;
    bool generate_accessor;
};

struct LookupTables {
    JsonArray const& gb18030_ranges;
    OrderedHashMap<StringView, LookupTable> indexes;
};

enum class GenerateAccessor {
    No,
    Yes,
};

LookupTable prepare_table(JsonArray const& data, GenerateAccessor generate_accessor = GenerateAccessor::No)
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
    if (generate_accessor == GenerateAccessor::Yes) {
        while (code_points.last() == 0xfffd)
            code_points.take_last();
    } else {
        VERIFY(first_pointer == 0);
    }
    return { first_pointer, max, move(code_points), generate_accessor == GenerateAccessor::Yes };
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
    if (table.generate_accessor)
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

void generate_table_accessor(SourceGenerator generator, StringView name, LookupTable& table)
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

    for (auto& [key, table] : tables.indexes) {
        if (table.generate_accessor)
            generate_table_accessor(generator.fork(), key, table);
    }

    generator.appendln("\n}");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}
} // end anonymous namespace

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

    auto gb18030_table = prepare_table(data.get("gb18030"sv)->as_array(), GenerateAccessor::Yes);

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
            { "big5"sv, prepare_table(data.get("big5"sv)->as_array(), GenerateAccessor::Yes) },
            { "jis0208"sv, prepare_table(data.get("jis0208"sv)->as_array(), GenerateAccessor::Yes) },
            { "jis0212"sv, prepare_table(data.get("jis0212"sv)->as_array(), GenerateAccessor::Yes) },
            { "euc_kr"sv, prepare_table(data.get("euc-kr"sv)->as_array(), GenerateAccessor::Yes) },
            { "ibm866"sv, prepare_table(data.get("ibm866"sv)->as_array()) },
            { "iso_8859_2"sv, prepare_table(data.get("iso-8859-2"sv)->as_array()) },
            { "iso_8859_3"sv, prepare_table(data.get("iso-8859-3"sv)->as_array()) },
            { "iso_8859_4"sv, prepare_table(data.get("iso-8859-4"sv)->as_array()) },
            { "iso_8859_5"sv, prepare_table(data.get("iso-8859-5"sv)->as_array()) },
            { "iso_8859_6"sv, prepare_table(data.get("iso-8859-6"sv)->as_array()) },
            { "iso_8859_7"sv, prepare_table(data.get("iso-8859-7"sv)->as_array()) },
            { "iso_8859_8"sv, prepare_table(data.get("iso-8859-8"sv)->as_array()) },
            { "iso_8859_10"sv, prepare_table(data.get("iso-8859-10"sv)->as_array()) },
            { "iso_8859_13"sv, prepare_table(data.get("iso-8859-13"sv)->as_array()) },
            { "iso_8859_14"sv, prepare_table(data.get("iso-8859-14"sv)->as_array()) },
            { "iso_8859_15"sv, prepare_table(data.get("iso-8859-15"sv)->as_array()) },
            { "iso_8859_16"sv, prepare_table(data.get("iso-8859-16"sv)->as_array()) },
            { "koi8_r"sv, prepare_table(data.get("koi8-r"sv)->as_array()) },
            { "koi8_u"sv, prepare_table(data.get("koi8-u"sv)->as_array()) },
            { "macintosh"sv, prepare_table(data.get("macintosh"sv)->as_array()) },
            { "windows_874"sv, prepare_table(data.get("windows-874"sv)->as_array()) },
            { "windows_1250"sv, prepare_table(data.get("windows-1250"sv)->as_array()) },
            { "windows_1251"sv, prepare_table(data.get("windows-1251"sv)->as_array()) },
            { "windows_1252"sv, prepare_table(data.get("windows-1252"sv)->as_array()) },
            { "windows_1253"sv, prepare_table(data.get("windows-1253"sv)->as_array()) },
            { "windows_1254"sv, prepare_table(data.get("windows-1254"sv)->as_array()) },
            { "windows_1255"sv, prepare_table(data.get("windows-1255"sv)->as_array()) },
            { "windows_1256"sv, prepare_table(data.get("windows-1256"sv)->as_array()) },
            { "windows_1257"sv, prepare_table(data.get("windows-1257"sv)->as_array()) },
            { "windows_1258"sv, prepare_table(data.get("windows-1258"sv)->as_array()) },
            { "x_mac_cyrillic"sv, prepare_table(data.get("x-mac-cyrillic"sv)->as_array()) },
        },
    };

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(tables, *generated_header_file));
    TRY(generate_implementation_file(tables, *generated_implementation_file));

    return 0;
}
