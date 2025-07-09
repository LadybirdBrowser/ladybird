/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/CharacterTypes.h>
#include <AK/GenericShorthands.h>
#include <AK/QuickSort.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

void replace_logical_aliases(JsonObject& properties, JsonObject& logical_property_groups);
void populate_all_property_longhands(JsonObject& properties);
ErrorOr<void> generate_header_file(JsonObject& properties, JsonObject& logical_property_groups, Core::File& file);
ErrorOr<void> generate_implementation_file(JsonObject& properties, JsonObject& logical_property_groups, Core::File& file);
void generate_bounds_checking_function(JsonObject& properties, SourceGenerator& parent_generator, StringView css_type_name, StringView type_name, Optional<StringView> default_unit_name = {}, Optional<StringView> value_getter = {});
bool is_animatable_property(JsonObject& properties, StringView property_name);

static bool type_name_is_enum(StringView type_name)
{
    return !AK::first_is_one_of(type_name,
        "angle"sv,
        "background-position"sv,
        "basic-shape"sv,
        "color"sv,
        "counter"sv,
        "custom-ident"sv,
        "easing-function"sv,
        "flex"sv,
        "fit-content"sv,
        "frequency"sv,
        "image"sv,
        "integer"sv,
        "length"sv,
        "number"sv,
        "opentype-tag"sv,
        "paint"sv,
        "percentage"sv,
        "position"sv,
        "ratio"sv,
        "rect"sv,
        "resolution"sv,
        "string"sv,
        "time"sv,
        "url"sv);
}

static bool is_legacy_alias(JsonObject const& property)
{
    return property.has_string("legacy-alias-for"sv);
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView properties_json_path;
    StringView groups_json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the PropertyID header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the PropertyID implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(properties_json_path, "Path to the properties JSON file to read from", "properties-json-path", 'j', "properties-json-path");
    args_parser.add_option(groups_json_path, "Path to the logical property groups JSON file to read from", "groups-json-path", 'g', "groups-json-path");
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

    auto properties = TRY(read_json_object(properties_json_path));
    auto logical_property_groups = TRY(read_json_object(groups_json_path));

    replace_logical_aliases(properties, logical_property_groups);
    populate_all_property_longhands(properties);

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(properties, logical_property_groups, *generated_header_file));
    TRY(generate_implementation_file(properties, logical_property_groups, *generated_implementation_file));

    return 0;
}

void replace_logical_aliases(JsonObject& properties, JsonObject& logical_property_groups)
{
    // Grab the first property in each logical group, to use as the template
    HashMap<String, String> first_property_in_logical_group;
    logical_property_groups.for_each_member([&first_property_in_logical_group](String const& name, JsonValue const& value) {
        bool found = false;
        value.as_object().for_each_member([&](String const&, JsonValue const& member_value) {
            if (found)
                return;
            first_property_in_logical_group.set(name, member_value.as_string());
            found = true;
        });
        VERIFY(found);
    });

    HashMap<String, String> logical_aliases;
    properties.for_each_member([&](String const& name, JsonValue const& value) {
        VERIFY(value.is_object());
        auto const& value_as_object = value.as_object();
        auto const logical_alias_for = value_as_object.get_object("logical-alias-for"sv);
        if (logical_alias_for.has_value()) {
            auto const& group_name = logical_alias_for->get_string("group"sv);
            if (!group_name.has_value()) {
                dbgln("Logical alias '{}' is missing its group", name);
                VERIFY_NOT_REACHED();
            }

            if (auto physical_property_name = first_property_in_logical_group.get(group_name.value()); physical_property_name.has_value()) {
                logical_aliases.set(name, physical_property_name.value());
            } else {
                dbgln("Logical property group '{}' not found! (Property: '{}')", group_name.value(), name);
                VERIFY_NOT_REACHED();
            }
        }
    });

    for (auto& [name, alias] : logical_aliases) {
        auto const maybe_alias_object = properties.get_object(alias);
        if (!maybe_alias_object.has_value()) {
            dbgln("No property '{}' found for logical alias '{}'", alias, name);
            VERIFY_NOT_REACHED();
        }
        JsonObject alias_object = maybe_alias_object.value();

        // Copy over anything the logical property overrides
        properties.get_object(name).value().for_each_member([&](auto& key, auto& value) {
            alias_object.set(key, value);
        });

        // Quirks don't carry across to logical aliases
        alias_object.remove("quirks"sv);

        properties.set(name, alias_object);
    }
}

void populate_all_property_longhands(JsonObject& properties)
{
    auto all_entry = properties.get_object("all"sv);

    VERIFY(all_entry.has_value());

    properties.for_each_member([&](auto name, auto value) {
        if (value.as_object().has_array("longhands"sv) || value.as_object().has_string("legacy-alias-for"sv) || name == "direction" || name == "unicode-bidi")
            return;

        MUST(all_entry->get_array("longhands"sv)->append(JsonValue { name }));
    });
}

