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
#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

enum class PseudoElement : @pseudo_element_underlying_type@ {
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        if (pseudo_element.has("alias-for"sv))
            return;

        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.appendln("    @name:titlecase@,");
    });
    generator.append(R"~~~(
    KnownPseudoElementCount,

    UnknownWebKit,
};

Optional<PseudoElement> pseudo_element_from_string(StringView);
Optional<PseudoElement> aliased_pseudo_element_from_string(StringView);
StringView pseudo_element_name(PseudoElement);

bool is_has_allowed_pseudo_element(PseudoElement);
bool pseudo_element_supports_property(PseudoElement, PropertyID);

struct PseudoElementMetadata {
    enum class ParameterType {
        None,
        PTNameSelector,
    } parameter_type;
    bool is_valid_as_function;
    bool is_valid_as_identifier;
};
PseudoElementMetadata pseudo_element_metadata(PseudoElement);

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

    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        if (pseudo_element.has("alias-for"sv))
            return;
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

Optional<PseudoElement> aliased_pseudo_element_from_string(StringView string)
{
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        auto alias_for = pseudo_element.get_string("alias-for"sv);
        if (!alias_for.has_value())
            return;

        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("alias:titlecase", title_casify(alias_for.value()));

        member_generator.append(R"~~~(
    if (string.equals_ignoring_ascii_case("@name@"sv))
        return PseudoElement::@alias:titlecase@;
)~~~");
    });

    generator.append(R"~~~(

    return {};
}

