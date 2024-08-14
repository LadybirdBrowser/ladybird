/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

ErrorOr<void> generate_header_file(JsonObject& enums_data, Core::File& file);
ErrorOr<void> generate_implementation_file(JsonObject& enums_data, Core::File& file);

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the Enums header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Enums implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(json_path, "Path to the JSON file to read from", "json-path", 'j', "json-path");
    args_parser.parse(arguments);

    auto json = TRY(read_entire_file_as_json(json_path));
    VERIFY(json.is_object());
    auto enums_data = json.as_object();

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(enums_data, *generated_header_file));
    TRY(generate_implementation_file(enums_data, *generated_implementation_file));

    return 0;
}

ErrorOr<void> generate_header_file(JsonObject& enums_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#pragma once

#include <AK/Optional.h>

namespace Web::CSS {

enum class Keyword;

)~~~");

    enums_data.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_array());
        auto& members = value.as_array();

        auto enum_generator = generator.fork();
        enum_generator.set("name:titlecase", title_casify(name));
        enum_generator.set("name:snakecase", snake_casify(name));

        // Find the smallest possible type to use.
        auto member_max_value = members.size() - 1;
        if (NumericLimits<u8>::max() >= member_max_value) {
            enum_generator.set("enum_type", "u8"_string);
        } else if (NumericLimits<u16>::max() >= member_max_value) {
            enum_generator.set("enum_type", "u16"_string);
        } else if (NumericLimits<u32>::max() >= member_max_value) {
            enum_generator.set("enum_type", "u32"_string);
        } else {
            enum_generator.set("enum_type", "u64"_string);
        }

        enum_generator.appendln("enum class @name:titlecase@ : @enum_type@ {");

        for (auto& member : members.values()) {
            auto member_name = member.as_string();
            // Don't include aliases in the enum.
            if (member_name.contains('='))
                continue;
            auto member_generator = enum_generator.fork();
            member_generator.set("member:titlecase", title_casify(member_name));
            member_generator.appendln("    @member:titlecase@,");
        }

        enum_generator.appendln("};");
        enum_generator.appendln("Optional<@name:titlecase@> keyword_to_@name:snakecase@(Keyword);");
        enum_generator.appendln("Keyword to_keyword(@name:titlecase@);");
        enum_generator.appendln("StringView to_string(@name:titlecase@);");
        enum_generator.append("\n");
    });

    generator.appendln("}");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_implementation_file(JsonObject& enums_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Keyword.h>

namespace Web::CSS {
)~~~");

    enums_data.for_each_member([&](auto& name, auto& value) -> void {
        VERIFY(value.is_array());
        auto& members = value.as_array();

        auto enum_generator = generator.fork();
        enum_generator.set("name:titlecase", title_casify(name));
        enum_generator.set("name:snakecase", snake_casify(name));

        enum_generator.append(R"~~~(
Optional<@name:titlecase@> keyword_to_@name:snakecase@(Keyword keyword)
{
    switch (keyword) {)~~~");

        for (auto& member : members.values()) {
            auto member_generator = enum_generator.fork();
            auto member_name = member.as_string();
            if (member_name.contains('=')) {
                auto parts = member_name.split_view('=');
                member_generator.set("valueid:titlecase", title_casify(parts[0]));
                member_generator.set("member:titlecase", title_casify(parts[1]));
            } else {
                member_generator.set("valueid:titlecase", title_casify(member_name));
                member_generator.set("member:titlecase", title_casify(member_name));
            }
            member_generator.append(R"~~~(
    case Keyword::@valueid:titlecase@:
        return @name:titlecase@::@member:titlecase@;)~~~");
        }

        enum_generator.append(R"~~~(
    default:
        return {};
    }
}
)~~~");

        enum_generator.append(R"~~~(
Keyword to_keyword(@name:titlecase@ @name:snakecase@_value)
{
    switch (@name:snakecase@_value) {)~~~");

        for (auto& member : members.values()) {
            auto member_generator = enum_generator.fork();
            auto member_name = member.as_string();
            if (member_name.contains('='))
                continue;
            member_generator.set("member:titlecase", title_casify(member_name));

            member_generator.append(R"~~~(
    case @name:titlecase@::@member:titlecase@:
        return Keyword::@member:titlecase@;)~~~");
        }

        enum_generator.append(R"~~~(
    default:
        VERIFY_NOT_REACHED();
    }
}
)~~~");

        enum_generator.append(R"~~~(
StringView to_string(@name:titlecase@ value)
{
    switch (value) {)~~~");

        for (auto& member : members.values()) {
            auto member_generator = enum_generator.fork();
            auto member_name = member.as_string();
            if (member_name.contains('='))
                continue;
            member_generator.set("member:css", member_name);
            member_generator.set("member:titlecase", title_casify(member_name));

            member_generator.append(R"~~~(
    case @name:titlecase@::@member:titlecase@:
        return "@member:css@"sv;)~~~");
        }

        enum_generator.append(R"~~~(
    default:
        VERIFY_NOT_REACHED();
    }
}
)~~~");
    });

    generator.appendln("}");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}