ErrorOr<void> generate_header_file(JsonObject& properties, JsonObject&, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };
    generator.set("property_id_underlying_type", underlying_type_for_enum(properties.size()));
    generator.append(R"~~~(
#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/StringView.h>
#include <AK/Traits.h>
#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

enum class PropertyID : @property_id_underlying_type@ {
    Invalid,
    Custom,
)~~~");

    Vector<String> inherited_shorthand_property_ids;
    Vector<String> inherited_longhand_property_ids;
    Vector<String> noninherited_shorthand_property_ids;
    Vector<String> noninherited_longhand_property_ids;

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        // Legacy aliases don't get a PropertyID
        if (is_legacy_alias(value.as_object()))
            return;
        bool inherited = value.as_object().get_bool("inherited"sv).value_or(false);
        if (value.as_object().has("longhands"sv)) {
            if (inherited)
                inherited_shorthand_property_ids.append(name);
            else
                noninherited_shorthand_property_ids.append(name);
        } else {
            if (inherited)
                inherited_longhand_property_ids.append(name);
            else
                noninherited_longhand_property_ids.append(name);
        }
    });

    // Section order:
    // 1. inherited shorthand properties
    // 2. noninherited shorthand properties
    // 3. inherited longhand properties
    // 4. noninherited longhand properties

    auto first_property_id = inherited_shorthand_property_ids.first();
    auto last_property_id = noninherited_longhand_property_ids.last();

    auto emit_properties = [&](auto& property_ids) {
        for (auto& name : property_ids) {
            auto member_generator = generator.fork();
            member_generator.set("name:titlecase", title_casify(name));
            member_generator.append(R"~~~(
        @name:titlecase@,
)~~~");
        }
    };

    emit_properties(inherited_shorthand_property_ids);
    emit_properties(noninherited_shorthand_property_ids);
    emit_properties(inherited_longhand_property_ids);
    emit_properties(noninherited_longhand_property_ids);

    generator.set("first_property_id", title_casify(first_property_id));
    generator.set("last_property_id", title_casify(last_property_id));

    generator.set("first_longhand_property_id", title_casify(inherited_longhand_property_ids.first()));
    generator.set("last_longhand_property_id", title_casify(noninherited_longhand_property_ids.last()));

    generator.set("first_inherited_shorthand_property_id", title_casify(inherited_shorthand_property_ids.first()));
    generator.set("last_inherited_shorthand_property_id", title_casify(inherited_shorthand_property_ids.last()));
    generator.set("first_inherited_longhand_property_id", title_casify(inherited_longhand_property_ids.first()));
    generator.set("last_inherited_longhand_property_id", title_casify(inherited_longhand_property_ids.last()));

    generator.append(R"~~~(
};

using PropertyIDOrCustomPropertyName = Variant<PropertyID, FlyString>;

enum class AnimationType {
    Discrete,
    ByComputedValue,
    RepeatableList,
    Custom,
    None,
};
AnimationType animation_type_from_longhand_property(PropertyID);
bool is_animatable_property(PropertyID);

Optional<PropertyID> property_id_from_camel_case_string(StringView);
Optional<PropertyID> property_id_from_string(StringView);
[[nodiscard]] FlyString const& string_from_property_id(PropertyID);
[[nodiscard]] FlyString const& camel_case_string_from_property_id(PropertyID);
bool is_inherited_property(PropertyID);
NonnullRefPtr<CSSStyleValue const> property_initial_value(PropertyID);

enum class ValueType {
    Angle,
    BackgroundPosition,
    BasicShape,
    Color,
    Counter,
    CustomIdent,
    EasingFunction,
    FilterValueList,
    FitContent,
    Flex,
    Frequency,
    Image,
    Integer,
    Length,
    Number,
    OpenTypeTag,
    Paint,
    Percentage,
    Position,
    Ratio,
    Rect,
    Resolution,
    String,
    Time,
    Url,
};
bool property_accepts_type(PropertyID, ValueType);
bool property_accepts_keyword(PropertyID, Keyword);
Optional<ValueType> property_resolves_percentages_relative_to(PropertyID);
Vector<StringView> property_custom_ident_blacklist(PropertyID);

// These perform range-checking, but are also safe to call with properties that don't accept that type. (They'll just return false.)
bool property_accepts_angle(PropertyID, Angle const&);
bool property_accepts_flex(PropertyID, Flex const&);
bool property_accepts_frequency(PropertyID, Frequency const&);
bool property_accepts_integer(PropertyID, i64 const&);
bool property_accepts_length(PropertyID, Length const&);
bool property_accepts_number(PropertyID, double const&);
bool property_accepts_percentage(PropertyID, Percentage const&);
bool property_accepts_resolution(PropertyID, Resolution const&);
bool property_accepts_time(PropertyID, Time const&);

bool property_is_shorthand(PropertyID);
Vector<PropertyID> longhands_for_shorthand(PropertyID);
Vector<PropertyID> expanded_longhands_for_shorthand(PropertyID);
bool property_maps_to_shorthand(PropertyID);
Vector<PropertyID> shorthands_for_longhand(PropertyID);

size_t property_maximum_value_count(PropertyID);

bool property_affects_layout(PropertyID);
bool property_affects_stacking_context(PropertyID);

constexpr PropertyID first_property_id = PropertyID::@first_property_id@;
constexpr PropertyID last_property_id = PropertyID::@last_property_id@;
constexpr PropertyID first_inherited_shorthand_property_id = PropertyID::@first_inherited_shorthand_property_id@;
constexpr PropertyID last_inherited_shorthand_property_id = PropertyID::@last_inherited_shorthand_property_id@;
constexpr PropertyID first_inherited_longhand_property_id = PropertyID::@first_inherited_longhand_property_id@;
constexpr PropertyID last_inherited_longhand_property_id = PropertyID::@last_inherited_longhand_property_id@;
constexpr PropertyID first_longhand_property_id = PropertyID::@first_longhand_property_id@;
constexpr PropertyID last_longhand_property_id = PropertyID::@last_longhand_property_id@;

enum class Quirk {
    // https://quirks.spec.whatwg.org/#the-hashless-hex-color-quirk
    HashlessHexColor,
    // https://quirks.spec.whatwg.org/#the-unitless-length-quirk
    UnitlessLength,
};
bool property_has_quirk(PropertyID, Quirk);

struct LogicalAliasMappingContext {
    WritingMode writing_mode;
    Direction direction;
    // TODO: text-orientation
};
bool property_is_logical_alias(PropertyID);
PropertyID map_logical_alias_to_physical_property(PropertyID logical_property_id, LogicalAliasMappingContext const&);

} // namespace Web::CSS

namespace AK {
template<>
struct Traits<Web::CSS::PropertyID> : public DefaultTraits<Web::CSS::PropertyID> {
    static unsigned hash(Web::CSS::PropertyID property_id) { return int_hash((unsigned)property_id); }
};

template<>
struct Formatter<Web::CSS::PropertyID> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::PropertyID const& property_id)
    {
        return Formatter<StringView>::format(builder, Web::CSS::string_from_property_id(property_id));
    }
};
} // namespace AK
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

