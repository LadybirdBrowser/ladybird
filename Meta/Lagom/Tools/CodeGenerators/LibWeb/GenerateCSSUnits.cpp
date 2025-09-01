/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GeneratorUtil.h"
#include <AK/GenericShorthands.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibMain/Main.h>

ErrorOr<void> generate_header_file(JsonObject& dimensions_data, Core::File& file);
ErrorOr<void> generate_implementation_file(JsonObject& dimensions_data, Core::File& file);
bool json_is_valid(JsonObject& dimensions_data, StringView json_path);

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView json_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the Units header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Units implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(json_path, "Path to the JSON file to read from", "json-path", 'j', "json-path");
    args_parser.parse(arguments);

    auto json = TRY(read_entire_file_as_json(json_path));
    VERIFY(json.is_object());
    auto dimensions_data = json.as_object();

    if (!json_is_valid(dimensions_data, json_path))
        return 1;

    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));

    TRY(generate_header_file(dimensions_data, *generated_header_file));
    TRY(generate_implementation_file(dimensions_data, *generated_implementation_file));

    return 0;
}

bool json_is_valid(JsonObject& dimensions_data, StringView json_path)
{
    bool is_valid = true;
    String most_recent_dimension_name;
    dimensions_data.for_each_member([&](auto& dimension_name, JsonValue const& value) {
        // Dimensions should be in alphabetical order
        if (dimension_name.to_ascii_lowercase() < most_recent_dimension_name.to_ascii_lowercase()) {
            warnln("{}: Dimension `{}` is in the wrong position. Please keep this list alphabetical!", json_path, dimension_name);
            is_valid = false;
        }
        most_recent_dimension_name = dimension_name;

        String most_recent_unit_name;
        Optional<String> canonical_unit;
        value.as_object().for_each_member([&](auto& unit_name, JsonValue const& unit_value) {
            auto& unit = unit_value.as_object();

            // Units should be in alphabetical order
            if (unit_name.to_ascii_lowercase() < most_recent_unit_name.to_ascii_lowercase()) {
                warnln("{}: {} unit `{}` is in the wrong position. Please keep this list alphabetical!", json_path, dimension_name, unit_name);
                is_valid = false;
            }
            most_recent_unit_name = unit_name;

            // A unit must have exactly 1 of:
            // - is-canonical-unit: true
            // - number-of-canonical-unit
            // - relative-to
            bool is_canonical_unit = unit.get_bool("is-canonical-unit"sv) == true;
            auto number_of_canonical_unit = unit.get_double_with_precision_loss("number-of-canonical-unit"sv);
            auto relative_to = unit.get_string("relative-to"sv);
            auto provided_count = (is_canonical_unit ? 1 : 0) + (number_of_canonical_unit.has_value() ? 1 : 0) + (relative_to.has_value() ? 1 : 0);
            if (provided_count != 1) {
                warnln("{}: {} unit `{}` must have exactly 1 of `is-canonical-unit: true`, `number-of-canonical-unit`, or `relative-to` provided.", json_path, dimension_name, unit_name);
                is_valid = false;
            }
            // Exactly 1 canonical unit is allowed.
            if (is_canonical_unit) {
                if (canonical_unit.has_value()) {
                    warnln("{}: {} unit `{}` marked canonical, but `{}` was already. Must have exactly 1.", json_path, dimension_name, unit_name, canonical_unit.value());
                    is_valid = false;
                } else {
                    canonical_unit = unit_name;
                }
            }
            // Also, relative-to has fixed values and is only permitted for length units, at least for now.
            if (relative_to.has_value()) {
                if (dimension_name == "length"sv) {
                    if (!first_is_one_of(relative_to.value(), "font"sv, "viewport"sv)) {
                        warnln("{}: {} unit `{}` is marked as relative to `{}`, which is unsupported.", json_path, dimension_name, unit_name, relative_to.value());
                        is_valid = false;
                    }
                } else {
                    warnln("{}: {} unit `{}` is marked as relative, but only relative length units are currently supported.", json_path, dimension_name, unit_name);
                    is_valid = false;
                }
            }
        });

        // Must have a canonical unit.
        if (!canonical_unit.has_value()) {
            warnln("{}: {} has no unit marked as canonical. Must have exactly 1.", json_path, dimension_name);
            is_valid = false;
        }
    });

    return is_valid;
}

