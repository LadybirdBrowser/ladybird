/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

ErrorOr<void> generate_header_file(JsonObject& properties, Core::File& file);
ErrorOr<void> generate_implementation_file(JsonObject& properties, Core::File& file);
ErrorOr<void> generate_idl_file(JsonObject& properties, Core::File& file);

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView generated_idl_path;
    StringView properties_json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the CSSStyleProperties header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the CSSStyleProperties implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(generated_idl_path, "Path to the CSSStyleProperties IDL file to generate", "generated-idl-path", 'i', "generated-idl-path");
    args_parser.add_option(properties_json_path, "Path to the JSON file to read from", "json-path", 'j', "json-path");
    args_parser.parse(arguments);

    auto json = TRY(read_entire_file_as_json(properties_json_path));
    VERIFY(json.is_object());
    auto properties = json.as_object();

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));
    auto generated_idl_file = TRY(Core::File::open(generated_idl_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(properties, *generated_header_file));
    TRY(generate_implementation_file(properties, *generated_implementation_file));
    TRY(generate_idl_file(properties, *generated_idl_file));

    return 0;
}

static String get_snake_case_function_name_for_css_property_name(ByteString const& name)
{
    auto snake_case_name = snake_casify(name);
    if (snake_case_name.starts_with('_'))
        return MUST(snake_case_name.substring_from_byte_offset(1));

    return snake_case_name;
}

static String make_name_acceptable_cpp(String const& name)
{
    if (name.is_one_of("float")) {
        StringBuilder builder;
        builder.append(name);
        builder.append('_');
        return MUST(builder.to_string());
    }

    return name;
}

ErrorOr<void> generate_header_file(JsonObject& properties, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#pragma once

#include <AK/String.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

class GeneratedCSSStyleProperties {
public:
)~~~");

    properties.for_each_member([&](auto& name, auto&) {
        auto declaration_generator = generator.fork();
        auto snake_case_name = get_snake_case_function_name_for_css_property_name(name);
        declaration_generator.set("name:acceptable_cpp", make_name_acceptable_cpp(snake_case_name));

        declaration_generator.append(R"~~~(
    WebIDL::ExceptionOr<void> set_@name:acceptable_cpp@(StringView value);
    String @name:acceptable_cpp@() const;
)~~~");
    });

    generator.append(R"~~~(
protected:
    GeneratedCSSStyleProperties() = default;
    virtual ~GeneratedCSSStyleProperties() = default;

    virtual CSS::CSSStyleDeclaration& generated_style_properties_to_css_style_declaration() = 0;
    CSS::CSSStyleDeclaration const& generated_style_properties_to_css_style_declaration() const { return const_cast<GeneratedCSSStyleProperties&>(*this).generated_style_properties_to_css_style_declaration(); }
}; // class GeneratedCSSStyleProperties

} // namespace Web::Bindings
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_implementation_file(JsonObject& properties, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/GeneratedCSSStyleProperties.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {
)~~~");

    properties.for_each_member([&](auto& name, auto&) {
        auto definition_generator = generator.fork();
        definition_generator.set("name", name);

        auto snake_case_name = get_snake_case_function_name_for_css_property_name(name);
        definition_generator.set("name:acceptable_cpp", make_name_acceptable_cpp(snake_case_name));

        definition_generator.append(R"~~~(
WebIDL::ExceptionOr<void> GeneratedCSSStyleProperties::set_@name:acceptable_cpp@(StringView value)
{
    return generated_style_properties_to_css_style_declaration().set_property("@name@"sv, value, ""sv);
}

String GeneratedCSSStyleProperties::@name:acceptable_cpp@() const
{
    return generated_style_properties_to_css_style_declaration().get_property_value("@name@"sv);
}
)~~~");
    });

    generator.append(R"~~~(
} // namespace Web::Bindings
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_idl_file(JsonObject& properties, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
interface mixin GeneratedCSSStyleProperties {
)~~~");

    properties.for_each_member([&](auto& name, auto&) {
        auto member_generator = generator.fork();

        member_generator.set("name", name);

        auto snake_case_name = get_snake_case_function_name_for_css_property_name(name);
        member_generator.set("name:snakecase", snake_case_name);
        member_generator.set("name:acceptable_cpp", make_name_acceptable_cpp(snake_case_name));

        // https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-camel-cased-attribute
        // For each CSS property property that is a supported CSS property, the following partial interface applies
        // where camel-cased attribute is obtained by running the CSS property to IDL attribute algorithm for property.
        // partial interface CSSStyleProperties {
        //     [CEReactions] attribute [LegacyNullToEmptyString] CSSOMString _camel_cased_attribute;
        // };
        member_generator.set("name:camelcase", css_property_to_idl_attribute(name));

        member_generator.append(R"~~~(
    [CEReactions, LegacyNullToEmptyString, AttributeCallbackName=@name:snakecase@_regular, ImplementedAs=@name:acceptable_cpp@] attribute CSSOMString @name:camelcase@;
)~~~");

        // For each CSS property property that is a supported CSS property and that begins with the string -webkit-,
        // the following partial interface applies where webkit-cased attribute is obtained by running the CSS property
        // to IDL attribute algorithm for property, with the lowercase first flag set.
        if (name.starts_with("-webkit-"sv)) {
            member_generator.set("name:webkit", css_property_to_idl_attribute(name, /* lowercase_first= */ true));
            member_generator.append(R"~~~(
    [CEReactions, LegacyNullToEmptyString, AttributeCallbackName=@name:snakecase@_webkit, ImplementedAs=@name:acceptable_cpp@] attribute CSSOMString @name:webkit@;
)~~~");
        }

        // For each CSS property property that is a supported CSS property, except for properties that have no
        // "-" (U+002D) in the property name, the following partial interface applies where dashed attribute is
        // property.
        // partial interface CSSStyleProperties {
        //     [CEReactions] attribute [LegacyNullToEmptyString] CSSOMString _dashed_attribute;
        // };
        if (name.contains('-')) {
            member_generator.append(R"~~~(
    [CEReactions, LegacyNullToEmptyString, AttributeCallbackName=@name:snakecase@_dashed, ImplementedAs=@name:acceptable_cpp@] attribute CSSOMString @name@;
)~~~");
        }
    });

    generator.append(R"~~~(
};
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}