void generate_bounds_checking_function(JsonObject& properties, SourceGenerator& parent_generator, StringView css_type_name, StringView type_name, Optional<StringView> default_unit_name, Optional<StringView> value_getter)
{
    auto generator = parent_generator.fork();
    generator.set("css_type_name", css_type_name);
    generator.set("type_name", type_name);

    generator.append(R"~~~(
bool property_accepts_@css_type_name@(PropertyID property_id, [[maybe_unused]] @type_name@ const& value)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, JsonValue const& value) -> void {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;
        if (auto maybe_valid_types = value.as_object().get_array("valid-types"sv); maybe_valid_types.has_value() && !maybe_valid_types->is_empty()) {
            for (auto valid_type : maybe_valid_types->values()) {
                auto type_and_range = MUST(valid_type.as_string().split(' '));
                if (type_and_range.first() != css_type_name)
                    continue;

                auto property_generator = generator.fork();
                property_generator.set("property_name:titlecase", title_casify(name));

                property_generator.append(R"~~~(
    case PropertyID::@property_name:titlecase@:
        return )~~~");

                if (type_and_range.size() > 1) {
                    auto range = type_and_range[1];
                    VERIFY(range.starts_with('[') && range.ends_with(']') && range.contains(','));
                    auto comma_index = range.find_byte_offset(',').value();
                    StringView min_value_string = range.bytes_as_string_view().substring_view(1, comma_index - 1);
                    StringView max_value_string = range.bytes_as_string_view().substring_view(comma_index + 1, range.byte_count() - comma_index - 2);

                    // If the min/max value is infinite, we can just skip that side of the check.
                    if (min_value_string == "-∞")
                        min_value_string = {};
                    if (max_value_string == "∞")
                        max_value_string = {};

                    if (min_value_string.is_empty() && max_value_string.is_empty()) {
                        property_generator.appendln("true;");
                        break;
                    }

                    auto output_check = [&](auto& value_string, StringView comparator) {
                        if (value_getter.has_value()) {
                            property_generator.set("value_number", value_string);
                            property_generator.set("value_getter", value_getter.value());
                            property_generator.set("comparator", comparator);
                            property_generator.append("@value_getter@ @comparator@ @value_number@");
                            return;
                        }

                        GenericLexer lexer { value_string };
                        auto value_number = lexer.consume_until(is_ascii_alpha);
                        auto value_unit = lexer.consume_while(is_ascii_alpha);
                        if (value_unit.is_empty())
                            value_unit = default_unit_name.value();
                        VERIFY(lexer.is_eof());
                        property_generator.set("value_number", value_number);
                        property_generator.set("value_unit", title_casify(value_unit));
                        property_generator.set("comparator", comparator);
                        property_generator.append("value @comparator@ @type_name@(@value_number@, @type_name@::Type::@value_unit@)");
                    };

                    if (!min_value_string.is_empty())
                        output_check(min_value_string, ">="sv);
                    if (!min_value_string.is_empty() && !max_value_string.is_empty())
                        property_generator.append(" && ");
                    if (!max_value_string.is_empty())
                        output_check(max_value_string, "<="sv);
                    property_generator.appendln(";");
                } else {
                    property_generator.appendln("true;");
                }
                break;
            }
        }
    });

    generator.append(R"~~~(
    default:
        return false;
    }
}
)~~~");
}

ErrorOr<void> generate_implementation_file(JsonObject& properties, JsonObject& logical_property_groups, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <AK/Assertions.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::CSS {

Optional<PropertyID> property_id_from_camel_case_string(StringView string)
{
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());

        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:camelcase", camel_casify(name));
        if (auto legacy_alias_for = value.as_object().get_string("legacy-alias-for"sv); legacy_alias_for.has_value()) {
            member_generator.set("name:titlecase", title_casify(legacy_alias_for.value()));
        } else {
            member_generator.set("name:titlecase", title_casify(name));
        }
        member_generator.append(R"~~~(
    if (string.equals_ignoring_ascii_case("@name:camelcase@"sv))
        return PropertyID::@name:titlecase@;
)~~~");
    });

    generator.append(R"~~~(
    return {};
}

Optional<PropertyID> property_id_from_string(StringView string)
{
    if (is_a_custom_property_name_string(string))
        return PropertyID::Custom;

)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());

        auto member_generator = generator.fork();
        member_generator.set("name", name);
        if (auto legacy_alias_for = value.as_object().get_string("legacy-alias-for"sv); legacy_alias_for.has_value()) {
            member_generator.set("name:titlecase", title_casify(legacy_alias_for.value()));
        } else {
            member_generator.set("name:titlecase", title_casify(name));
        }
        member_generator.append(R"~~~(
    if (string.equals_ignoring_ascii_case("@name@"sv))
        return PropertyID::@name:titlecase@;
)~~~");
    });

    generator.append(R"~~~(
    return {};
}

FlyString const& string_from_property_id(PropertyID property_id) {
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.append(R"~~~(
    case PropertyID::@name:titlecase@: {
        static FlyString name = "@name@"_fly_string;
        return name;
    }
)~~~");
    });

    generator.append(R"~~~(
    default: {
        static FlyString invalid_property_id_string = "(invalid CSS::PropertyID)"_fly_string;
        return invalid_property_id_string;
    }
    }
}

FlyString const& camel_case_string_from_property_id(PropertyID property_id) {
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        auto member_generator = generator.fork();
        member_generator.set("name", name);
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.set("name:camelcase", camel_casify(name));
        member_generator.append(R"~~~(
    case PropertyID::@name:titlecase@: {
        static FlyString name = "@name:camelcase@"_fly_string;
        return name;
    }
)~~~");
    });

    generator.append(R"~~~(
    default: {
        static FlyString invalid_property_id_string = "(invalid CSS::PropertyID)"_fly_string;
        return invalid_property_id_string;
    }
    }
}

AnimationType animation_type_from_longhand_property(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));

        // Shorthand properties should have already been expanded before calling into this function
        if (value.as_object().has("longhands"sv)) {
            if (value.as_object().has("animation-type"sv)) {
                dbgln("Property '{}' with longhands cannot specify 'animation-type'", name);
                VERIFY_NOT_REACHED();
            }
            member_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
        VERIFY_NOT_REACHED();
)~~~");
            return;
        }

        if (!value.as_object().has("animation-type"sv)) {
            dbgln("No animation-type specified for property '{}'", name);
            VERIFY_NOT_REACHED();
        }

        auto animation_type = value.as_object().get_string("animation-type"sv).value();
        member_generator.set("value", title_casify(animation_type));
        member_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
        return AnimationType::@value@;
)~~~");
    });

    generator.append(R"~~~(
    default:
        return AnimationType::None;
    }
}

