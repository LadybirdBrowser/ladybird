/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"

#include <AK/QuickSort.h>
#include <AK/SourceGenerator.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

ErrorOr<void> generate_header_file(JsonObject const& at_rules_data, Core::File& file);
ErrorOr<void> generate_implementation_file(JsonObject const& at_rules_data, Core::File& file);

static bool is_legacy_alias(JsonObject const& descriptor)
{
    return descriptor.has_string("legacy-alias-for"sv);
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the DescriptorID header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the DescriptorID implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
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

Vector<StringView> all_descriptors;
auto at_rule_count = 0u;

ErrorOr<void> generate_header_file(JsonObject const& at_rules_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    // DescriptorID is a set of all descriptor names used by any at-rules, so gather them up.
    HashTable<StringView> descriptors_set;
    at_rules_data.for_each_member([&](auto const&, JsonValue const& value) {
        auto const& at_rule = value.as_object();
        ++at_rule_count;

        if (auto descriptors = at_rule.get_object("descriptors"sv); descriptors.has_value()) {
            descriptors.value().for_each_member([&](auto const& descriptor_name, JsonValue const& descriptor_value) {
                if (is_legacy_alias(descriptor_value.as_object()))
                    return;
                descriptors_set.set(descriptor_name);
            });
        }
    });

    all_descriptors.ensure_capacity(descriptors_set.size());
    for (auto name : descriptors_set)
        all_descriptors.unchecked_append(name);
    quick_sort(all_descriptors);

    generator.set("at_rule_id_underlying_type", underlying_type_for_enum(at_rule_count));
    generator.set("descriptor_id_underlying_type", underlying_type_for_enum(all_descriptors.size()));

    generator.append(R"~~~(
#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

enum class AtRuleID : @at_rule_id_underlying_type@ {
)~~~");
    at_rules_data.for_each_member([&](auto const& name, auto const&) {
        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.appendln("    @name:titlecase@,");
    });
    generator.append(R"~~~(
};

FlyString to_string(AtRuleID);

enum class DescriptorID : @descriptor_id_underlying_type@ {
)~~~");
    for (auto const& descriptor_name : all_descriptors) {
        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(descriptor_name));
        member_generator.appendln("    @name:titlecase@,");
    }
    generator.append(R"~~~(
};

Optional<DescriptorID> descriptor_id_from_string(AtRuleID, StringView);
FlyString to_string(DescriptorID);

bool at_rule_supports_descriptor(AtRuleID, DescriptorID);
RefPtr<CSSStyleValue const> descriptor_initial_value(AtRuleID, DescriptorID);

struct DescriptorMetadata {
    enum class ValueType {
        // FIXME: Parse the grammar instead of hard-coding all the options!
        FamilyName,
        FontSrcList,
        OptionalDeclarationValue,
        PageSize,
        PositivePercentage,
        String,
        UnicodeRangeTokens,
    };
    Vector<Variant<Keyword, PropertyID, ValueType>> syntax;
};

DescriptorMetadata get_descriptor_metadata(AtRuleID, DescriptorID);

}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_implementation_file(JsonObject const& at_rules_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.set("at_rule_count", String::number(at_rule_count));
    generator.set("descriptor_count", String::number(all_descriptors.size()));

    generator.append(R"~~~(
#include <LibWeb/CSS/DescriptorID.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Parser/Parser.h>

namespace Web::CSS {

FlyString to_string(AtRuleID at_rule_id)
{
    switch (at_rule_id) {
)~~~");

    at_rules_data.for_each_member([&](auto const& at_rule_name, JsonValue const&) {
        auto at_rule_generator = generator.fork();
        at_rule_generator.set("at_rule", at_rule_name);
        at_rule_generator.set("at_rule:titlecase", title_casify(at_rule_name));
        at_rule_generator.append(R"~~~(
    case AtRuleID::@at_rule:titlecase@:
        return "\@@at_rule@"_fly_string;
)~~~");
    });

    generator.append(R"~~~(
    }
    VERIFY_NOT_REACHED();
}

Optional<DescriptorID> descriptor_id_from_string(AtRuleID at_rule_id, StringView string)
{
    switch (at_rule_id) {
)~~~");
    at_rules_data.for_each_member([&](auto const& at_rule_name, JsonValue const& value) {
        auto const& at_rule = value.as_object();

        auto at_rule_generator = generator.fork();
        at_rule_generator.set("at_rule:titlecase", title_casify(at_rule_name));
        at_rule_generator.append(R"~~~(
    case AtRuleID::@at_rule:titlecase@:
)~~~");

        auto const& descriptors = at_rule.get_object("descriptors"sv).value();

        descriptors.for_each_member([&](auto const& descriptor_name, JsonValue const& descriptor_value) {
            auto const& descriptor = descriptor_value.as_object();
            auto descriptor_generator = at_rule_generator.fork();

            descriptor_generator.set("descriptor", descriptor_name);
            if (auto alias_for = descriptor.get_string("legacy-alias-for"sv); alias_for.has_value()) {
                descriptor_generator.set("result:titlecase", title_casify(alias_for.value()));
            } else {
                descriptor_generator.set("result:titlecase", title_casify(descriptor_name));
            }
            descriptor_generator.append(R"~~~(
        if (string.equals_ignoring_ascii_case("@descriptor@"sv))
            return DescriptorID::@result:titlecase@;
)~~~");
        });

        at_rule_generator.append(R"~~~(
        break;
)~~~");
    });

    generator.append(R"~~~(
    }
    return {};
}

FlyString to_string(DescriptorID descriptor_id)
{
    switch (descriptor_id) {
)~~~");

    for (auto const& descriptor_name : all_descriptors) {
        auto member_generator = generator.fork();
        member_generator.set("name", descriptor_name);
        member_generator.set("name:titlecase", title_casify(descriptor_name));

        member_generator.append(R"~~~(
    case DescriptorID::@name:titlecase@:
        return "@name@"_fly_string;
)~~~");
    }

    generator.append(R"~~~(
    }
    VERIFY_NOT_REACHED();
}

bool at_rule_supports_descriptor(AtRuleID at_rule_id, DescriptorID descriptor_id)
{
    switch (at_rule_id) {
)~~~");

    at_rules_data.for_each_member([&](auto const& at_rule_name, JsonValue const& value) {
        auto const& at_rule = value.as_object();

        auto at_rule_generator = generator.fork();
        at_rule_generator.set("at_rule:titlecase", title_casify(at_rule_name));
        at_rule_generator.append(R"~~~(
    case AtRuleID::@at_rule:titlecase@:
        switch (descriptor_id) {
)~~~");

        auto const& descriptors = at_rule.get_object("descriptors"sv).value();
        descriptors.for_each_member([&](auto const& descriptor_name, JsonValue const& descriptor_value) {
            if (is_legacy_alias(descriptor_value.as_object()))
                return;

            auto descriptor_generator = at_rule_generator.fork();
            descriptor_generator.set("descriptor:titlecase", title_casify(descriptor_name));
            descriptor_generator.appendln("        case DescriptorID::@descriptor:titlecase@:");
        });

        at_rule_generator.append(R"~~~(
            return true;
        default:
            return false;
        }
)~~~");
    });

    generator.append(R"~~~(
    }
    VERIFY_NOT_REACHED();
}


RefPtr<CSSStyleValue const> descriptor_initial_value(AtRuleID at_rule_id, DescriptorID descriptor_id)
{
    if (!at_rule_supports_descriptor(at_rule_id, descriptor_id))
        return nullptr;

    static Array<Array<RefPtr<CSSStyleValue const>, @descriptor_count@>, @at_rule_count@> initial_values;
    if (auto initial_value = initial_values[to_underlying(at_rule_id)][to_underlying(descriptor_id)])
        return initial_value.release_nonnull();

    // Lazily parse initial values as needed.

    Parser::ParsingParams parsing_params;
    switch (at_rule_id) {
)~~~");

    at_rules_data.for_each_member([&](auto const& at_rule_name, JsonValue const& value) {
        auto const& at_rule = value.as_object();

        auto at_rule_generator = generator.fork();
        at_rule_generator.set("at_rule:titlecase", title_casify(at_rule_name));
        at_rule_generator.append(R"~~~(
    case AtRuleID::@at_rule:titlecase@:
        switch (descriptor_id) {
)~~~");

        auto const& descriptors = at_rule.get_object("descriptors"sv).value();
        descriptors.for_each_member([&](auto const& descriptor_name, JsonValue const& descriptor_value) {
            auto const& descriptor = descriptor_value.as_object();
            if (is_legacy_alias(descriptor))
                return;

            auto descriptor_generator = at_rule_generator.fork();
            descriptor_generator.set("descriptor:titlecase", title_casify(descriptor_name));

            if (auto initial_value = descriptor.get_string("initial"sv); initial_value.has_value()) {
                descriptor_generator.set("initial_value_string", initial_value.value());
                descriptor_generator.append(R"~~~(
        case DescriptorID::@descriptor:titlecase@: {
            auto parsed_value = parse_css_descriptor(parsing_params, AtRuleID::@at_rule:titlecase@, DescriptorID::@descriptor:titlecase@, "@initial_value_string@"sv);
            VERIFY(!parsed_value.is_null());
            auto initial_value = parsed_value.release_nonnull();
            initial_values[to_underlying(at_rule_id)][to_underlying(descriptor_id)] = initial_value;
            return initial_value;
        }
)~~~");
            } else {
                descriptor_generator.append(R"~~~(
        case DescriptorID::@descriptor:titlecase@:
            return nullptr;
)~~~");
            }
        });

        at_rule_generator.append(R"~~~(
        default:
            VERIFY_NOT_REACHED();
        }
)~~~");
    });

    generator.append(R"~~~(
    }
    VERIFY_NOT_REACHED();
}

DescriptorMetadata get_descriptor_metadata(AtRuleID at_rule_id, DescriptorID descriptor_id)
{
    switch (at_rule_id) {
)~~~");

    at_rules_data.for_each_member([&](auto const& at_rule_name, JsonValue const& value) {
        auto const& at_rule = value.as_object();

        auto at_rule_generator = generator.fork();
        at_rule_generator.set("at_rule:titlecase", title_casify(at_rule_name));
        at_rule_generator.append(R"~~~(
    case AtRuleID::@at_rule:titlecase@:
        switch (descriptor_id) {
)~~~");

        auto const& descriptors = at_rule.get_object("descriptors"sv).value();
        descriptors.for_each_member([&](auto const& descriptor_name, JsonValue const& descriptor_value) {
            auto const& descriptor = descriptor_value.as_object();
            if (is_legacy_alias(descriptor))
                return;

            auto descriptor_generator = at_rule_generator.fork();
            descriptor_generator.set("descriptor:titlecase", title_casify(descriptor_name));
            descriptor_generator.append(R"~~~(
        case DescriptorID::@descriptor:titlecase@: {
            DescriptorMetadata metadata;
)~~~");
            auto const& syntax = descriptor.get_array("syntax"sv).value();
            for (auto const& entry : syntax.values()) {
                auto option_generator = descriptor_generator.fork();
                auto const& syntax_string = entry.as_string();

                if (syntax_string.starts_with_bytes("<'"sv)) {
                    // Property
                    option_generator.set("property:titlecase"sv, title_casify(MUST(syntax_string.substring_from_byte_offset_with_shared_superstring(2, syntax_string.byte_count() - 4))));
                    option_generator.append(R"~~~(
            metadata.syntax.empend(PropertyID::@property:titlecase@);
)~~~");
                } else if (syntax_string.starts_with('<')) {
                    // Value type
                    // FIXME: Actually parse the grammar, instead of hard-coding the options!
                    auto value_type = [&syntax_string] {
                        if (syntax_string == "<family-name>"sv)
                            return "FamilyName"_string;
                        if (syntax_string == "<font-src-list>"sv)
                            return "FontSrcList"_string;
                        if (syntax_string == "<declaration-value>?"sv)
                            return "OptionalDeclarationValue"_string;
                        if (syntax_string == "<page-size>"sv)
                            return "PageSize"_string;
                        if (syntax_string == "<percentage [0,∞]>"sv)
                            return "PositivePercentage"_string;
                        if (syntax_string == "<string>"sv)
                            return "String"_string;
                        if (syntax_string == "<unicode-range-token>#"sv)
                            return "UnicodeRangeTokens"_string;
                        VERIFY_NOT_REACHED();
                    }();
                    option_generator.set("value_type"sv, value_type);
                    option_generator.append(R"~~~(
            metadata.syntax.empend(DescriptorMetadata::ValueType::@value_type@);
)~~~");

                } else {
                    // Keyword
                    option_generator.set("keyword:titlecase"sv, title_casify(syntax_string));
                    option_generator.append(R"~~~(
            metadata.syntax.empend(Keyword::@keyword:titlecase@);
)~~~");
                }
            }
            descriptor_generator.append(R"~~~(
            return metadata;
        }
)~~~");
        });

        at_rule_generator.append(R"~~~(
        default:
            VERIFY_NOT_REACHED();
        }
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
