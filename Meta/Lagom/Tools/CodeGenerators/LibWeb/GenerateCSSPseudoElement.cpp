/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/SourceGenerator.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

ErrorOr<void> generate_header_file(JsonObject& pseudo_elements_data, Core::File& file);
ErrorOr<void> generate_implementation_file(JsonObject& pseudo_elements_data, Core::File& file);

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the PseudoElements header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the PseudoElements implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(json_path, "Path to the JSON file to read from", "json-path", 'j', "json-path");
    args_parser.parse(arguments);

    auto json = TRY(read_entire_file_as_json(json_path));
    VERIFY(json.is_object());
    auto data = json.as_object();

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(data, *generated_header_file));
    TRY(generate_implementation_file(data, *generated_implementation_file));

    return 0;
}

ErrorOr<void> generate_header_file(JsonObject& pseudo_elements_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    auto pseudo_element_count = 0u;
    auto generated_pseudo_element_count = 0u;
    pseudo_elements_data.for_each_member([&](auto const&, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        ++pseudo_element_count;
        if (pseudo_element.get_bool("is-generated"sv).value_or(false))
            ++generated_pseudo_element_count;
    });
    generator.set("pseudo_element_underlying_type", underlying_type_for_enum(pseudo_element_count));
    generator.set("generated_pseudo_element_underlying_type", underlying_type_for_enum(generated_pseudo_element_count));

    generator.append(R"~~~(
#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>

namespace Web::CSS {

enum class PseudoElement : @pseudo_element_underlying_type@ {
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, auto&) {
        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.appendln("    @name:titlecase@,");
    });
    generator.append(R"~~~(
    KnownPseudoElementCount,

    UnknownWebKit,
};

Optional<PseudoElement> pseudo_element_from_string(StringView);
StringView pseudo_element_name(PseudoElement);

bool is_has_allowed_pseudo_element(PseudoElement);

enum class GeneratedPseudoElement : @generated_pseudo_element_underlying_type@ {
)~~~");
    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        if (!pseudo_element.get_bool("is-generated"sv).value_or(false))
            return;

        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.appendln("    @name:titlecase@,");
    });
    generator.append(R"~~~(
};

Optional<GeneratedPseudoElement> to_generated_pseudo_element(PseudoElement);
PseudoElement to_pseudo_element(GeneratedPseudoElement);

}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_implementation_file(JsonObject& pseudo_elements_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <LibWeb/CSS/PseudoElement.h>

namespace Web::CSS {

Optional<PseudoElement> pseudo_element_from_string(StringView string)
{
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, auto&) {
        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.append(R"~~~(
    if (string.equals_ignoring_ascii_case("@name@"sv))
        return PseudoElement::@name:titlecase@;
)~~~");
    });

    generator.append(R"~~~(

    return {};
}

StringView pseudo_element_name(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, auto&) {
        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.append(R"~~~(
    case PseudoElement::@name:titlecase@:
        return "@name@"sv;
)~~~");
    });

    generator.append(R"~~~(
    case PseudoElement::KnownPseudoElementCount:
    case PseudoElement::UnknownWebKit:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

bool is_has_allowed_pseudo_element(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        if (!pseudo_element.get_bool("is-allowed-in-has"sv).value_or(false))
            return;

        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.append(R"~~~(
    case PseudoElement::@name:titlecase@:
        return true;
)~~~");
    });

    generator.append(R"~~~(
    default:
        return false;
    }
}

Optional<GeneratedPseudoElement> to_generated_pseudo_element(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        if (!pseudo_element.get_bool("is-generated"sv).value_or(false))
            return;

        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.append(R"~~~(
    case PseudoElement::@name:titlecase@:
        return GeneratedPseudoElement::@name:titlecase@;
)~~~");
    });

    generator.append(R"~~~(
    default:
        return {};
    }
}

PseudoElement to_pseudo_element(GeneratedPseudoElement generated_pseudo_element)
{
    switch (generated_pseudo_element) {
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        if (!pseudo_element.get_bool("is-generated"sv).value_or(false))
            return;

        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.append(R"~~~(
    case GeneratedPseudoElement::@name:titlecase@:
        return PseudoElement::@name:titlecase@;
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