ErrorOr<void> generate_header_file(JsonObject& dimensions_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#pragma once

#include <AK/Optional.h>

namespace Web::CSS {
)~~~");

    generator.set("enum_type", underlying_type_for_enum(dimensions_data.size()));
    generator.appendln("enum class DimensionType : @enum_type@ {");
    dimensions_data.for_each_member([&](auto& name, auto&) {
        auto dimension_generator = generator.fork();
        dimension_generator.set("dimension_name:titlecase", title_casify(name));
        dimension_generator.appendln("    @dimension_name:titlecase@,");
    });
    generator.append(R"~~~(
};

Optional<DimensionType> dimension_for_unit(StringView);
)~~~");

    dimensions_data.for_each_member([&](auto& dimension_name, auto& value) {
        auto& units = value.as_object();

        auto enum_generator = generator.fork();
        enum_generator.set("dimension_name:titlecase", title_casify(dimension_name));
        enum_generator.set("dimension_name:snakecase", snake_casify(dimension_name));
        enum_generator.set("enum_type", underlying_type_for_enum(units.size()));

        enum_generator.append(R"~~~(
enum class @dimension_name:titlecase@Unit : @enum_type@ {
)~~~");
        units.for_each_member([&](auto& unit_name, auto&) {
            auto unit_generator = enum_generator.fork();
            unit_generator.set("unit_name:titlecase", title_casify(unit_name));
            unit_generator.appendln("    @unit_name:titlecase@,");
        });
        enum_generator.append(R"~~~(
};
Optional<@dimension_name:titlecase@Unit> string_to_@dimension_name:snakecase@_unit(StringView);
StringView to_string(@dimension_name:titlecase@Unit);
double ratio_between_units(@dimension_name:titlecase@Unit, @dimension_name:titlecase@Unit);
)~~~");
    });

    generator.append(R"~~~(
bool is_absolute(LengthUnit);
bool is_font_relative(LengthUnit);
bool is_viewport_relative(LengthUnit);
inline bool is_relative(LengthUnit unit) { return !is_absolute(unit); }

}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<void> generate_implementation_file(JsonObject& dimensions_data, Core::File& file)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <AK/StringView.h>
#include <LibWeb/CSS/Units.h>

namespace Web::CSS {

Optional<DimensionType> dimension_for_unit(StringView unit_name)
{
)~~~");
    dimensions_data.for_each_member([&](String const& dimension_name, JsonValue const& units) {
        auto dimension_generator = generator.fork();
        dimension_generator.set("dimension_name:titlecase", title_casify(dimension_name));
        dimension_generator.append("    if (");
        bool first = true;
        units.as_object().for_each_member([&](String const& unit_name, auto const&) {
            auto unit_generator = dimension_generator.fork();
            unit_generator.set("unit_name", unit_name);
            if (first)
                first = false;
            else
                unit_generator.append("\n         || ");
            unit_generator.append("unit_name.equals_ignoring_ascii_case(\"@unit_name@\"sv)");
        });
        dimension_generator.append(R"~~~()
        return DimensionType::@dimension_name:titlecase@;
)~~~");
    });