StringView pseudo_element_name(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        if (pseudo_element.has("alias-for"sv))
            return;
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
        if (pseudo_element.has("alias-for"sv))
            return;
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

bool pseudo_element_supports_property(PseudoElement pseudo_element, PropertyID property_id)
{
    switch (pseudo_element) {
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        if (pseudo_element.has("alias-for"sv))
            return;
        auto property_whitelist = pseudo_element.get_array("property-whitelist"sv);
        // No whitelist = accept everything, by falling back to the default case.
        if (!property_whitelist.has_value())
            return;

        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.append(R"~~~(
    case PseudoElement::@name:titlecase@:
        switch (property_id) {
)~~~");

        property_whitelist->for_each([&](JsonValue const& entry) {
            auto& property = entry.as_string();
            if (property.starts_with_bytes("FIXME:"sv))
                return;

            auto append_property = [&](StringView const& property_name) {
                auto property_generator = member_generator.fork();
                property_generator.set("property:titlecase", title_casify(property_name));
                property_generator.appendln("        case PropertyID::@property:titlecase@:");
            };

            if (!property.starts_with('#')) {
                append_property(property);
                return;
            }
            // Categories
            // TODO: Maybe define these in data somewhere too?
            if (property == "#background-properties"sv) {
                // https://drafts.csswg.org/css-backgrounds/#property-index
                append_property("background"sv);
                append_property("background-attachment"sv);
                append_property("background-clip"sv);
                append_property("background-color"sv);
                append_property("background-image"sv);
                append_property("background-origin"sv);
                append_property("background-position"sv);
                append_property("background-position-x"sv);
                append_property("background-position-y"sv);
                append_property("background-repeat"sv);
                append_property("background-size"sv);
                return;
            }
            if (property == "#border-properties"sv) {
                // https://drafts.csswg.org/css-backgrounds/#property-index
                append_property("border"sv);
                append_property("border-block-end"sv);
                append_property("border-block-end-color"sv);
                append_property("border-block-end-style"sv);
                append_property("border-block-end-width"sv);
                append_property("border-block-start"sv);
                append_property("border-block-start-color"sv);
                append_property("border-block-start-style"sv);
                append_property("border-block-start-width"sv);
                append_property("border-bottom"sv);
                append_property("border-bottom-color"sv);
                append_property("border-bottom-left-radius"sv);
                append_property("border-bottom-right-radius"sv);
                append_property("border-bottom-style"sv);
                append_property("border-bottom-width"sv);
                append_property("border-color"sv);
                append_property("border-inline-end"sv);
                append_property("border-inline-end-color"sv);
                append_property("border-inline-end-style"sv);
                append_property("border-inline-end-width"sv);
                append_property("border-inline-start"sv);
                append_property("border-inline-start-color"sv);
                append_property("border-inline-start-style"sv);
                append_property("border-inline-start-width"sv);
                append_property("border-left"sv);
                append_property("border-left-color"sv);
                append_property("border-left-style"sv);
                append_property("border-left-width"sv);
                append_property("border-radius"sv);
                append_property("border-right"sv);
                append_property("border-right-color"sv);
                append_property("border-right-style"sv);
                append_property("border-right-width"sv);
                append_property("border-style"sv);
                append_property("border-top"sv);
                append_property("border-top-color"sv);
                append_property("border-top-left-radius"sv);
                append_property("border-top-right-radius"sv);
                append_property("border-top-style"sv);
                append_property("border-top-width"sv);
                append_property("border-width"sv);
                return;
            }
            if (property == "#custom-properties"sv) {
                append_property("custom"sv);
                return;
            }
            if (property == "#font-properties"sv) {
                // https://drafts.csswg.org/css-fonts/#property-index
                append_property("font"sv);
                append_property("font-family"sv);
                append_property("font-feature-settings"sv);
                // FIXME: font-kerning
                append_property("font-language-override"sv);
                // FIXME: font-optical-sizing
                // FIXME: font-palette
                append_property("font-size"sv);
                // FIXME: font-size-adjust
                append_property("font-style"sv);
                // FIXME: font-synthesis and longhands
                append_property("font-variant"sv);
                append_property("font-variant-alternates"sv);
                append_property("font-variant-caps"sv);
                append_property("font-variant-east-asian"sv);
                append_property("font-variant-emoji"sv);
                append_property("font-variant-ligatures"sv);
                append_property("font-variant-numeric"sv);
                append_property("font-variant-position"sv);
                append_property("font-variation-settings"sv);
                append_property("font-weight"sv);
                append_property("font-width"sv);
                return;
            }
            if (property == "#inline-layout-properties"sv) {
                // https://drafts.csswg.org/css-inline/#property-index
                // FIXME: alignment-baseline
                // FIXME: baseline-shift
                // FIXME: baseline-source
                // FIXME: dominant-baseline
                // FIXME: initial-letter
                // FIXME: initial-letter-align
                // FIXME: initial-letter-wrap
                // FIXME: inline-sizing
                // FIXME: line-edge-fit
                append_property("line-height"sv);
                // FIXME: text-box
                // FIXME: text-box-edge
                // FIXME: text-box-trim
                append_property("vertical-align"sv);
                return;
            }
            if (property == "#inline-typesetting-properties"sv) {
                // https://drafts.csswg.org/css-text-4/#property-index
                // FIXME: hanging-punctuation
                // FIXME: hyphenate-character
                // FIXME: hyphenate-limit-chars
                // FIXME: hyphenate-limit-last
                // FIXME: hyphenate-limit-lines
                // FIXME: hyphenate-limit-zone
                // FIXME: hyphens
                append_property("letter-spacing"sv);
                // FIXME: line-break
                // FIXME: line-padding
                // FIXME: overflow-wrap
                append_property("tab-size"sv);
                append_property("text-align"sv);
                // FIXME: text-align-all
                // FIXME: text-align-last
                // FIXME: text-autospace
                // FIXME: text-group-align
                append_property("text-indent"sv);
                append_property("text-justify"sv);
                // FIXME: text-spacing
                // FIXME: text-spacing-trim
                append_property("text-transform"sv);
                // FIXME: text-wrap
                // FIXME: text-wrap-mode
                // FIXME: text-wrap-style
                append_property("white-space"sv);
                // FIXME: white-space-collapse
                // FIXME: white-space-trim
                append_property("word-break"sv);
                // FIXME: word-space-transform
                append_property("word-spacing"sv);
                append_property("word-wrap"sv);
                // FIXME: wrap-after
                // FIXME: wrap-before
                // FIXME: wrap-inside
                return;
            }
            if (property == "#margin-properties"sv) {
                append_property("margin"sv);
                append_property("margin-block"sv);
                append_property("margin-block-end"sv);
                append_property("margin-block-start"sv);
                append_property("margin-bottom"sv);
                append_property("margin-inline"sv);
                append_property("margin-inline-end"sv);
                append_property("margin-inline-start"sv);
                append_property("margin-left"sv);
                append_property("margin-right"sv);
                append_property("margin-top"sv);
                return;
            }
            if (property == "#padding-properties"sv) {
                append_property("padding"sv);
                append_property("padding-block"sv);
                append_property("padding-block-end"sv);
                append_property("padding-block-start"sv);
                append_property("padding-bottom"sv);
                append_property("padding-inline"sv);
                append_property("padding-inline-end"sv);
                append_property("padding-inline-start"sv);
                append_property("padding-left"sv);
                append_property("padding-right"sv);
                append_property("padding-top"sv);
                return;
            }
            if (property == "#text-decoration-properties"sv) {
                append_property("text-decoration"sv);
                append_property("text-decoration-color"sv);
                append_property("text-decoration-line"sv);
                append_property("text-decoration-style"sv);
                append_property("text-decoration-thickness"sv);
                return;
            }
            outln("Error: Unrecognized property group name '{}' in {}", property, name);
            exit(1);
        });

        member_generator.append(R"~~~(
            return true;
        default:
            return false;
        }
)~~~");
    });

    generator.append(R"~~~(
    default:
        return true;
    }
}

