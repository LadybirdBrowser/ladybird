/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../LibUnicode/GeneratorUtil.h" // FIXME: Move this somewhere common.
#include <AK/AllOf.h>
#include <AK/ByteString.h>
#include <AK/CharacterTypes.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibLocale/DateTimeFormat.h>

using HourCycleList = Vector<Locale::HourCycle>;

template<>
struct AK::Formatter<Locale::HourCycle> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Locale::HourCycle hour_cycle)
    {
        return builder.put_u64(to_underlying(hour_cycle));
    }
};

struct CLDR {
    UniqueStorage<HourCycleList> unique_hour_cycle_lists;

    HashMap<ByteString, size_t> hour_cycles;
    Vector<ByteString> hour_cycle_regions;

    HashMap<ByteString, u8> minimum_days;
    Vector<ByteString> minimum_days_regions;

    HashMap<ByteString, Locale::Weekday> first_day;
    Vector<ByteString> first_day_regions;

    HashMap<ByteString, Locale::Weekday> weekend_start;
    Vector<ByteString> weekend_start_regions;

    HashMap<ByteString, Locale::Weekday> weekend_end;
    Vector<ByteString> weekend_end_regions;
};

static ErrorOr<void> parse_hour_cycles(ByteString core_path, CLDR& cldr)
{
    // https://unicode.org/reports/tr35/tr35-dates.html#Time_Data
    LexicalPath time_data_path(move(core_path));
    time_data_path = time_data_path.append("supplemental"sv);
    time_data_path = time_data_path.append("timeData.json"sv);

    auto time_data = TRY(read_json_file(time_data_path.string()));
    auto const& supplemental_object = time_data.as_object().get_object("supplemental"sv).value();
    auto const& time_data_object = supplemental_object.get_object("timeData"sv).value();

    auto parse_hour_cycle = [](StringView hour_cycle) -> Optional<Locale::HourCycle> {
        if (hour_cycle.is_one_of("h"sv, "hb"sv, "hB"sv))
            return Locale::HourCycle::H12;
        if (hour_cycle.is_one_of("H"sv, "Hb"sv, "HB"sv))
            return Locale::HourCycle::H23;
        if (hour_cycle == "K"sv)
            return Locale::HourCycle::H11;
        if (hour_cycle == "k"sv)
            return Locale::HourCycle::H24;
        return {};
    };

    time_data_object.for_each_member([&](auto const& key, JsonValue const& value) {
        auto allowed_hour_cycles_string = value.as_object().get_byte_string("_allowed"sv).value();
        auto allowed_hour_cycles = allowed_hour_cycles_string.split_view(' ');

        Vector<Locale::HourCycle> hour_cycles;

        for (auto allowed_hour_cycle : allowed_hour_cycles) {
            if (auto hour_cycle = parse_hour_cycle(allowed_hour_cycle); hour_cycle.has_value())
                hour_cycles.append(*hour_cycle);
        }

        auto hour_cycles_index = cldr.unique_hour_cycle_lists.ensure(move(hour_cycles));
        cldr.hour_cycles.set(key, hour_cycles_index);

        if (!cldr.hour_cycle_regions.contains_slow(key))
            cldr.hour_cycle_regions.append(key);
    });

    return {};
}

