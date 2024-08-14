/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/SourceGenerator.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

ErrorOr<void> generate_header_file(JsonObject& pseudo_classes_data, Core::File& file);
ErrorOr<void> generate_implementation_file(JsonObject& pseudo_classes_data, Core::File& file);

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the PseudoClasses header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the PseudoClasses implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
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

ErrorOr<void> generate_header_file(JsonObject& pseudo_classes_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>

namespace Web::CSS {

enum class PseudoClass {
)~~~");

    pseudo_classes_data.for_each_member([&](auto& name, auto&) {
        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.appendln("    @name:titlecase@,");
    });
    generator.append(R"~~~(
};

Optional<PseudoClass> pseudo_class_from_string(StringView);
StringView pseudo_class_name(PseudoClass);

struct PseudoClassMetadata {
    enum class ParameterType {
        None,
        ANPlusB,
        ANPlusBOf,
        CompoundSelector,
        ForgivingSelectorList,
        ForgivingRelativeSelectorList,
        Ident,
        LanguageRanges,
        SelectorList,
    } parameter_type;
    bool is_valid_as_function;
    bool is_valid_as_identifier;
};
PseudoClassMetadata pseudo_class_metadata(PseudoClass);

}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_implementation_file(JsonObject& pseudo_classes_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <LibWeb/CSS/PseudoClass.h>

namespace Web::CSS {

Optional<PseudoClass> pseudo_class_from_string(StringView string)
{
)~~~");

    pseudo_classes_data.for_each_member([&](auto& name, auto&) {
        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.append(R"~~~(
    if (string.equals_ignoring_ascii_case("@name@"sv))
        return PseudoClass::@name:titlecase@;
)~~~");
    });

    generator.append(R"~~~(

    return {};
}

StringView pseudo_class_name(PseudoClass pseudo_class)
{
    switch (pseudo_class) {
)~~~");

    pseudo_classes_data.for_each_member([&](auto& name, auto&) {
        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.append(R"~~~(
    case PseudoClass::@name:titlecase@:
        return "@name@"sv;
)~~~");
    });

    generator.append(R"~~~(
    }
    VERIFY_NOT_REACHED();
}

PseudoClassMetadata pseudo_class_metadata(PseudoClass pseudo_class)
{
    switch (pseudo_class) {
)~~~");

    pseudo_classes_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto member_generator = generator.fork();
        auto& pseudo_class = value.as_object();
        auto argument_string = pseudo_class.get_byte_string("argument"sv).value();

        bool is_valid_as_identifier = argument_string.is_empty();
        bool is_valid_as_function = !argument_string.is_empty();

        if (argument_string.ends_with('?')) {
            is_valid_as_identifier = true;
            argument_string = argument_string.substring(0, argument_string.length() - 1);
        }

        String parameter_type = "None"_string;
        if (is_valid_as_function) {
            if (argument_string == "<an+b>"sv) {
                parameter_type = "ANPlusB"_string;
            } else if (argument_string == "<an+b-of>"sv) {
                parameter_type = "ANPlusBOf"_string;
            } else if (argument_string == "<compound-selector>"sv) {
                parameter_type = "CompoundSelector"_string;
            } else if (argument_string == "<forgiving-selector-list>"sv) {
                parameter_type = "ForgivingSelectorList"_string;
            } else if (argument_string == "<forgiving-relative-selector-list>"sv) {
                parameter_type = "ForgivingRelativeSelectorList"_string;
            } else if (argument_string == "<ident>"sv) {
                parameter_type = "Ident"_string;
            } else if (argument_string == "<language-ranges>"sv) {
                parameter_type = "LanguageRanges"_string;
            } else if (argument_string == "<selector-list>"sv) {
                parameter_type = "SelectorList"_string;
            } else {
                warnln("Unrecognized pseudo-class argument type: `{}`", argument_string);
                VERIFY_NOT_REACHED();
            }
        }

        member_generator.set("name:titlecase", title_casify(name));
        member_generator.set("parameter_type", parameter_type);
        member_generator.set("is_valid_as_function", is_valid_as_function ? "true"_string : "false"_string);
        member_generator.set("is_valid_as_identifier", is_valid_as_identifier ? "true"_string : "false"_string);

        member_generator.append(R"~~~(
    case PseudoClass::@name:titlecase@:
        return {
            .parameter_type = PseudoClassMetadata::ParameterType::@parameter_type@,
            .is_valid_as_function = @is_valid_as_function@,
            .is_valid_as_identifier = @is_valid_as_identifier@,
        };
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
