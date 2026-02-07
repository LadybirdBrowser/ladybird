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
ErrorOr<void> generate_implementation_file(JsonObject& properties, JsonObject& logical_property_groups, ReadonlySpan<StringView> enum_names, Core::File& file);
void generate_bounds_checking_function(JsonObject& properties, SourceGenerator& parent_generator, StringView css_type_name, StringView type_name, StringView value_getter = {});
bool is_animatable_property(JsonObject& properties, StringView property_name);

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
    StringView enums_json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the PropertyID header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the PropertyID implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(properties_json_path, "Path to the properties JSON file to read from", "properties-json-path", 'j', "properties-json-path");
    args_parser.add_option(enums_json_path, "Path to the enums JSON file to read from", "enums-json-path", 'e', "enums-json-path");
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
    auto enums = TRY(read_json_object(enums_json_path));

    Vector<StringView> enum_names;
    enums.for_each_member([&enum_names](String const& key, auto const&) {
        enum_names.append(key);
    });

    replace_logical_aliases(properties, logical_property_groups);
    populate_all_property_longhands(properties);

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(properties, logical_property_groups, *generated_header_file));
    TRY(generate_implementation_file(properties, logical_property_groups, enum_names, *generated_implementation_file));

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

ErrorOr<void> generate_header_file(JsonObject& properties, JsonObject& logical_property_groups, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };
    generator.set("property_id_underlying_type", underlying_type_for_enum(properties.size()));
    generator.set("logical_property_group_underlying_type", underlying_type_for_enum(logical_property_groups.size()));
    generator.append(R"~~~(
#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/StringView.h>
#include <AK/Traits.h>
#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibWeb/CSS/AcceptedTypeRange.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/ValueType.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

enum class PropertyID : @property_id_underlying_type@ {
    Custom,
)~~~");

    Vector<String> shorthand_property_ids;
    Vector<String> inherited_longhand_property_ids;
    Vector<String> noninherited_longhand_property_ids;

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        // Legacy aliases don't get a PropertyID
        if (is_legacy_alias(value.as_object()))
            return;
        auto inherited = value.as_object().get_bool("inherited"sv);
        if (value.as_object().has("longhands"sv)) {
            if (inherited.has_value()) {
                dbgln("Property '{}' with longhands cannot specify 'inherited'", name);
                VERIFY_NOT_REACHED();
            }
            shorthand_property_ids.append(name);
        } else {
            if (!inherited.has_value()) {
                dbgln("Property '{}' is missing 'inherited'", name);
                VERIFY_NOT_REACHED();
            }

            if (inherited.value())
                inherited_longhand_property_ids.append(name);
            else
                noninherited_longhand_property_ids.append(name);
        }
    });

    // Section order:
    // 1. shorthand properties
    // 2. inherited longhand properties
    // 3. noninherited longhand properties

    auto first_property_id = shorthand_property_ids.first();
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

    emit_properties(shorthand_property_ids);
    emit_properties(inherited_longhand_property_ids);
    emit_properties(noninherited_longhand_property_ids);

    generator.set("first_property_id", title_casify(first_property_id));
    generator.set("last_property_id", title_casify(last_property_id));

    generator.set("first_longhand_property_id", title_casify(inherited_longhand_property_ids.first()));
    generator.set("last_longhand_property_id", title_casify(noninherited_longhand_property_ids.last()));

    generator.set("first_inherited_property_id", title_casify(inherited_longhand_property_ids.first()));
    generator.set("last_inherited_property_id", title_casify(inherited_longhand_property_ids.last()));

    // FIXME: property_accepts_{number,percentage}() has a different range from accepted_type_ranges() despite the names sounding similar.
    generator.append(R"~~~(
};

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
WEB_API Optional<PropertyID> property_id_from_string(StringView);
[[nodiscard]] WEB_API FlyString const& string_from_property_id(PropertyID);
[[nodiscard]] FlyString const& camel_case_string_from_property_id(PropertyID);
WEB_API bool is_inherited_property(PropertyID);
NonnullRefPtr<StyleValue const> property_initial_value(PropertyID);

enum class PropertyMultiplicity {
    Single,
    List,
    CoordinatingList,
};
PropertyMultiplicity property_multiplicity(PropertyID);
bool property_is_single_valued(PropertyID);
bool property_is_list_valued(PropertyID);