PseudoElementMetadata pseudo_element_metadata(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
)~~~");
    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        if (pseudo_element.has("alias-for"sv))
            return;

        bool is_valid_as_function = false;
        bool is_valid_as_identifier = false;
        auto const& type = pseudo_element.get_string("type"sv);
        if (type == "function"sv) {
            is_valid_as_function = true;
        } else if (type == "both"sv) {
            is_valid_as_function = true;
            is_valid_as_identifier = true;
        } else {
            is_valid_as_identifier = true;
        }

        String parameter_type = "None"_string;
        if (is_valid_as_function) {
            auto const& function_syntax = pseudo_element.get_string("function-syntax"sv).value();
            if (function_syntax == "<pt-name-selector>"sv) {
                parameter_type = "PTNameSelector"_string;
            } else {
                warnln("Unrecognized pseudo-element parameter type: `{}`", function_syntax);
                VERIFY_NOT_REACHED();
            }
        } else if (pseudo_element.has("function-syntax"sv)) {
            warnln("Pseudo-element `::{}` has `function-syntax` but is not a function type.", name);
            VERIFY_NOT_REACHED();
        }

        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.set("parameter_type", parameter_type);
        member_generator.set("is_valid_as_function", is_valid_as_function ? "true"_string : "false"_string);
        member_generator.set("is_valid_as_identifier", is_valid_as_identifier ? "true"_string : "false"_string);

        member_generator.append(R"~~~(
    case PseudoElement::@name:titlecase@:
        return {
            .parameter_type = PseudoElementMetadata::ParameterType::@parameter_type@,
            .is_valid_as_function = @is_valid_as_function@,
            .is_valid_as_identifier = @is_valid_as_identifier@,
        };
)~~~");
    });

    generator.append(R"~~~(
    case PseudoElement::KnownPseudoElementCount:
    case PseudoElement::UnknownWebKit:
        break;
    }
    VERIFY_NOT_REACHED();
}

Optional<GeneratedPseudoElement> to_generated_pseudo_element(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        if (pseudo_element.has("alias-for"sv))
            return;
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
        if (pseudo_element.has("alias-for"sv))
            return;
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