bool is_animatable_property(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        VERIFY(!name.is_empty() && !is_ascii_digit(name.bytes_as_string_view()[0])); // Ensure `PropertyKey`s are not Numbers.
        if (is_legacy_alias(value.as_object()))
            return;

        if (is_animatable_property(properties, name)) {
            auto member_generator = generator.fork();
            member_generator.set("name:titlecase", title_casify(name));
            member_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
)~~~");
        }
    });

    generator.append(R"~~~(
        return true;
    default:
        return false;
    }
}

bool is_inherited_property(PropertyID property_id)
{
    if (property_id >= first_inherited_shorthand_property_id && property_id <= last_inherited_shorthand_property_id)
        return true;
    if (property_id >= first_inherited_longhand_property_id && property_id <= last_inherited_longhand_property_id)
        return true;
    return false;
}

bool property_affects_layout(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        bool affects_layout = true;
        if (value.as_object().has("affects-layout"sv))
            affects_layout = value.as_object().get_bool("affects-layout"sv).value_or(false);

        if (affects_layout) {
            auto member_generator = generator.fork();
            member_generator.set("name:titlecase", title_casify(name));
            member_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
)~~~");
        }
    });

    generator.append(R"~~~(
        return true;
    default:
        return false;
    }
}

bool property_affects_stacking_context(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        bool affects_stacking_context = false;
        if (value.as_object().has("affects-stacking-context"sv))
            affects_stacking_context = value.as_object().get_bool("affects-stacking-context"sv).value_or(false);

        if (affects_stacking_context) {
            auto member_generator = generator.fork();
            member_generator.set("name:titlecase", title_casify(name));
            member_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
)~~~");
        }
    });

    generator.append(R"~~~(
        return true;
    default:
        return false;
    }
}

NonnullRefPtr<CSSStyleValue const> property_initial_value(PropertyID property_id)
{
    static Array<RefPtr<CSSStyleValue const>, to_underlying(last_property_id) + 1> initial_values;
    if (auto initial_value = initial_values[to_underlying(property_id)])
        return initial_value.release_nonnull();

    // Lazily parse initial values as needed.
    // This ensures the shorthands will always be able to get the initial values of their longhands.
    // This also now allows a longhand have its own longhand (like background-position-x).

    Parser::ParsingParams parsing_params;
    switch (property_id) {
)~~~");

    auto output_initial_value_code = [&](auto& name, auto& object) {
        if (!object.has("initial"sv)) {
            dbgln("No initial value specified for property '{}'", name);
            VERIFY_NOT_REACHED();
        }
        auto initial_value = object.get_string("initial"sv);
        VERIFY(initial_value.has_value());
        auto& initial_value_string = initial_value.value();

        auto member_generator = generator.fork();
        member_generator.set("name:titlecase", title_casify(name));
        member_generator.set("initial_value_string", initial_value_string);
        member_generator.append(
            R"~~~(        case PropertyID::@name:titlecase@:
        {
            auto parsed_value = parse_css_value(parsing_params, "@initial_value_string@"sv, PropertyID::@name:titlecase@);
            VERIFY(!parsed_value.is_null());
            auto initial_value = parsed_value.release_nonnull();
            initial_values[to_underlying(PropertyID::@name:titlecase@)] = initial_value;
            return initial_value;
        }
)~~~");
    };

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;
        output_initial_value_code(name, value.as_object());
    });

    generator.append(
        R"~~~(        default: VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

bool property_has_quirk(PropertyID property_id, Quirk quirk)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        if (value.as_object().has("quirks"sv)) {
            auto quirks_value = value.as_object().get_array("quirks"sv);
            VERIFY(quirks_value.has_value());
            auto& quirks = quirks_value.value();

            if (!quirks.is_empty()) {
                auto property_generator = generator.fork();
                property_generator.set("name:titlecase", title_casify(name));
                property_generator.append(R"~~~(
    case PropertyID::@name:titlecase@: {
        switch (quirk) {
)~~~");
                for (auto& quirk : quirks.values()) {
                    VERIFY(quirk.is_string());
                    auto quirk_generator = property_generator.fork();
                    quirk_generator.set("quirk:titlecase", title_casify(quirk.as_string()));
                    quirk_generator.append(R"~~~(
        case Quirk::@quirk:titlecase@:
            return true;
)~~~");
                }
                property_generator.append(R"~~~(
        default:
            return false;
        }
    }
)~~~");
            }
        }
    });

    generator.append(R"~~~(
    default:
        return false;
    }
}