bool property_accepts_type(PropertyID, ValueType);
AcceptedTypeRangeMap property_accepted_type_ranges(PropertyID);
bool property_accepts_keyword(PropertyID, Keyword);
Optional<Keyword> resolve_legacy_value_alias(PropertyID, Keyword);
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
Vector<PropertyID> const& longhands_for_shorthand(PropertyID);
Vector<PropertyID> const& expanded_longhands_for_shorthand(PropertyID);
bool property_maps_to_shorthand(PropertyID);
Vector<PropertyID> const& shorthands_for_longhand(PropertyID);
Vector<PropertyID> const& property_computation_order();
bool property_is_positional_value_list_shorthand(PropertyID);

bool property_requires_computation_with_inherited_value(PropertyID);
bool property_requires_computation_with_initial_value(PropertyID);
bool property_requires_computation_with_cascaded_value(PropertyID);

size_t property_maximum_value_count(PropertyID);

bool property_affects_layout(PropertyID);
bool property_affects_stacking_context(PropertyID);
bool property_needs_layout_for_getcomputedstyle(PropertyID);
bool property_needs_layout_node_for_resolved_value(PropertyID);

constexpr PropertyID first_property_id = PropertyID::@first_property_id@;
constexpr PropertyID last_property_id = PropertyID::@last_property_id@;
constexpr PropertyID first_inherited_property_id = PropertyID::@first_inherited_property_id@;
constexpr PropertyID last_inherited_property_id = PropertyID::@last_inherited_property_id@;
constexpr PropertyID first_longhand_property_id = PropertyID::@first_longhand_property_id@;
constexpr PropertyID last_longhand_property_id = PropertyID::@last_longhand_property_id@;
constexpr size_t number_of_longhand_properties = to_underlying(last_longhand_property_id) - to_underlying(first_longhand_property_id) + 1;

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

enum class LogicalPropertyGroup : @logical_property_group_underlying_type@ {
)~~~");

    logical_property_groups.for_each_member([&](auto& name, auto&) {
        generator.set("logical_property_group_name:titlecase", title_casify(name));
        generator.append(R"~~~(
    @logical_property_group_name:titlecase@,
)~~~");
    });

    generator.append(R"~~~(
};

Optional<LogicalPropertyGroup> logical_property_group_for_property(PropertyID);

} // namespace Web::CSS

namespace AK {

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

void generate_bounds_checking_function(JsonObject& properties, SourceGenerator& parent_generator, StringView css_type_name, StringView type_name, StringView value_getter)
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
                        property_generator.set("value_number", value_string);
                        property_generator.set("value_getter", value_getter);
                        property_generator.set("comparator", comparator);
                        property_generator.append("@value_getter@ @comparator@ @value_number@");
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

ErrorOr<void> generate_implementation_file(JsonObject& properties, JsonObject& logical_property_groups, ReadonlySpan<StringView> enum_names, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <AK/Assertions.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::CSS {

static auto generate_camel_case_property_table()
{
    HashMap<StringView, PropertyID, CaseInsensitiveASCIIStringViewTraits> table;
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
    table.set("@name:camelcase@"sv, PropertyID::@name:titlecase@);
)~~~");
    });

    generator.append(R"~~~(
    return table;
}

static HashMap<StringView, PropertyID, CaseInsensitiveASCIIStringViewTraits> const camel_case_properties_table = generate_camel_case_property_table();

Optional<PropertyID> property_id_from_camel_case_string(StringView string)
{
    return camel_case_properties_table.get(string);
}

static auto generate_properties_table()
{
    HashMap<StringView, PropertyID, CaseInsensitiveASCIIStringViewTraits> table;
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
    table.set("@name@"sv, PropertyID::@name:titlecase@);
)~~~");
    });

    generator.append(R"~~~(
    return table;
}

static HashMap<StringView, PropertyID, CaseInsensitiveASCIIStringViewTraits> const properties_table = generate_properties_table();

