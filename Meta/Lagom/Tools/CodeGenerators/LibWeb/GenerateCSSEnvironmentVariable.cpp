/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/SourceGenerator.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

ErrorOr<void> generate_header_file(JsonObject const& environment_variables, Core::File& file);
ErrorOr<void> generate_implementation_file(JsonObject const& environment_variables, Core::File& file);

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the EnvironmentVariable header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the EnvironmentVariable implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(json_path, "Path to the JSON file to read from", "json-path", 'j', "json-path");
    args_parser.parse(arguments);

    auto read_json_object = [](auto& path) -> ErrorOr<JsonObject> {
        auto json = TRY(read_entire_file_as_json(path));
        VERIFY(json.is_object());

        // Check we're in alphabetical order
        String most_recent_name;
        json.as_object().for_each_member([&](auto& name, auto&) {
            if (name < most_recent_name) {
                warnln("`{}` is in the wrong position in `{}`. Please keep this list alphabetical!", name, path);
                VERIFY_NOT_REACHED();
            }
            most_recent_name = name;
        });

        return json.as_object();
    };

    auto environment_variables = TRY(read_json_object(json_path));

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(environment_variables, *generated_header_file));
    TRY(generate_implementation_file(environment_variables, *generated_implementation_file));

    return 0;
}

ErrorOr<void> generate_header_file(JsonObject const& environment_variables, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.set("environment_variable_underlying_type", underlying_type_for_enum(environment_variables.size()));

    generator.append(R"~~~(
#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibWeb/CSS/ValueType.h>

namespace Web::CSS {

enum class EnvironmentVariable : @environment_variable_underlying_type@ {
)~~~");

    environment_variables.for_each_member([&](auto& name, JsonValue const&) {
        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.appendln("    @name:titlecase@,");
    });
    generator.append(R"~~~(
};

Optional<EnvironmentVariable> environment_variable_from_string(StringView);
StringView to_string(EnvironmentVariable);

ValueType environment_variable_type(EnvironmentVariable);
u32 environment_variable_dimension_count(EnvironmentVariable);
}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_implementation_file(JsonObject const& environment_variables, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <AK/HashMap.h>
#include <LibWeb/CSS/EnvironmentVariable.h>

namespace Web::CSS {

static HashMap<StringView, EnvironmentVariable, AK::CaseInsensitiveASCIIStringViewTraits> environment_variable_table = {
)~~~");

    environment_variables.for_each_member([&](auto& name, JsonValue const&) {
        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.append(R"~~~(
    { "@name@"sv, EnvironmentVariable::@name:titlecase@ },
)~~~");
    });

    generator.append(R"~~~(
};

Optional<EnvironmentVariable> environment_variable_from_string(StringView string)
{
    return environment_variable_table.get(string);
}

StringView to_string(EnvironmentVariable environment_variable)
{
    switch (environment_variable) {
)~~~");

    environment_variables.for_each_member([&](auto& name, JsonValue const&) {
        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.append(R"~~~(
    case EnvironmentVariable::@name:titlecase@:
        return "@name@"sv;
)~~~");
    });

    generator.append(R"~~~(
    }
    VERIFY_NOT_REACHED();
}

ValueType environment_variable_type(EnvironmentVariable environment_variable)
{
    switch (environment_variable) {
)~~~");

    environment_variables.for_each_member([&](auto& name, JsonValue const& value) {
        auto& variable = value.as_object();
        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));

        auto value_type = MUST(variable.get_string("type"sv)->trim("<>"sv));
        member_generator.set("value_type:titlecase", title_casify(value_type));

        member_generator.append(R"~~~(
    case EnvironmentVariable::@name:titlecase@:
        return ValueType::@value_type:titlecase@;
)~~~");
    });

    generator.append(R"~~~(
    }
    VERIFY_NOT_REACHED();
}

u32 environment_variable_dimension_count(EnvironmentVariable environment_variable)
{
    switch (environment_variable) {
)~~~");

    environment_variables.for_each_member([&](auto& name, JsonValue const& value) {
        auto& variable = value.as_object();
        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.set("dimension_count", String::number(variable.get_u32("dimensions"sv).value()));

        member_generator.append(R"~~~(
    case EnvironmentVariable::@name:titlecase@:
        return @dimension_count@;
)~~~");
    });

    generator.append(R"~~~(
    }
    VERIFY_NOT_REACHED();
}

}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}