static ErrorOr<void> parse_week_data(ByteString core_path, CLDR& cldr)
{
    // https://unicode.org/reports/tr35/tr35-dates.html#Week_Data
    LexicalPath week_data_path(move(core_path));
    week_data_path = week_data_path.append("supplemental"sv);
    week_data_path = week_data_path.append("weekData.json"sv);

    auto week_data = TRY(read_json_file(week_data_path.string()));
    auto const& supplemental_object = week_data.as_object().get_object("supplemental"sv).value();
    auto const& week_data_object = supplemental_object.get_object("weekData"sv).value();

    auto parse_weekday = [](StringView day) -> Locale::Weekday {
        if (day == "sun"sv)
            return Locale::Weekday::Sunday;
        if (day == "mon"sv)
            return Locale::Weekday::Monday;
        if (day == "tue"sv)
            return Locale::Weekday::Tuesday;
        if (day == "wed"sv)
            return Locale::Weekday::Wednesday;
        if (day == "thu"sv)
            return Locale::Weekday::Thursday;
        if (day == "fri"sv)
            return Locale::Weekday::Friday;
        if (day == "sat"sv)
            return Locale::Weekday::Saturday;
        VERIFY_NOT_REACHED();
    };

    auto parse_regional_weekdays = [&](auto const& region, auto const& weekday, auto& weekdays_map, auto& weekday_regions) {
        if (region.ends_with("alt-variant"sv))
            return;

        weekdays_map.set(region, parse_weekday(weekday));

        if (!weekday_regions.contains_slow(region))
            weekday_regions.append(region);
    };

    auto const& minimum_days_object = week_data_object.get_object("minDays"sv).value();
    auto const& first_day_object = week_data_object.get_object("firstDay"sv).value();
    auto const& weekend_start_object = week_data_object.get_object("weekendStart"sv).value();
    auto const& weekend_end_object = week_data_object.get_object("weekendEnd"sv).value();

    minimum_days_object.for_each_member([&](auto const& region, auto const& value) {
        auto minimum_days = value.as_string().template to_number<u8>();
        cldr.minimum_days.set(region, *minimum_days);

        if (!cldr.minimum_days_regions.contains_slow(region))
            cldr.minimum_days_regions.append(region);
    });

    first_day_object.for_each_member([&](auto const& region, auto const& value) {
        parse_regional_weekdays(region, value.as_string(), cldr.first_day, cldr.first_day_regions);
    });
    weekend_start_object.for_each_member([&](auto const& region, auto const& value) {
        parse_regional_weekdays(region, value.as_string(), cldr.weekend_start, cldr.weekend_start_regions);
    });
    weekend_end_object.for_each_member([&](auto const& region, auto const& value) {
        parse_regional_weekdays(region, value.as_string(), cldr.weekend_end, cldr.weekend_end_regions);
    });

    return {};
}

static ErrorOr<void> parse_all_locales(ByteString core_path, CLDR& cldr)
{
    TRY(parse_hour_cycles(core_path, cldr));
    TRY(parse_week_data(core_path, cldr));
    return {};
}

static ByteString format_identifier(StringView owner, ByteString identifier)
{
    identifier = identifier.replace("-"sv, "_"sv, ReplaceMode::All);
    identifier = identifier.replace("/"sv, "_"sv, ReplaceMode::All);

    if (all_of(identifier, is_ascii_digit))
        return ByteString::formatted("{}_{}", owner[0], identifier);
    if (is_ascii_lower_alpha(identifier[0]))
        return ByteString::formatted("{:c}{}", to_ascii_uppercase(identifier[0]), identifier.substring_view(1));
    return identifier;
}

static ErrorOr<void> generate_unicode_locale_header(Core::InputBufferedFile& file, CLDR& cldr)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#pragma once

#include <AK/Types.h>

namespace Locale {
)~~~");

    generate_enum(generator, format_identifier, "HourCycleRegion"sv, {}, cldr.hour_cycle_regions);
    generate_enum(generator, format_identifier, "MinimumDaysRegion"sv, {}, cldr.minimum_days_regions);
    generate_enum(generator, format_identifier, "FirstDayRegion"sv, {}, cldr.first_day_regions);
    generate_enum(generator, format_identifier, "WeekendStartRegion"sv, {}, cldr.weekend_start_regions);
    generate_enum(generator, format_identifier, "WeekendEndRegion"sv, {}, cldr.weekend_end_regions);

    generator.append(R"~~~(
}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

static ErrorOr<void> generate_unicode_locale_implementation(Core::InputBufferedFile& file, CLDR& cldr)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#include <AK/Array.h>
#include <AK/BinarySearch.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibLocale/DateTimeFormat.h>
#include <LibLocale/DateTimeFormatData.h>
#include <LibLocale/Locale.h>
#include <LibLocale/LocaleData.h>