    generator.append(R"~~~(
    return {};
}
)~~~");

    dimensions_data.for_each_member([&](String const& dimension_name, JsonValue const& dimension_data) {
        auto& units = dimension_data.as_object();

        String canonical_unit;
        units.for_each_member([&](String const& unit_name, JsonValue const& unit_value) {
            if (unit_value.as_object().get_bool("is-canonical-unit"sv) == true)
                canonical_unit = unit_name;
        });

        auto dimension_generator = generator.fork();
        dimension_generator.set("dimension_name:titlecase", title_casify(dimension_name));
        dimension_generator.set("dimension_name:snakecase", snake_casify(dimension_name));
        dimension_generator.set("canonical_unit:titlecase", title_casify(canonical_unit));

        dimension_generator.append(R"~~~(
Optional<@dimension_name:titlecase@Unit> string_to_@dimension_name:snakecase@_unit(StringView unit_name)
{
)~~~");
        units.for_each_member([&](String const& unit_name, JsonValue const&) {
            auto unit_generator = dimension_generator.fork();
            unit_generator.set("unit_name:lowercase", unit_name);
            unit_generator.set("unit_name:titlecase", title_casify(unit_name));
            unit_generator.append(R"~~~(
    if (unit_name.equals_ignoring_ascii_case("@unit_name:lowercase@"sv))
        return @dimension_name:titlecase@Unit::@unit_name:titlecase@;)~~~");
        });

        dimension_generator.append(R"~~~(
    return {};
}

StringView to_string(@dimension_name:titlecase@Unit value)
{
    switch (value) {)~~~");

        units.for_each_member([&](String const& unit_name, JsonValue const&) {
            auto unit_generator = dimension_generator.fork();
            unit_generator.set("unit_name:lowercase", unit_name);
            unit_generator.set("unit_name:titlecase", title_casify(unit_name));
            unit_generator.append(R"~~~(
    case @dimension_name:titlecase@Unit::@unit_name:titlecase@:
        return "@unit_name:lowercase@"sv;)~~~");
        });

        dimension_generator.append(R"~~~(
    default:
        VERIFY_NOT_REACHED();
    }
}

double ratio_between_units(@dimension_name:titlecase@Unit from, @dimension_name:titlecase@Unit to)
{
    if (from == to)
        return 1;

    auto ratio_to_canonical_unit = [](@dimension_name:titlecase@Unit unit) -> double {
        switch (unit) {
)~~~");
        units.for_each_member([&](String const& unit_name, JsonValue const& unit_value) {
            auto const& unit = unit_value.as_object();
            if (unit.has("relative-to"sv))
                return;
            auto unit_generator = dimension_generator.fork();
            unit_generator.set("unit_name:titlecase", title_casify(unit_name));
            if (auto ratio = unit.get_double_with_precision_loss("number-of-canonical-unit"sv); ratio.has_value()) {
                unit_generator.set("unit_ratio", String::number(ratio.value()));
            } else {
                // This must be the canonical unit, so the ratio is 1.
                unit_generator.set("unit_ratio", "1");
            }
            unit_generator.append(R"~~~(
        case @dimension_name:titlecase@Unit::@unit_name:titlecase@:
            return @unit_ratio@;
)~~~");
        });
        dimension_generator.append(R"~~~(
        default:
            // `from` is a relative unit, so this isn't valid.
            VERIFY_NOT_REACHED();
        }
    };

    if (to == @dimension_name:titlecase@Unit::@canonical_unit:titlecase@)
        return ratio_to_canonical_unit(from);
    return ratio_to_canonical_unit(from) / ratio_to_canonical_unit(to);
}
)~~~");
    });

    // And now some length-specific functions.
    auto& length_units = dimensions_data.get_object("length"sv).value();

    generator.append(R"~~~(
bool is_absolute(LengthUnit unit)
{
    switch (unit) {
)~~~");
    length_units.for_each_member([&](String const& unit_name, JsonValue const& unit_value) {
        auto& unit = unit_value.as_object();
        if (unit.has("relative-to"sv))
            return;
        auto unit_generator = generator.fork();
        unit_generator.set("unit_name:titlecase", title_casify(unit_name));
        unit_generator.appendln("    case LengthUnit::@unit_name:titlecase@:");
    });
    generator.append(R"~~~(
        return true;
    default:
        return false;
    }
}

bool is_font_relative(LengthUnit unit)
{
    switch (unit) {
)~~~");
    length_units.for_each_member([&](String const& unit_name, JsonValue const& unit_value) {
        auto& unit = unit_value.as_object();
        if (unit.get_string("relative-to"sv) != "font"sv)
            return;
        auto unit_generator = generator.fork();
        unit_generator.set("unit_name:titlecase", title_casify(unit_name));
        unit_generator.appendln("    case LengthUnit::@unit_name:titlecase@:");
    });
    generator.append(R"~~~(
        return true;
    default:
        return false;
    }
}

bool is_viewport_relative(LengthUnit unit)
{
    switch (unit) {
)~~~");
    length_units.for_each_member([&](String const& unit_name, JsonValue const& unit_value) {
        auto& unit = unit_value.as_object();
        if (unit.get_string("relative-to"sv) != "viewport"sv)
            return;
        auto unit_generator = generator.fork();
        unit_generator.set("unit_name:titlecase", title_casify(unit_name));
        unit_generator.appendln("    case LengthUnit::@unit_name:titlecase@:");
    });
    generator.append(R"~~~(
        return true;
    default:
        return false;
    }
}

}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}