bool property_accepts_type(PropertyID property_id, ValueType value_type)
{
    switch (property_id) {
)~~~");
    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        auto& object = value.as_object();
        if (is_legacy_alias(object))
            return;

        if (auto maybe_valid_types = object.get_array("valid-types"sv); maybe_valid_types.has_value() && !maybe_valid_types->is_empty()) {
            auto& valid_types = maybe_valid_types.value();
            auto property_generator = generator.fork();
            property_generator.set("name:titlecase", title_casify(name));
            property_generator.append(R"~~~(
    case PropertyID::@name:titlecase@: {
        switch (value_type) {
)~~~");

            bool did_output_accepted_type = false;
            for (auto& type : valid_types.values()) {
                VERIFY(type.is_string());
                auto type_name = MUST(type.as_string().split(' ')).first();
                if (type_name_is_enum(type_name))
                    continue;

                if (type_name == "angle") {
                    property_generator.appendln("        case ValueType::Angle:");
                } else if (type_name == "background-position") {
                    property_generator.appendln("        case ValueType::BackgroundPosition:");
                } else if (type_name == "basic-shape") {
                    property_generator.appendln("        case ValueType::BasicShape:");
                } else if (type_name == "color") {
                    property_generator.appendln("        case ValueType::Color:");
                } else if (type_name == "counter") {
                    property_generator.appendln("        case ValueType::Counter:");
                } else if (type_name == "custom-ident") {
                    property_generator.appendln("        case ValueType::CustomIdent:");
                } else if (type_name == "easing-function") {
                    property_generator.appendln("        case ValueType::EasingFunction:");
                } else if (type_name == "fit-content") {
                    property_generator.appendln("        case ValueType::FitContent:");
                } else if (type_name == "flex") {
                    property_generator.appendln("        case ValueType::Flex:");
                } else if (type_name == "frequency") {
                    property_generator.appendln("        case ValueType::Frequency:");
                } else if (type_name == "image") {
                    property_generator.appendln("        case ValueType::Image:");
                } else if (type_name == "integer") {
                    property_generator.appendln("        case ValueType::Integer:");
                } else if (type_name == "length") {
                    property_generator.appendln("        case ValueType::Length:");
                } else if (type_name == "number") {
                    property_generator.appendln("        case ValueType::Number:");
                } else if (type_name == "opentype-tag") {
                    property_generator.appendln("        case ValueType::OpenTypeTag:");
                } else if (type_name == "paint") {
                    property_generator.appendln("        case ValueType::Paint:");
                } else if (type_name == "percentage") {
                    property_generator.appendln("        case ValueType::Percentage:");
                } else if (type_name == "position") {
                    property_generator.appendln("        case ValueType::Position:");
                } else if (type_name == "ratio") {
                    property_generator.appendln("        case ValueType::Ratio:");
                } else if (type_name == "rect") {
                    property_generator.appendln("        case ValueType::Rect:");
                } else if (type_name == "resolution") {
                    property_generator.appendln("        case ValueType::Resolution:");
                } else if (type_name == "string") {
                    property_generator.appendln("        case ValueType::String:");
                } else if (type_name == "time") {
                    property_generator.appendln("        case ValueType::Time:");
                } else if (type_name == "url") {
                    property_generator.appendln("        case ValueType::Url:");
                } else {
                    VERIFY_NOT_REACHED();
                }
                did_output_accepted_type = true;
            }

            if (did_output_accepted_type)
                property_generator.appendln("            return true;");

            property_generator.append(R"~~~(
        default:
            return false;
        }
    }
)~~~");
        }
    });
    generator.append(R"~~~(
    default:
        return false;
    }
}

bool property_accepts_keyword(PropertyID property_id, Keyword keyword)
{
    switch (property_id) {
)~~~");
    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        auto& object = value.as_object();
        if (is_legacy_alias(object))
            return;

        auto property_generator = generator.fork();
        property_generator.set("name:titlecase", title_casify(name));
        property_generator.appendln("    case PropertyID::@name:titlecase@: {");

        if (auto maybe_valid_identifiers = object.get_array("valid-identifiers"sv); maybe_valid_identifiers.has_value() && !maybe_valid_identifiers->is_empty()) {
            property_generator.appendln("        switch (keyword) {");
            auto& valid_identifiers = maybe_valid_identifiers.value();
            for (auto& keyword : valid_identifiers.values()) {
                auto keyword_generator = generator.fork();
                keyword_generator.set("keyword:titlecase", title_casify(keyword.as_string()));
                keyword_generator.appendln("        case Keyword::@keyword:titlecase@:");
            }
            property_generator.append(R"~~~(
            return true;
        default:
            break;
        }
)~~~");
        }

        if (auto maybe_valid_types = object.get_array("valid-types"sv); maybe_valid_types.has_value() && !maybe_valid_types->is_empty()) {
            auto& valid_types = maybe_valid_types.value();
            for (auto& valid_type : valid_types.values()) {
                auto type_name = MUST(valid_type.as_string().split(' ')).first();
                if (!type_name_is_enum(type_name))
                    continue;

                auto type_generator = generator.fork();
                type_generator.set("type_name:snakecase", snake_casify(type_name));
                type_generator.append(R"~~~(
        if (keyword_to_@type_name:snakecase@(keyword).has_value())
            return true;
)~~~");
            }
        }
        property_generator.append(R"~~~(
        return false;
    }
)~~~");
    });
    generator.append(R"~~~(
    default:
        return false;
    }
}

Optional<ValueType> property_resolves_percentages_relative_to(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        if (auto resolved_type = value.as_object().get_string("percentages-resolve-to"sv); resolved_type.has_value()) {
            auto property_generator = generator.fork();
            property_generator.set("name:titlecase", title_casify(name));
            property_generator.set("resolved_type:titlecase", title_casify(resolved_type.value()));
            property_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
        return ValueType::@resolved_type:titlecase@;
)~~~");
        }
    });

    generator.append(R"~~~(
    default:
        return {};
    }
}

Vector<StringView> property_custom_ident_blacklist(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        auto& object = value.as_object();
        if (is_legacy_alias(object))
            return;

        // We only have a custom-ident blacklist if we accept custom idents!
        if (auto maybe_valid_types = object.get_array("valid-types"sv); maybe_valid_types.has_value() && !maybe_valid_types->is_empty()) {
            auto& valid_types = maybe_valid_types.value();
            for (auto const& valid_type : valid_types.values()) {
                auto type_and_parameters = MUST(valid_type.as_string().split(' '));
                if (type_and_parameters.first() != "custom-ident"sv || type_and_parameters.size() == 1)
                    continue;
                VERIFY(type_and_parameters.size() == 2);

                auto parameters_string = type_and_parameters[1].bytes_as_string_view();
                VERIFY(parameters_string.starts_with("!["sv) && parameters_string.ends_with(']'));
                auto blacklisted_keywords = parameters_string.substring_view(2, parameters_string.length() - 3).split_view(',');

                auto property_generator = generator.fork();
                property_generator.set("property_name:titlecase", title_casify(name));
                property_generator.append(R"~~~(
    case PropertyID::@property_name:titlecase@:
        return Vector { )~~~");
                for (auto const& keyword : blacklisted_keywords) {
                    auto value_generator = property_generator.fork();
                    value_generator.set("keyword", keyword);

                    value_generator.append("\"@keyword@\"sv, ");
                }

                property_generator.appendln("};");
            }
        }
    });

    generator.append(R"~~~(
    default:
        return {};
    }
}