namespace Locale {
)~~~");

    cldr.unique_hour_cycle_lists.generate(generator, cldr.unique_hour_cycle_lists.type_that_fits(), "s_hour_cycle_lists"sv);

    auto append_mapping = [&](auto const& keys, auto const& map, auto type, auto name, auto mapping_getter) {
        generator.set("type", type);
        generator.set("name", name);
        generator.set("size", ByteString::number(keys.size()));

        generator.append(R"~~~(
static constexpr Array<@type@, @size@> @name@ { {)~~~");

        bool first = true;
        for (auto const& key : keys) {
            auto const& value = map.find(key)->value;
            auto mapping = mapping_getter(value);

            generator.append(first ? " "sv : ", "sv);
            generator.append(ByteString::number(mapping));
            first = false;
        }

        generator.append(" } };");
    };

    append_mapping(cldr.hour_cycle_regions, cldr.hour_cycles, cldr.unique_hour_cycle_lists.type_that_fits(), "s_hour_cycles"sv, [](auto const& hour_cycles) { return hour_cycles; });
    append_mapping(cldr.minimum_days_regions, cldr.minimum_days, "u8"sv, "s_minimum_days"sv, [](auto minimum_days) { return minimum_days; });
    append_mapping(cldr.first_day_regions, cldr.first_day, "u8"sv, "s_first_day"sv, [](auto first_day) { return to_underlying(first_day); });
    append_mapping(cldr.weekend_start_regions, cldr.weekend_start, "u8"sv, "s_weekend_start"sv, [](auto weekend_start) { return to_underlying(weekend_start); });
    append_mapping(cldr.weekend_end_regions, cldr.weekend_end, "u8"sv, "s_weekend_end"sv, [](auto weekend_end) { return to_underlying(weekend_end); });
    generator.append("\n");

    auto append_from_string = [&](StringView enum_title, StringView enum_snake, auto const& values, Vector<Alias> const& aliases = {}) -> ErrorOr<void> {
        HashValueMap<ByteString> hashes;
        TRY(hashes.try_ensure_capacity(values.size()));

        for (auto const& value : values)
            hashes.set(value.hash(), format_identifier(enum_title, value));
        for (auto const& alias : aliases)
            hashes.set(alias.alias.hash(), format_identifier(enum_title, alias.alias));

        generate_value_from_string(generator, "{}_from_string"sv, enum_title, enum_snake, move(hashes));

        return {};
    };

    TRY(append_from_string("HourCycleRegion"sv, "hour_cycle_region"sv, cldr.hour_cycle_regions));
    TRY(append_from_string("MinimumDaysRegion"sv, "minimum_days_region"sv, cldr.minimum_days_regions));
    TRY(append_from_string("FirstDayRegion"sv, "first_day_region"sv, cldr.first_day_regions));
    TRY(append_from_string("WeekendStartRegion"sv, "weekend_start_region"sv, cldr.weekend_start_regions));
    TRY(append_from_string("WeekendEndRegion"sv, "weekend_end_region"sv, cldr.weekend_end_regions));

    generator.append(R"~~~(
Vector<HourCycle> get_regional_hour_cycles(StringView region)
{
    auto region_value = hour_cycle_region_from_string(region);
    if (!region_value.has_value())
        return {};

    auto region_index = to_underlying(*region_value);

    auto regional_hour_cycles_index = s_hour_cycles.at(region_index);
    auto const& regional_hour_cycles = s_hour_cycle_lists.at(regional_hour_cycles_index);

    Vector<HourCycle> hour_cycles;
    hour_cycles.ensure_capacity(regional_hour_cycles.size());

    for (auto hour_cycle : regional_hour_cycles)
        hour_cycles.unchecked_append(static_cast<HourCycle>(hour_cycle));

    return hour_cycles;
}
)~~~");

    auto append_regional_lookup = [&](StringView return_type, StringView lookup_type) {
        generator.set("return_type", return_type);
        generator.set("lookup_type", lookup_type);

        generator.append(R"~~~(
Optional<@return_type@> get_regional_@lookup_type@(StringView region)
{
    auto region_value = @lookup_type@_region_from_string(region);
    if (!region_value.has_value())
        return {};

    auto region_index = to_underlying(*region_value);
    auto @lookup_type@ = s_@lookup_type@.at(region_index);

    return static_cast<@return_type@>(@lookup_type@);
}
)~~~");
    };

    append_regional_lookup("u8"sv, "minimum_days"sv);
    append_regional_lookup("Weekday"sv, "first_day"sv);
    append_regional_lookup("Weekday"sv, "weekend_start"sv);
    append_regional_lookup("Weekday"sv, "weekend_end"sv);

    generator.append(R"~~~(
}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    StringView generated_header_path;
    StringView generated_implementation_path;
    StringView core_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the Unicode locale header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Unicode locale implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(core_path, "Path to cldr-core directory", "core-path", 'r', "core-path");
    args_parser.parse(arguments);

    auto generated_header_file = TRY(open_file(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(open_file(generated_implementation_path, Core::File::OpenMode::Write));

    CLDR cldr;
    TRY(parse_all_locales(core_path, cldr));

    TRY(generate_unicode_locale_header(*generated_header_file, cldr));
    TRY(generate_unicode_locale_implementation(*generated_implementation_file, cldr));

    return 0;
}