Optional<PropertyID> property_id_from_string(StringView string)
{
    if (is_a_custom_property_name_string(string))
        return PropertyID::Custom;

    return properties_table.get(string);
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
    if (property_id >= first_inherited_property_id && property_id <= last_inherited_property_id)
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

bool property_needs_layout_for_getcomputedstyle(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        if (value.as_object().get_bool("needs-layout-for-getcomputedstyle"sv).value_or(false)) {
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

bool property_needs_layout_node_for_resolved_value(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        if (value.as_object().get_bool("needs-layout-node-for-resolved-value"sv).value_or(false)) {
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

NonnullRefPtr<StyleValue const> property_initial_value(PropertyID property_id)
{
    static Array<RefPtr<StyleValue const>, to_underlying(last_property_id) + 1> initial_values;
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

PropertyMultiplicity property_multiplicity(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, JsonValue const& value) {
        auto const& property = value.as_object();
        if (auto multiplicity = property.get_string("multiplicity"sv);
            multiplicity.has_value() && multiplicity != "single"sv) {

            if (!first_is_one_of(multiplicity, "single"sv, "list"sv, "coordinating-list"sv)) {
                dbgln("'{}' is not a valid value for 'multiplicity'. Accepted values are: 'single', 'list', 'coordinating-list'", multiplicity.value());
                VERIFY_NOT_REACHED();
            }

            auto property_generator = generator.fork();
            property_generator.set("name:titlecase", title_casify(name));
            property_generator.set("multiplicity:titlecase", title_casify(multiplicity.value()));
            property_generator.appendln("    case PropertyID::@name:titlecase@:");
            property_generator.appendln("        return PropertyMultiplicity::@multiplicity:titlecase@;");
        }
    });

    generator.append(R"~~~(
    default:
        return PropertyMultiplicity::Single;
    }

    VERIFY_NOT_REACHED();
}

bool property_is_single_valued(PropertyID property_id)
{
    return !property_is_list_valued(property_id);
}

bool property_is_list_valued(PropertyID property_id)
{
    switch (property_id) {
)~~~");

    properties.for_each_member([&](auto& name, JsonValue const& value) {
        auto property = value.as_object();
        if (auto multiplicity = property.get_string("multiplicity"sv);
            multiplicity.has_value() && multiplicity != "single"sv) {

            if (!first_is_one_of(multiplicity, "list"sv, "coordinating-list"sv)) {
                dbgln("'{}' is not a valid value for 'multiplicity'. Accepted values are: 'single', 'list', 'coordinating-list'", multiplicity.value());
                VERIFY_NOT_REACHED();
            }
            auto property_generator = generator.fork();
            property_generator.set("name:titlecase", title_casify(name));
            property_generator.appendln("    case PropertyID::@name:titlecase@:");
        }
    });

    generator.append(R"~~~(
        return true;
    default:
        return false;
    }
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
                if (enum_names.contains_slow(type_name))
                    continue;

                property_generator.set("type_name", title_casify(type_name));
                property_generator.appendln("        case ValueType::@type_name@:");
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

AcceptedTypeRangeMap property_accepted_type_ranges(PropertyID property_id)
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

            StringBuilder ranges_builder;

            for (auto& type : valid_types.values()) {
                VERIFY(type.is_string());

                // Opacity values should have their calculated and interpolated values clamped to [0,1] which is
                // different from the range of allowed values [-∞,∞].
                if (type.as_string() == "opacity"sv) {
                    ranges_builder.append("{ ValueType::Number, { 0, 1 } }, { ValueType::Percentage, { 0, 100 } }"sv);
                    continue;
                }

                Vector<String> type_parts = MUST(type.as_string().split(' '));

                if (type_parts.size() < 2)
                    continue;

                auto type_name = type_parts.first();

                if (type_name == "custom-ident")
                    continue;

                // Drop the brackets on the range e.g. "[-∞,∞]" -> "-∞,∞"
                auto type_range = MUST(type_parts.get(1)->substring_from_byte_offset(1, type_parts.get(1)->byte_count() - 2));

                auto limits = MUST(type_range.split(','));

                if (limits.size() != 2)
                    VERIFY_NOT_REACHED();

                // FIXME: Use min and max values for i32 instead of float where applicable (e.g. for "integer")
                auto min = limits.get(0) == "-∞" ? "AK::NumericLimits<float>::lowest()"_string : *limits.get(0);
                auto max = limits.get(1) == "∞" ? "AK::NumericLimits<float>::max()"_string : *limits.get(1);

                if (!ranges_builder.is_empty())
                    ranges_builder.appendff(", ");

                ranges_builder.appendff("{{ ValueType::{}, {{ {}, {} }} }}", title_casify(type_name), min, max);
            }

            property_generator.set("ranges", ranges_builder.to_string_without_validation());

            property_generator.append(R"~~~(
    case PropertyID::@name:titlecase@: {
        return { @ranges@ };
    })~~~");
        }
    });

    generator.append(R"~~~(
    default: {
        return { };
    }
    }
}

bool property_accepts_keyword(PropertyID property_id, Keyword keyword)
{
    switch (property_id) {
)~~~");
    properties.for_each_member([&](auto& name, JsonValue const& value) {
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
            for (auto& keyword_value : valid_identifiers.values()) {
                auto keyword_generator = generator.fork();
                auto const& keyword_string = keyword_value.as_string();
                if (keyword_string.contains('>')) {
                    auto parts = MUST(keyword_string.split_limit('>', 2));
                    keyword_generator.set("keyword:titlecase", title_casify(parts[0]));
                } else {
                    keyword_generator.set("keyword:titlecase", title_casify(keyword_string));
                }
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
                if (!enum_names.contains_slow(type_name))
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

Optional<Keyword> resolve_legacy_value_alias(PropertyID property_id, Keyword keyword)
{
    switch (property_id) {
)~~~");
    properties.for_each_member([&](auto& name, JsonValue const& value) {
        VERIFY(value.is_object());
        auto& object = value.as_object();
        if (is_legacy_alias(object))
            return;
        if (auto maybe_valid_identifiers = object.get_array("valid-identifiers"sv); maybe_valid_identifiers.has_value() && !maybe_valid_identifiers->is_empty()) {
            auto& valid_identifiers = maybe_valid_identifiers.value();

            bool has_any_legacy_value_aliases = false;
            for (auto& keyword_value : valid_identifiers.values()) {
                if (keyword_value.as_string().contains('>')) {
                    has_any_legacy_value_aliases = true;
                    break;
                }
            }
            if (!has_any_legacy_value_aliases)
                return;

            auto property_generator = generator.fork();
            property_generator.set("name:titlecase", title_casify(name));
            property_generator.append(R"~~~(
    case PropertyID::@name:titlecase@:
        switch (keyword) {)~~~");
            for (auto& keyword_value : valid_identifiers.values()) {
                auto const& keyword_string = keyword_value.as_string();
                if (!keyword_string.contains('>'))
                    continue;

                auto keyword_generator = generator.fork();
                auto parts = MUST(keyword_string.split_limit('>', 2));
                keyword_generator.set("from_keyword:titlecase", title_casify(parts[0]));
                keyword_generator.set("to_keyword:titlecase", title_casify(parts[1]));
                keyword_generator.append(R"~~~(
        case Keyword::@from_keyword:titlecase@:
            return Keyword::@to_keyword:titlecase@;)~~~");
            }
            property_generator.append(R"~~~(
        default:
            break;
        }
        break;
)~~~");
        }
    });

    generator.append(R"~~~(
    default:
        break;
    }
    return {};
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

    generate_bounds_checking_function(properties, generator, "angle"sv, "Angle"sv, "value.raw_value()"sv);
    generate_bounds_checking_function(properties, generator, "flex"sv, "Flex"sv, "value.raw_value()"sv);
    generate_bounds_checking_function(properties, generator, "frequency"sv, "Frequency"sv, "value.raw_value()"sv);
    generate_bounds_checking_function(properties, generator, "integer"sv, "i64"sv, "value"sv);
    generate_bounds_checking_function(properties, generator, "length"sv, "Length"sv, "value.raw_value()"sv);
    generate_bounds_checking_function(properties, generator, "number"sv, "double"sv, "value"sv);
    generate_bounds_checking_function(properties, generator, "percentage"sv, "Percentage"sv, "value.value()"sv);
    generate_bounds_checking_function(properties, generator, "resolution"sv, "Resolution"sv, "value.raw_value()"sv);
    generate_bounds_checking_function(properties, generator, "time"sv, "Time"sv, "value.raw_value()"sv);

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
Vector<PropertyID> const& longhands_for_shorthand(PropertyID property_id)
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
        case PropertyID::@name:titlecase@: {
            static Vector<PropertyID> longhands = { @longhands@ };
            return longhands;
        })~~~");
        }
    });

    generator.append(R"~~~(
        default:
            static Vector<PropertyID> empty_longhands;
            return empty_longhands;
        }
}
)~~~");

    generator.append(R"~~~(
Vector<PropertyID> const& expanded_longhands_for_shorthand(PropertyID property_id)
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
    case PropertyID::@name:titlecase@: {
        static Vector<PropertyID> longhands = { @longhands@ };
        return longhands;
    })~~~");
        }
    });

    generator.append(R"~~~(
    default: {
        static Vector<PropertyID> empty_longhands;
        return empty_longhands;
    }
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
Vector<PropertyID> const& shorthands_for_longhand(PropertyID property_id)
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
    case PropertyID::@name:titlecase@: {
        static Vector<PropertyID> shorthands = { @shorthands@ };
        return shorthands;
    })~~~");
    }

    generator.append(R"~~~(
    default: {
        static Vector<PropertyID> empty_shorthands;
        return empty_shorthands;
    }
    }
}
)~~~");

    Vector<StringView> manually_specified_computation_order = {
        // math-depth is required to compute font-size
        "MathDepth"sv,

        // Font properties are required to absolutize font-relative units used in other properties, including line-height.
        "FontFamily"sv,
        "FontFeatureSettings"sv,
        "FontKerning"sv,
        "FontOpticalSizing"sv,
        "FontSize"sv,
        "FontStyle"sv,
        "FontVariantAlternates"sv,
        "FontVariantCaps"sv,
        "FontVariantEastAsian"sv,
        "FontVariantEmoji"sv,
        "FontVariantLigatures"sv,
        "FontVariantNumeric"sv,
        "FontVariantPosition"sv,
        "FontVariationSettings"sv,
        "FontWeight"sv,
        "FontWidth"sv,
        "TextRendering"sv,

        // line-height is required to absolutize `lh` units used in other properties.
        "LineHeight"sv,

        // color-scheme is included in the generic computation context in order to compute light-dark() color functions
        "ColorScheme"sv,

        // background-image is required to compute the other background-* properties
        "BackgroundImage"sv,
    };

    generator.append(R"~~~(
Vector<PropertyID> const& property_computation_order() {
    static Vector<PropertyID> order = {
)~~~");

    for (auto const& property_name : manually_specified_computation_order) {
        auto property_generator = generator.fork();
        property_generator.set("name:titlecase", property_name);
        property_generator.appendln("        PropertyID::@name:titlecase@,");
    }

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        if (value.as_object().has("longhands"sv))
            return;

        if (manually_specified_computation_order.contains_slow(title_casify(name)))
            return;

        auto property_generator = generator.fork();
        property_generator.set("name:titlecase", title_casify(name));
        property_generator.appendln("        PropertyID::@name:titlecase@,");
    });

    generator.append(R"~~~(
    };

    return order;
}
)~~~");

    generator.append(R"~~~(
bool property_is_positional_value_list_shorthand(PropertyID property_id)
{
    switch (property_id)
    {
)~~~");
    properties.for_each_member([&](auto& name, auto& value) {
        if (is_legacy_alias(value.as_object()))
            return;

        if (value.as_object().has("positional-value-list-shorthand"sv)) {
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

    Vector<StringView> properties_requiring_computation_with_inherited_value;
    Vector<StringView> properties_requiring_computation_with_initial_value;
    Vector<StringView> properties_requiring_computation_with_cascaded_value;

    properties.for_each_member([&](auto& name, auto& value) {
        VERIFY(value.is_object());
        if (is_legacy_alias(value.as_object()))
            return;

        auto const& requires_computation = value.as_object().get_string("requires-computation"sv);

        if (requires_computation.has_value() && value.as_object().has("longhands"sv)) {
            dbgln("Property '{}' is a shorthand and cannot have 'requires-computation' set.", name);
            VERIFY_NOT_REACHED();
        }

        if (value.as_object().has("longhands"sv))
            return;

        if (!requires_computation.has_value()) {
            dbgln("Property '{}' is missing 'requires-computation' field.", name);
            VERIFY_NOT_REACHED();
        }

        if (requires_computation.value() == "always"sv) {
            properties_requiring_computation_with_inherited_value.append(name);
            properties_requiring_computation_with_initial_value.append(name);
            properties_requiring_computation_with_cascaded_value.append(name);
        } else if (requires_computation.value() == "non-inherited-value"sv) {
            properties_requiring_computation_with_initial_value.append(name);
            properties_requiring_computation_with_cascaded_value.append(name);
        } else if (requires_computation.value() == "cascaded-value"sv) {
            properties_requiring_computation_with_cascaded_value.append(name);
        } else if (requires_computation.value() != "never"sv) {
            dbgln("Property '{}' has unrecognized 'requires-computation' value '{}'", name, requires_computation.value());
            VERIFY_NOT_REACHED();
        }
    });

    generator.append(R"~~~(
bool property_requires_computation_with_inherited_value(PropertyID property_id)
{
    switch(property_id) {
    )~~~");

    for (auto const& property_name : properties_requiring_computation_with_inherited_value) {
        auto property_generator = generator.fork();
        property_generator.set("name:titlecase", title_casify(property_name));
        property_generator.appendln("    case PropertyID::@name:titlecase@:");
    }

    generator.append(R"~~~(
        return true;
    default:
        return false;
    }
}

bool property_requires_computation_with_initial_value(PropertyID property_id)
{
    switch(property_id) {
    )~~~");

    for (auto const& property_name : properties_requiring_computation_with_initial_value) {
        auto property_generator = generator.fork();
        property_generator.set("name:titlecase", title_casify(property_name));
        property_generator.appendln("    case PropertyID::@name:titlecase@:");
    }

    generator.append(R"~~~(
        return true;
    default:
        return false;
    }
}

bool property_requires_computation_with_cascaded_value(PropertyID property_id)
{
    switch(property_id) {
    )~~~");

    for (auto const& property_name : properties_requiring_computation_with_cascaded_value) {
        auto property_generator = generator.fork();
        property_generator.set("name:titlecase", title_casify(property_name));
        property_generator.appendln("    case PropertyID::@name:titlecase@:");
    }

    generator.append(R"~~~(
        return true;
    default:
        return false;
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
            } else if (mapping == "block-xy"sv) {
                property_generator.set("x:titlecase", mapped_property("x"sv));
                property_generator.set("y:titlecase", mapped_property("y"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::@y:titlecase@;
        return PropertyID::@x:titlecase@;
)~~~");
            } else if (mapping == "inline-xy"sv) {
                property_generator.set("x:titlecase", mapped_property("x"sv));
                property_generator.set("y:titlecase", mapped_property("y"sv));
                property_generator.append(R"~~~(
        if (mapping_context.writing_mode == WritingMode::HorizontalTb)
            return PropertyID::@x:titlecase@;
        return PropertyID::@y:titlecase@;
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
Optional<LogicalPropertyGroup> logical_property_group_for_property(PropertyID property_id)
{
    switch(property_id) {
)~~~");

    HashMap<String, Vector<String>> logical_property_group_members;

    logical_property_groups.for_each_member([&](auto& logical_property_group_name, auto& mapping) {
        auto& group_members = logical_property_group_members.ensure(logical_property_group_name);

        mapping.as_object().for_each_member([&](auto&, auto& physical_property) {
            group_members.append(physical_property.as_string());
        });
    });

    properties.for_each_member([&](auto& property_name, auto& value) {
        if (auto maybe_logical_property_group = value.as_object().get_object("logical-alias-for"sv); maybe_logical_property_group.has_value()) {
            auto group = maybe_logical_property_group.value().get_string("group"sv).value();

            logical_property_group_members.get(group).value().append(property_name);
        }
    });

    for (auto const& logical_property_group : logical_property_group_members.keys()) {
        generator.set("logical_property_group_name:titlecase", title_casify(logical_property_group));
        for (auto const& property : logical_property_group_members.get(logical_property_group).value()) {
            generator.set("property_name:titlecase", title_casify(property));
            generator.append(R"~~~(
    case PropertyID::@property_name:titlecase@:
)~~~");
        }
        generator.append(R"~~~(
        return LogicalPropertyGroup::@logical_property_group_name:titlecase@;
)~~~");
    }

    generator.append(R"~~~(
    default:
        return {};
    }
}

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