size_t property_maximum_value_count(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        if (value.as_object().has("max-values"sv)) {
            JsonValue max_values = value.as_object().get("max-values"sv).release_value();
            VERIFY(max_values.is_integer<size_t>());
            auto property_generator = generator.fork();
            property_generator.set("name:titlecase", title_casify(name));
            property_generator.set("max_values", MUST(String::formatted("{}", max_values.as_integer<size_t>())));
            property_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
        return @max_values@;
)~~~");
        }
    });

    generator.append(R"~~~(
    default:
        return 1;
    }
})~~~");

    generate_bounds_checking_function(properties, generator, "angle"sv, "Angle"sv, "Deg"sv);
    generate_bounds_checking_function(properties, generator, "flex"sv, "Flex"sv, "Fr"sv);
    generate_bounds_checking_function(properties, generator, "frequency"sv, "Frequency"sv, "Hertz"sv);
    generate_bounds_checking_function(properties, generator, "integer"sv, "i64"sv, {}, "value"sv);
    generate_bounds_checking_function(properties, generator, "length"sv, "Length"sv, {}, "value.raw_value()"sv);
    generate_bounds_checking_function(properties, generator, "number"sv, "double"sv, {}, "value"sv);
    generate_bounds_checking_function(properties, generator, "percentage"sv, "Percentage"sv, {}, "value.value()"sv);
    generate_bounds_checking_function(properties, generator, "resolution"sv, "Resolution"sv, "Dpi"sv);
    generate_bounds_checking_function(properties, generator, "time"sv, "Time"sv, "S"sv);

    generator.append(R"~~~(
bool property_is_shorthand(PropertyID property_id)
{
    switch (property_id) {
)~~~");
    properties.for_each_member([&](auto& name, auto& value) {
        if (is_legacy_alias(value.as_object()))
            return;

        if (value.as_object().has("longhands"sv)) {
            auto property_generator = generator.fork();
            property_generator.set("name:titlecase", title_casify(name));
            property_generator.append(R"~~~(
        case PropertyID::@name:titlecase@:
)~~~");
        }
    });

    generator.append(R"~~~(
            return true;
        default:
            return false;
        }
}
)~~~");

    generator.append(R"~~~(
Vector<PropertyID> longhands_for_shorthand(PropertyID property_id)
{
    switch (property_id) {
)~~~");
    Function<Vector<String>(String const&)> get_longhands = [&](String const& property_id) {
        auto object = properties.get_object(property_id);
        VERIFY(object.has_value());

        auto longhands_json_array = object.value().get_array("longhands"sv);
        VERIFY(longhands_json_array.has_value());

        Vector<String> longhands;

        longhands_json_array.value().for_each([&](auto longhand_value) {
            longhands.append(longhand_value.as_string());
        });

        return longhands;
    };

    properties.for_each_member([&](auto& name, auto& value) {
        if (is_legacy_alias(value.as_object()))
            return;

        if (value.as_object().has("longhands"sv)) {
            auto property_generator = generator.fork();
            property_generator.set("name:titlecase", title_casify(name));
            StringBuilder builder;
            for (auto longhand : get_longhands(name)) {
                if (!builder.is_empty())
                    builder.append(", "sv);
                builder.appendff("PropertyID::{}", title_casify(longhand));
            }
            property_generator.set("longhands", builder.to_byte_string());
            property_generator.append(R"~~~(
        case PropertyID::@name:titlecase@:
                return { @longhands@ };
)~~~");
        }
    });

    generator.append(R"~~~(
        default:
                return { };
        }
}
)~~~");

    generator.append(R"~~~(
Vector<PropertyID> expanded_longhands_for_shorthand(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    Function<Vector<String>(String const&)> get_expanded_longhands = [&](String const& property_id) {
        Vector<String> expanded_longhands;

        for (auto const& longhand_id : get_longhands(property_id)) {

            auto property = properties.get_object(longhand_id);

            VERIFY(property.has_value());

            if (property->has_array("longhands"sv))
                expanded_longhands.extend(get_expanded_longhands(longhand_id));
            else
                expanded_longhands.append(longhand_id);
        }

        return expanded_longhands;
    };

    properties.for_each_member([&](auto& name, auto& value) {
        if (is_legacy_alias(value.as_object()))
            return;

        if (value.as_object().has("longhands"sv)) {
            auto property_generator = generator.fork();
            property_generator.set("name:titlecase", title_casify(name));
            StringBuilder builder;
            for (auto longhand : get_expanded_longhands(name)) {
                if (!builder.is_empty())
                    builder.append(", "sv);
                builder.appendff("PropertyID::{}", title_casify(longhand));
            }
            property_generator.set("longhands", builder.to_byte_string());
            property_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
        return { @longhands@ };
)~~~");
        }
    });

    generator.append(R"~~~(
    default:
        return { };
    }
}
)~~~");

    HashMap<String, Vector<String>> shorthands_for_longhand_map;

    properties.for_each_member([&](auto& name, auto& value) {
        if (is_legacy_alias(value.as_object()))
            return;

        if (value.as_object().has("longhands"sv)) {
            auto longhands = value.as_object().get("longhands"sv);
            VERIFY(longhands.has_value() && longhands->is_array());
            auto longhand_values = longhands->as_array();
            for (auto& longhand : longhand_values.values()) {
                VERIFY(longhand.is_string());
                auto& longhand_name = longhand.as_string();
                shorthands_for_longhand_map.ensure(longhand_name).append(name);
            }
        }
    });

    generator.append(R"~~~(
bool property_maps_to_shorthand(PropertyID property_id)
{
    switch (property_id) {
)~~~");
    for (auto const& longhand : shorthands_for_longhand_map.keys()) {
        auto property_generator = generator.fork();
        property_generator.set("name:titlecase", title_casify(longhand));
        property_generator.append(R"~~~(
        case PropertyID::@name:titlecase@:
)~~~");
    }

    generator.append(R"~~~(
            return true;
        default:
            return false;
        }
}
)~~~");

    generator.append(R"~~~(
Vector<PropertyID> shorthands_for_longhand(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    Function<Vector<String>(String)> get_shorthands_for_longhand = [&](auto const& longhand) {
        Vector<String> shorthands;

        for (auto const& immediate_shorthand : shorthands_for_longhand_map.get(longhand).value()) {
            shorthands.append(immediate_shorthand);

            if (shorthands_for_longhand_map.get(immediate_shorthand).has_value())
                shorthands.extend(get_shorthands_for_longhand(immediate_shorthand));
        }

        // https://www.w3.org/TR/cssom/#concept-shorthands-preferred-order
        // NOTE: The steps are performed in a order different to the spec in order to complete this in a single sort.
        AK::quick_sort(shorthands, [&](String a, String b) {
            auto shorthand_a_longhands = get_expanded_longhands(a);
            auto shorthand_b_longhands = get_expanded_longhands(b);

            // 4. Order shorthands by the number of longhand properties that map to it, with the greatest number first.
            if (shorthand_a_longhands.size() != shorthand_b_longhands.size())
                return shorthand_a_longhands.size() > shorthand_b_longhands.size();

            // 2. Move all items in shorthands that begin with "-" (U+002D) last in the list, retaining their relative order.
            if (a.starts_with_bytes("-"sv) != b.starts_with_bytes("-"sv))
                return b.starts_with_bytes("-"sv);

            // 3. Move all items in shorthands that begin with "-" (U+002D) but do not begin with "-webkit-" last in the list, retaining their relative order.
            if (a.starts_with_bytes("-webkit-"sv) != b.starts_with_bytes("-webkit-"sv))
                return a.starts_with_bytes("-webkit-"sv);

            // 1. Order shorthands lexicographically.
            return a < b;
        });

        return shorthands;
    };

    for (auto const& longhand : shorthands_for_longhand_map.keys()) {
        auto property_generator = generator.fork();
        property_generator.set("name:titlecase", title_casify(longhand));
        StringBuilder builder;
        for (auto& shorthand : get_shorthands_for_longhand(longhand)) {
            if (!builder.is_empty())
                builder.append(", "sv);
            builder.appendff("PropertyID::{}", title_casify(shorthand));
        }
        property_generator.set("shorthands", builder.to_byte_string());
        property_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
        return { @shorthands@ };
)~~~");
    }

    generator.append(R"~~~(
    default:
        return { };
    }
}
)~~~");

    generator.append(R"~~~(
bool property_is_logical_alias(PropertyID property_id)
{
    switch(property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        if (is_legacy_alias(value.as_object()))
            return;

        if (value.as_object().has("logical-alias-for"sv)) {
            auto property_generator = generator.fork();
            property_generator.set("name:titlecase", title_casify(name));
            property_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
)~~~");
        }
    });

    generator.append(R"~~~(
        return true;
    default:
        return false;
    }
}
)~~~");
    generator.append(R"~~~(
PropertyID map_logical_alias_to_physical_property(PropertyID property_id, LogicalAliasMappingContext const& mapping_context)
{
    // https://drafts.csswg.org/css-writing-modes-4/#logical-to-physical
    // FIXME: Note: The used direction depends on the computed writing-mode and text-orientation: in vertical writing
    //              modes, a text-orientation value of upright forces the used direction to ltr.
    auto used_direction = mapping_context.direction;
    switch(property_id) {
)~~~");

    properties.for_each_member([&](auto& property_name, JsonValue const& value) {
        auto& property = value.as_object();
        if (is_legacy_alias(property))
            return;

        if (auto logical_alias_for = property.get_object("logical-alias-for"sv); logical_alias_for.has_value()) {
            auto group_name = logical_alias_for->get_string("group"sv);
            auto mapping = logical_alias_for->get_string("mapping"sv);
            if (!group_name.has_value() || !mapping.has_value()) {
                dbgln("Logical alias '{}' is missing either its group or its mapping!", property_name);
                VERIFY_NOT_REACHED();
            }

            auto maybe_group = logical_property_groups.get_object(group_name.value());
            if (!maybe_group.has_value()) {
                dbgln("Logical alias '{}' has unrecognized group '{}'", property_name, group_name.value());
                VERIFY_NOT_REACHED();
            }
            auto const& group = maybe_group.value();
            auto mapped_property = [&](StringView entry_name) {
                if (auto maybe_string = group.get_string(entry_name); maybe_string.has_value()) {
                    return title_casify(maybe_string.value());
                }
                dbgln("Logical property group '{}' is missing entry for '{}', requested by property '{}'.", group_name.value(), entry_name, property_name);
                VERIFY_NOT_REACHED();
            };

            auto property_generator = generator.fork();
            property_generator.set("name:titlecase", title_casify(property_name));
            property_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
)~~~");
            if (mapping == "block-end"sv) {
                property_generator.set("left:titlecase", mapped_property("left"sv));
                property_generator.set("right:titlecase", mapped_property("right"sv));
                property_generator.set("bottom:titlecase", mapped_property("bottom"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::@bottom:titlecase@;
        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl))
            return PropertyID::@left:titlecase@;
        return PropertyID::@right:titlecase@;
)~~~");
            } else if (mapping == "block-size"sv) {
                property_generator.set("height:titlecase", mapped_property("height"sv));
                property_generator.set("width:titlecase", mapped_property("width"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::@height:titlecase@;
        return PropertyID::@width:titlecase@;
)~~~");
            } else if (mapping == "block-start"sv) {
                property_generator.set("left:titlecase", mapped_property("left"sv));
                property_generator.set("right:titlecase", mapped_property("right"sv));
                property_generator.set("top:titlecase", mapped_property("top"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::@top:titlecase@;
        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl))
            return PropertyID::@right:titlecase@;
        return PropertyID::@left:titlecase@;
)~~~");
            } else if (mapping == "end-end"sv) {
                property_generator.set("top-left:titlecase", mapped_property("top-left"sv));
                property_generator.set("bottom-left:titlecase", mapped_property("bottom-left"sv));
                property_generator.set("top-right:titlecase", mapped_property("top-right"sv));
                property_generator.set("bottom-right:titlecase", mapped_property("bottom-right"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@bottom-right:titlecase@;
            return PropertyID::@bottom-left:titlecase@;
        }

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl)) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@bottom-left:titlecase@;
            return PropertyID::@top-left:titlecase@;
        }

        if (mapping_context.writing_mode == WritingMode::VerticalLr) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@bottom-right:titlecase@;
            return PropertyID::@top-right:titlecase@;
        }

        if (used_direction == Direction::Ltr)
            return PropertyID::@top-right:titlecase@;
        return PropertyID::@bottom-right:titlecase@;
)~~~");
            } else if (mapping == "end-start"sv) {
                property_generator.set("top-left:titlecase", mapped_property("top-left"sv));
                property_generator.set("bottom-left:titlecase", mapped_property("bottom-left"sv));
                property_generator.set("top-right:titlecase", mapped_property("top-right"sv));
                property_generator.set("bottom-right:titlecase", mapped_property("bottom-right"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@bottom-left:titlecase@;
            return PropertyID::@bottom-right:titlecase@;
        }

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl)) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@top-left:titlecase@;
            return PropertyID::@bottom-left:titlecase@;
        }

        if (mapping_context.writing_mode == WritingMode::VerticalLr) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@top-right:titlecase@;
            return PropertyID::@bottom-right:titlecase@;
        }

        if (used_direction == Direction::Ltr)
            return PropertyID::@bottom-right:titlecase@;
        return PropertyID::@top-right:titlecase@;
)~~~");
            } else if (mapping == "inline-end"sv) {
                property_generator.set("left:titlecase", mapped_property("left"sv));
                property_generator.set("right:titlecase", mapped_property("right"sv));
                property_generator.set("top:titlecase", mapped_property("top"sv));
                property_generator.set("bottom:titlecase", mapped_property("bottom"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@right:titlecase@;
            return PropertyID::@left:titlecase@;
        }

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl, WritingMode::VerticalLr)) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@bottom:titlecase@;
            return PropertyID::@top:titlecase@;
        }

        if (used_direction == Direction::Ltr)
            return PropertyID::@top:titlecase@;
        return PropertyID::@bottom:titlecase@;
)~~~");
            } else if (mapping == "inline-size"sv) {
                property_generator.set("height:titlecase", mapped_property("height"sv));
                property_generator.set("width:titlecase", mapped_property("width"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::@width:titlecase@;
        return PropertyID::@height:titlecase@;
)~~~");
            } else if (mapping == "inline-start"sv) {
                property_generator.set("left:titlecase", mapped_property("left"sv));
                property_generator.set("right:titlecase", mapped_property("right"sv));
                property_generator.set("top:titlecase", mapped_property("top"sv));
                property_generator.set("bottom:titlecase", mapped_property("bottom"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@left:titlecase@;
            return PropertyID::@right:titlecase@;
        }

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl, WritingMode::VerticalLr)) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@top:titlecase@;
            return PropertyID::@bottom:titlecase@;
        }

        if (used_direction == Direction::Ltr)
            return PropertyID::@bottom:titlecase@;
        return PropertyID::@top:titlecase@;
)~~~");
            } else if (mapping == "start-end"sv) {
                property_generator.set("top-left:titlecase", mapped_property("top-left"sv));
                property_generator.set("bottom-left:titlecase", mapped_property("bottom-left"sv));
                property_generator.set("top-right:titlecase", mapped_property("top-right"sv));
                property_generator.set("bottom-right:titlecase", mapped_property("bottom-right"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@top-right:titlecase@;
            return PropertyID::@top-left:titlecase@;
        }

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl)) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@bottom-right:titlecase@;
            return PropertyID::@top-right:titlecase@;
        }

        if (mapping_context.writing_mode == WritingMode::VerticalLr) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@bottom-left:titlecase@;
            return PropertyID::@top-left:titlecase@;
        }

        if (used_direction == Direction::Ltr)
            return PropertyID::@top-left:titlecase@;
        return PropertyID::@bottom-left:titlecase@;
)~~~");
            } else if (mapping == "start-start"sv) {
                property_generator.set("top-left:titlecase", mapped_property("top-left"sv));
                property_generator.set("bottom-left:titlecase", mapped_property("bottom-left"sv));
                property_generator.set("top-right:titlecase", mapped_property("top-right"sv));
                property_generator.set("bottom-right:titlecase", mapped_property("bottom-right"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@top-left:titlecase@;
            return PropertyID::@top-right:titlecase@;
        }

        if (first_is_one_of(mapping_context.writing_mode, WritingMode::VerticalRl, WritingMode::SidewaysRl)) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@top-right:titlecase@;
            return PropertyID::@bottom-right:titlecase@;
        }

        if (mapping_context.writing_mode == WritingMode::VerticalLr) {
            if (used_direction == Direction::Ltr)
                return PropertyID::@top-left:titlecase@;
            return PropertyID::@bottom-left:titlecase@;
        }
        if (used_direction == Direction::Ltr)
            return PropertyID::@bottom-left:titlecase@;
        return PropertyID::@top-left:titlecase@;
)~~~");
            } else {
                dbgln("Logical alias '{}' has unrecognized mapping '{}'", property_name, mapping.value());
                VERIFY_NOT_REACHED();
            }
        }
    });

    generator.append(R"~~~(
    default:
        VERIFY(!property_is_logical_alias(property_id));
        return property_id;
    }
}
)~~~");

    generator.append(R"~~~(

} // namespace Web::CSS
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

bool is_animatable_property(JsonObject& properties, StringView property_name)
{
    auto property = properties.get_object(property_name);
    VERIFY(property.has_value());

    if (auto animation_type = property.value().get_string("animation-type"sv); animation_type.has_value()) {
        return animation_type != "none";
    }

    if (!property.value().has("longhands"sv)) {
        dbgln("Property '{}' must specify either 'animation-type' or 'longhands'", property_name);
        VERIFY_NOT_REACHED();
    }

    auto longhands = property.value().get_array("longhands"sv);
    VERIFY(longhands.has_value());
    for (auto const& subproperty_name : longhands->values()) {
        VERIFY(subproperty_name.is_string());
        if (is_animatable_property(properties, subproperty_name.as_string()))
            return true;
    }

    return false;
}
