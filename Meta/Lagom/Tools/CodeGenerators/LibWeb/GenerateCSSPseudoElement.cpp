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
        if (pseudo_element.get_bool("is-generated"_sv).value_or(false))
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
        if (pseudo_element.has("alias-for"_sv))
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
bool is_pseudo_element_root(PseudoElement);
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
        if (!pseudo_element.get_bool("is-generated"_sv).value_or(false))
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
        if (pseudo_element.has("alias-for"_sv))
            return;
        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.append(R"~~~(
    if (string.equals_ignoring_ascii_case("@name@"_sv))
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
        auto alias_for = pseudo_element.get_string("alias-for"_sv);
        if (!alias_for.has_value())
            return;

        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("alias:titlecase", title_casify(alias_for.value()));

        member_generator.append(R"~~~(
    if (string.equals_ignoring_ascii_case("@name@"_sv))
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
        if (pseudo_element.has("alias-for"_sv))
            return;
        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));

        member_generator.append(R"~~~(
    case PseudoElement::@name:titlecase@:
        return "@name@"_sv;
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
        if (pseudo_element.has("alias-for"_sv))
            return;
        if (!pseudo_element.get_bool("is-allowed-in-has"_sv).value_or(false))
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

bool is_pseudo_element_root(PseudoElement pseudo_element)
{
    switch (pseudo_element) {
)~~~");

    pseudo_elements_data.for_each_member([&](auto& name, JsonValue const& value) {
        auto& pseudo_element = value.as_object();
        if (pseudo_element.has("alias-for"_sv))
            return;
        if (!pseudo_element.get_bool("is-pseudo-root"_sv).value_or(false))
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
        if (pseudo_element.has("alias-for"_sv))
            return;
        auto property_whitelist = pseudo_element.get_array("property-whitelist"_sv);
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
            if (property.starts_with_bytes("FIXME:"_sv))
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
            if (property == "#background-properties"_sv) {
                // https://drafts.csswg.org/css-backgrounds/#property-index
                append_property("background"_sv);
                append_property("background-attachment"_sv);
                append_property("background-clip"_sv);
                append_property("background-color"_sv);
                append_property("background-image"_sv);
                append_property("background-origin"_sv);
                append_property("background-position"_sv);
                append_property("background-position-x"_sv);
                append_property("background-position-y"_sv);
                append_property("background-repeat"_sv);
                append_property("background-size"_sv);
                return;
            }
            if (property == "#border-properties"_sv) {
                // https://drafts.csswg.org/css-backgrounds/#property-index
                append_property("border"_sv);
                append_property("border-block-end"_sv);
                append_property("border-block-end-color"_sv);
                append_property("border-block-end-style"_sv);
                append_property("border-block-end-width"_sv);
                append_property("border-block-start"_sv);
                append_property("border-block-start-color"_sv);
                append_property("border-block-start-style"_sv);
                append_property("border-block-start-width"_sv);
                append_property("border-bottom"_sv);
                append_property("border-bottom-color"_sv);
                append_property("border-bottom-left-radius"_sv);
                append_property("border-bottom-right-radius"_sv);
                append_property("border-bottom-style"_sv);
                append_property("border-bottom-width"_sv);
                append_property("border-color"_sv);
                append_property("border-inline-end"_sv);
                append_property("border-inline-end-color"_sv);
                append_property("border-inline-end-style"_sv);
                append_property("border-inline-end-width"_sv);
                append_property("border-inline-start"_sv);
                append_property("border-inline-start-color"_sv);
                append_property("border-inline-start-style"_sv);
                append_property("border-inline-start-width"_sv);
                append_property("border-left"_sv);
                append_property("border-left-color"_sv);
                append_property("border-left-style"_sv);
                append_property("border-left-width"_sv);
                append_property("border-radius"_sv);
                append_property("border-right"_sv);
                append_property("border-right-color"_sv);
                append_property("border-right-style"_sv);
                append_property("border-right-width"_sv);
                append_property("border-style"_sv);
                append_property("border-top"_sv);
                append_property("border-top-color"_sv);
                append_property("border-top-left-radius"_sv);
                append_property("border-top-right-radius"_sv);
                append_property("border-top-style"_sv);
                append_property("border-top-width"_sv);
                append_property("border-width"_sv);
                return;
            }
            if (property == "#custom-properties"_sv) {
                append_property("custom"_sv);
                return;
            }
            if (property == "#font-properties"_sv) {
                // https://drafts.csswg.org/css-fonts/#property-index
                append_property("font"_sv);
                append_property("font-family"_sv);
                append_property("font-feature-settings"_sv);
                // FIXME: font-kerning
                append_property("font-language-override"_sv);
                // FIXME: font-optical-sizing
                // FIXME: font-palette
                append_property("font-size"_sv);
                // FIXME: font-size-adjust
                append_property("font-style"_sv);
                // FIXME: font-synthesis and longhands
                append_property("font-variant"_sv);
                append_property("font-variant-alternates"_sv);
                append_property("font-variant-caps"_sv);
                append_property("font-variant-east-asian"_sv);
                append_property("font-variant-emoji"_sv);
                append_property("font-variant-ligatures"_sv);
                append_property("font-variant-numeric"_sv);
                append_property("font-variant-position"_sv);
                append_property("font-variation-settings"_sv);
                append_property("font-weight"_sv);
                append_property("font-width"_sv);
                return;
            }
            if (property == "#inline-layout-properties"_sv) {
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
                append_property("line-height"_sv);
                // FIXME: text-box
                // FIXME: text-box-edge
                // FIXME: text-box-trim
                append_property("vertical-align"_sv);
                return;
            }
            if (property == "#inline-typesetting-properties"_sv) {
                // https://drafts.csswg.org/css-text-4/#property-index
                // FIXME: hanging-punctuation
                // FIXME: hyphenate-character
                // FIXME: hyphenate-limit-chars
                // FIXME: hyphenate-limit-last
                // FIXME: hyphenate-limit-lines
                // FIXME: hyphenate-limit-zone
                // FIXME: hyphens
                append_property("letter-spacing"_sv);
                // FIXME: line-break
                // FIXME: line-padding
                // FIXME: overflow-wrap
                append_property("tab-size"_sv);
                append_property("text-align"_sv);
                // FIXME: text-align-all
                // FIXME: text-align-last
                // FIXME: text-autospace
                // FIXME: text-group-align
                append_property("text-indent"_sv);
                append_property("text-justify"_sv);
                // FIXME: text-spacing
                // FIXME: text-spacing-trim
                append_property("text-transform"_sv);
                append_property("text-wrap"_sv);
                append_property("text-wrap-mode"_sv);
                append_property("text-wrap-style"_sv);
                append_property("white-space"_sv);
                append_property("white-space-collapse"_sv);
                append_property("white-space-trim"_sv);
                append_property("word-break"_sv);
                // FIXME: word-space-transform
                append_property("word-spacing"_sv);
                append_property("word-wrap"_sv);
                // FIXME: wrap-after
                // FIXME: wrap-before
                // FIXME: wrap-inside
                return;
            }
            if (property == "#margin-properties"_sv) {
                append_property("margin"_sv);
                append_property("margin-block"_sv);
                append_property("margin-block-end"_sv);
                append_property("margin-block-start"_sv);
                append_property("margin-bottom"_sv);
                append_property("margin-inline"_sv);
                append_property("margin-inline-end"_sv);
                append_property("margin-inline-start"_sv);
                append_property("margin-left"_sv);
                append_property("margin-right"_sv);
                append_property("margin-top"_sv);
                return;
            }
            if (property == "#padding-properties"_sv) {
                append_property("padding"_sv);
                append_property("padding-block"_sv);
                append_property("padding-block-end"_sv);
                append_property("padding-block-start"_sv);
                append_property("padding-bottom"_sv);
                append_property("padding-inline"_sv);
                append_property("padding-inline-end"_sv);
                append_property("padding-inline-start"_sv);
                append_property("padding-left"_sv);
                append_property("padding-right"_sv);
                append_property("padding-top"_sv);
                return;
            }
            if (property == "#text-decoration-properties"_sv) {
                append_property("text-decoration"_sv);
                append_property("text-decoration-color"_sv);
                append_property("text-decoration-line"_sv);
                append_property("text-decoration-style"_sv);
                append_property("text-decoration-thickness"_sv);
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
        if (pseudo_element.has("alias-for"_sv))
            return;

        bool is_valid_as_function = false;
        bool is_valid_as_identifier = false;
        auto const& type = pseudo_element.get_string("type"_sv);
        if (type == "function"_sv) {
            is_valid_as_function = true;
        } else if (type == "both"_sv) {
            is_valid_as_function = true;
            is_valid_as_identifier = true;
        } else {
            is_valid_as_identifier = true;
        }

        String parameter_type = "None"_string;
        if (is_valid_as_function) {
            auto const& function_syntax = pseudo_element.get_string("function-syntax"_sv).value();
            if (function_syntax == "<pt-name-selector>"_sv) {
                parameter_type = "PTNameSelector"_string;
            } else {
                warnln("Unrecognized pseudo-element parameter type: `{}`", function_syntax);
                VERIFY_NOT_REACHED();
            }
        } else if (pseudo_element.has("function-syntax"_sv)) {
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
    case PseudoElement::UnknownWebKit:
        return {
            .parameter_type = PseudoElementMetadata::ParameterType::None,
            .is_valid_as_function = false,
            .is_valid_as_identifier = true,
        };
    case PseudoElement::KnownPseudoElementCount:
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
        if (pseudo_element.has("alias-for"_sv))
            return;
        if (!pseudo_element.get_bool("is-generated"_sv).value_or(false))
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
        if (pseudo_element.has("alias-for"_sv))
            return;
        if (!pseudo_element.get_bool("is-generated"_sv).value_or(false))
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
