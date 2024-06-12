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
#include <AK/Find.h>
#include <AK/Format.h>
#include <AK/HashFunctions.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <AK/Traits.h>
#include <AK/Utf8View.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibLocale/DateTimeFormat.h>
#include <LibTimeZone/TimeZone.h>

struct TimeZoneNames {
    unsigned hash() const
    {
        auto hash = pair_int_hash(short_standard_name, long_standard_name);
        hash = pair_int_hash(hash, short_daylight_name);
        hash = pair_int_hash(hash, long_daylight_name);
        hash = pair_int_hash(hash, short_generic_name);
        hash = pair_int_hash(hash, long_generic_name);

        return hash;
    }

    bool operator==(TimeZoneNames const& other) const
    {
        return (short_standard_name == other.short_standard_name)
            && (long_standard_name == other.long_standard_name)
            && (short_daylight_name == other.short_daylight_name)
            && (long_daylight_name == other.long_daylight_name)
            && (short_generic_name == other.short_generic_name)
            && (long_generic_name == other.long_generic_name);
    }

    size_t short_standard_name { 0 };
    size_t long_standard_name { 0 };

    size_t short_daylight_name { 0 };
    size_t long_daylight_name { 0 };

    size_t short_generic_name { 0 };
    size_t long_generic_name { 0 };
};

template<>
struct AK::Formatter<TimeZoneNames> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, TimeZoneNames const& time_zone)
    {
        return Formatter<FormatString>::format(builder,
            "{{ {}, {}, {}, {}, {}, {} }}"sv,
            time_zone.short_standard_name,
            time_zone.long_standard_name,
            time_zone.short_daylight_name,
            time_zone.long_daylight_name,
            time_zone.short_generic_name,
            time_zone.long_generic_name);
    }
};

template<>
struct AK::Traits<TimeZoneNames> : public DefaultTraits<TimeZoneNames> {
    static unsigned hash(TimeZoneNames const& t) { return t.hash(); }
};

struct TimeZoneFormat {
    unsigned hash() const
    {
        auto hash = int_hash(symbol_ahead_sign);
        hash = pair_int_hash(hash, symbol_ahead_separator);
        hash = pair_int_hash(hash, symbol_behind_sign);
        hash = pair_int_hash(hash, symbol_behind_separator);
        hash = pair_int_hash(hash, gmt_format);
        hash = pair_int_hash(hash, gmt_zero_format);
        return hash;
    }

    bool operator==(TimeZoneFormat const& other) const
    {
        return (symbol_ahead_sign == other.symbol_ahead_sign)
            && (symbol_ahead_separator == other.symbol_ahead_separator)
            && (symbol_behind_sign == other.symbol_behind_sign)
            && (symbol_behind_separator == other.symbol_behind_separator)
            && (gmt_format == other.gmt_format)
            && (gmt_zero_format == other.gmt_zero_format);
    }

    size_t symbol_ahead_sign { 0 };
    size_t symbol_ahead_separator { 0 };

    size_t symbol_behind_sign { 0 };
    size_t symbol_behind_separator { 0 };

    size_t gmt_format { 0 };
    size_t gmt_zero_format { 0 };
};

template<>
struct AK::Formatter<TimeZoneFormat> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, TimeZoneFormat const& time_zone_format)
    {
        return Formatter<FormatString>::format(builder, "{{ {}, {}, {}, {}, {}, {} }}"sv,
            time_zone_format.symbol_ahead_sign,
            time_zone_format.symbol_ahead_separator,
            time_zone_format.symbol_behind_sign,
            time_zone_format.symbol_behind_separator,
            time_zone_format.gmt_format,
            time_zone_format.gmt_zero_format);
    }
};

template<>
struct AK::Traits<TimeZoneFormat> : public DefaultTraits<TimeZoneFormat> {
    static unsigned hash(TimeZoneFormat const& t) { return t.hash(); }
};

using TimeZoneNamesList = Vector<size_t>;
using HourCycleList = Vector<Locale::HourCycle>;

template<>
struct AK::Formatter<Locale::HourCycle> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Locale::HourCycle hour_cycle)
    {
        return builder.put_u64(to_underlying(hour_cycle));
    }
};

struct LocaleData {
    size_t time_zones { 0 };
    size_t time_zone_formats { 0 };
};

struct CLDR {
    UniqueStringStorage unique_strings;
    UniqueStorage<TimeZoneNames> unique_time_zones;
    UniqueStorage<TimeZoneNamesList> unique_time_zone_lists;
    UniqueStorage<TimeZoneFormat> unique_time_zone_formats;
    UniqueStorage<HourCycleList> unique_hour_cycle_lists;

    HashMap<ByteString, LocaleData> locales;

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

    HashMap<ByteString, Vector<TimeZone::TimeZone>> meta_zones;
    Vector<ByteString> time_zones { "UTC"sv };
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

static ErrorOr<void> parse_meta_zones(ByteString core_path, CLDR& cldr)
{
    // https://unicode.org/reports/tr35/tr35-dates.html#Metazones
    LexicalPath meta_zone_path(move(core_path));
    meta_zone_path = meta_zone_path.append("supplemental"sv);
    meta_zone_path = meta_zone_path.append("metaZones.json"sv);

    auto meta_zone = TRY(read_json_file(meta_zone_path.string()));
    auto const& supplemental_object = meta_zone.as_object().get_object("supplemental"sv).value();
    auto const& meta_zone_object = supplemental_object.get_object("metaZones"sv).value();
    auto const& meta_zone_array = meta_zone_object.get_array("metazones"sv).value();

    meta_zone_array.for_each([&](JsonValue const& value) {
        auto const& mapping = value.as_object().get_object("mapZone"sv).value();
        auto const& meta_zone = mapping.get_byte_string("_other"sv).value();
        auto const& golden_zone = mapping.get_byte_string("_type"sv).value();

        if (auto time_zone = TimeZone::time_zone_from_string(golden_zone); time_zone.has_value()) {
            auto& golden_zones = cldr.meta_zones.ensure(meta_zone);
            golden_zones.append(*time_zone);
        }
    });

    // UTC does not appear in metaZones.json. Define it for convenience so other parsers don't need to check for its existence.
    if (auto time_zone = TimeZone::time_zone_from_string("UTC"sv); time_zone.has_value())
        cldr.meta_zones.set("UTC"sv, { *time_zone });

    return {};
}

static ErrorOr<void> parse_time_zone_names(ByteString locale_time_zone_names_path, CLDR& cldr, LocaleData& locale)
{
    LexicalPath time_zone_names_path(move(locale_time_zone_names_path));
    time_zone_names_path = time_zone_names_path.append("timeZoneNames.json"sv);

    auto time_zone_names = TRY(read_json_file(time_zone_names_path.string()));
    auto const& main_object = time_zone_names.as_object().get_object("main"sv).value();
    auto const& locale_object = main_object.get_object(time_zone_names_path.parent().basename()).value();
    auto const& dates_object = locale_object.get_object("dates"sv).value();
    auto const& time_zone_names_object = dates_object.get_object("timeZoneNames"sv).value();
    auto const& meta_zone_object = time_zone_names_object.get_object("metazone"sv);
    auto const& hour_format_string = time_zone_names_object.get_byte_string("hourFormat"sv).value();
    auto const& gmt_format_string = time_zone_names_object.get_byte_string("gmtFormat"sv).value();
    auto const& gmt_zero_format_string = time_zone_names_object.get_byte_string("gmtZeroFormat"sv).value();

    if (!meta_zone_object.has_value())
        return {};

    auto parse_name = [&](StringView type, JsonObject const& meta_zone_object, StringView key) -> Optional<size_t> {
        auto const& names = meta_zone_object.get_object(type);
        if (!names.has_value())
            return {};

        auto const& name = names->get_byte_string(key);
        if (name.has_value())
            return cldr.unique_strings.ensure(name.value());

        return {};
    };

    auto parse_hour_format = [&](auto const& format, auto& time_zone_formats) {
        auto hour_formats = format.split_view(';');

        auto hour_format_ahead_start = hour_formats[0].find('H').value();
        auto separator_ahead_start = hour_formats[0].find_last('H').value() + 1;
        auto separator_ahead_end = hour_formats[0].find('m').value();

        auto hour_format_behind_start = hour_formats[1].find('H').value();
        auto separator_behind_start = hour_formats[1].find_last('H').value() + 1;
        auto separator_behind_end = hour_formats[1].find('m').value();

        auto symbol_ahead_sign = hour_formats[0].substring_view(0, hour_format_ahead_start);
        auto symbol_ahead_separator = hour_formats[0].substring_view(separator_ahead_start, separator_ahead_end - separator_ahead_start);

        auto symbol_behind_sign = hour_formats[1].substring_view(0, hour_format_behind_start);
        auto symbol_behind_separator = hour_formats[1].substring_view(separator_behind_start, separator_behind_end - separator_behind_start);

        time_zone_formats.symbol_ahead_sign = cldr.unique_strings.ensure(symbol_ahead_sign);
        time_zone_formats.symbol_ahead_separator = cldr.unique_strings.ensure(symbol_ahead_separator);
        time_zone_formats.symbol_behind_sign = cldr.unique_strings.ensure(symbol_behind_sign);
        time_zone_formats.symbol_behind_separator = cldr.unique_strings.ensure(symbol_behind_separator);
    };

    TimeZoneNamesList time_zones;

    TimeZoneFormat time_zone_formats {};
    parse_hour_format(hour_format_string, time_zone_formats);
    time_zone_formats.gmt_format = cldr.unique_strings.ensure(gmt_format_string);
    time_zone_formats.gmt_zero_format = cldr.unique_strings.ensure(gmt_zero_format_string);

    auto parse_time_zone = [&](StringView meta_zone, JsonObject const& meta_zone_object) {
        auto golden_zones = cldr.meta_zones.find(meta_zone);
        if (golden_zones == cldr.meta_zones.end())
            return;

        TimeZoneNames time_zone_names {};

        if (auto name = parse_name("long"sv, meta_zone_object, "standard"sv); name.has_value())
            time_zone_names.long_standard_name = name.value();
        if (auto name = parse_name("short"sv, meta_zone_object, "standard"sv); name.has_value())
            time_zone_names.short_standard_name = name.value();

        if (auto name = parse_name("long"sv, meta_zone_object, "daylight"sv); name.has_value())
            time_zone_names.long_daylight_name = name.value();
        if (auto name = parse_name("short"sv, meta_zone_object, "daylight"sv); name.has_value())
            time_zone_names.short_daylight_name = name.value();

        if (auto name = parse_name("long"sv, meta_zone_object, "generic"sv); name.has_value())
            time_zone_names.long_generic_name = name.value();
        if (auto name = parse_name("short"sv, meta_zone_object, "generic"sv); name.has_value())
            time_zone_names.short_generic_name = name.value();

        auto time_zone_index = cldr.unique_time_zones.ensure(move(time_zone_names));

        for (auto golden_zone : golden_zones->value) {
            auto time_zone = to_underlying(golden_zone);
            if (time_zone >= time_zones.size())
                time_zones.resize(time_zone + 1);

            time_zones[time_zone] = time_zone_index;
        }
    };

    meta_zone_object->for_each_member([&](auto const& meta_zone, JsonValue const& value) {
        parse_time_zone(meta_zone, value.as_object());
    });

    // The long and short names for UTC are not under the "timeZoneNames/metazone" object, but are under "timeZoneNames/zone/Etc".
    auto const& zone_object = time_zone_names_object.get_object("zone"sv).value();
    auto const& etc_object = zone_object.get_object("Etc"sv).value();
    auto const& utc_object = etc_object.get_object("UTC"sv).value();
    parse_time_zone("UTC"sv, utc_object);

    locale.time_zones = cldr.unique_time_zone_lists.ensure(move(time_zones));
    locale.time_zone_formats = cldr.unique_time_zone_formats.ensure(move(time_zone_formats));

    return {};
}

static ErrorOr<void> parse_all_locales(ByteString core_path, ByteString dates_path, CLDR& cldr)
{
    TRY(parse_hour_cycles(core_path, cldr));
    TRY(parse_week_data(core_path, cldr));
    TRY(parse_meta_zones(core_path, cldr));

    auto remove_variants_from_path = [&](ByteString path) -> ErrorOr<ByteString> {
        auto parsed_locale = TRY(CanonicalLanguageID::parse(cldr.unique_strings, LexicalPath::basename(path)));

        StringBuilder builder;
        builder.append(cldr.unique_strings.get(parsed_locale.language));
        if (auto script = cldr.unique_strings.get(parsed_locale.script); !script.is_empty())
            builder.appendff("-{}", script);
        if (auto region = cldr.unique_strings.get(parsed_locale.region); !region.is_empty())
            builder.appendff("-{}", region);

        return builder.to_byte_string();
    };

    TRY(Core::Directory::for_each_entry(TRY(String::formatted("{}/main", dates_path)), Core::DirIterator::SkipParentAndBaseDir, [&](auto& entry, auto& directory) -> ErrorOr<IterationDecision> {
        auto dates_path = LexicalPath::join(directory.path().string(), entry.name).string();

        auto language = TRY(remove_variants_from_path(dates_path));
        auto& locale = cldr.locales.ensure(language);

        TRY(parse_time_zone_names(move(dates_path), cldr, locale));
        return IterationDecision::Continue;
    }));

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
    generator.set("string_index_type"sv, cldr.unique_strings.type_that_fits());
    generator.set("time_zone_index_type"sv, cldr.unique_time_zones.type_that_fits());
    generator.set("time_zone_list_index_type"sv, cldr.unique_time_zone_lists.type_that_fits());

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
#include <LibTimeZone/TimeZone.h>

namespace Locale {
)~~~");

    cldr.unique_strings.generate(generator);

    generator.append(R"~~~(
struct TimeZoneNames {
    @string_index_type@ short_standard_name { 0 };
    @string_index_type@ long_standard_name { 0 };

    @string_index_type@ short_daylight_name { 0 };
    @string_index_type@ long_daylight_name { 0 };

    @string_index_type@ short_generic_name { 0 };
    @string_index_type@ long_generic_name { 0 };
};

struct TimeZoneFormatImpl {
    TimeZoneFormat to_time_zone_format() const {
        TimeZoneFormat time_zone_format {};

        time_zone_format.symbol_ahead_sign = decode_string(symbol_ahead_sign);
        time_zone_format.symbol_ahead_separator = decode_string(symbol_ahead_separator);
        time_zone_format.symbol_behind_sign = decode_string(symbol_behind_sign);
        time_zone_format.symbol_behind_separator = decode_string(symbol_behind_separator);
        time_zone_format.gmt_format = decode_string(gmt_format);
        time_zone_format.gmt_zero_format = decode_string(gmt_zero_format);

        return time_zone_format;
    }

    @string_index_type@ symbol_ahead_sign { 0 };
    @string_index_type@ symbol_ahead_separator { 0 };

    @string_index_type@ symbol_behind_sign { 0 };
    @string_index_type@ symbol_behind_separator { 0 };

    @string_index_type@ gmt_format { 0 };
    @string_index_type@ gmt_zero_format { 0 };
};
)~~~");

    cldr.unique_time_zones.generate(generator, "TimeZoneNames"sv, "s_time_zones"sv, 30);
    cldr.unique_time_zone_lists.generate(generator, cldr.unique_time_zones.type_that_fits(), "s_time_zone_lists"sv);
    cldr.unique_time_zone_formats.generate(generator, "TimeZoneFormatImpl"sv, "s_time_zone_formats"sv, 30);
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

    auto locales = cldr.locales.keys();
    quick_sort(locales);

    append_mapping(locales, cldr.locales, cldr.unique_time_zones.type_that_fits(), "s_locale_time_zones"sv, [](auto const& locale) { return locale.time_zones; });
    append_mapping(locales, cldr.locales, cldr.unique_time_zone_formats.type_that_fits(), "s_locale_time_zone_formats"sv, [](auto const& locale) { return locale.time_zone_formats; });
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
Optional<TimeZoneFormat> get_time_zone_format(StringView locale)
{
    auto locale_value = locale_from_string(locale);
    if (!locale_value.has_value())
        return {};

    auto locale_index = to_underlying(*locale_value) - 1; // Subtract 1 because 0 == Locale::None.
    auto time_zone_format_index = s_locale_time_zone_formats.at(locale_index);

    auto const& time_zone_format = s_time_zone_formats.at(time_zone_format_index);
    return time_zone_format.to_time_zone_format();
}

static TimeZoneNames const* find_time_zone_names(StringView locale, StringView time_zone)
{
    auto locale_value = locale_from_string(locale);
    if (!locale_value.has_value())
        return nullptr;

    auto time_zone_value = ::TimeZone::time_zone_from_string(time_zone);
    if (!time_zone_value.has_value())
        return nullptr;

    auto locale_index = to_underlying(*locale_value) - 1; // Subtract 1 because 0 == Locale::None.
    size_t time_zone_index = to_underlying(*time_zone_value);

    auto time_zone_list_index = s_locale_time_zones.at(locale_index);
    auto const& time_zone_list = s_time_zone_lists.at(time_zone_list_index);
    if (time_zone_list.size() <= time_zone_index)
        return nullptr;

    time_zone_index = time_zone_list.at(time_zone_index);
    return &s_time_zones[time_zone_index];
}

Optional<StringView> get_time_zone_name(StringView locale, StringView time_zone, CalendarPatternStyle style, TimeZone::InDST in_dst)
{
    if (auto const* data = find_time_zone_names(locale, time_zone); data != nullptr) {
        size_t name_index = 0;

        switch (style) {
        case CalendarPatternStyle::Short:
            name_index = (in_dst == TimeZone::InDST::No) ? data->short_standard_name : data->short_daylight_name;
            break;
        case CalendarPatternStyle::Long:
            name_index = (in_dst == TimeZone::InDST::No) ? data->long_standard_name : data->long_daylight_name;
            break;
        case CalendarPatternStyle::ShortGeneric:
            name_index = data->short_generic_name;
            break;
        case CalendarPatternStyle::LongGeneric:
            name_index = data->long_generic_name;
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        if (name_index != 0)
            return decode_string(name_index);
    }

    return {};
}

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
    StringView dates_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the Unicode locale header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Unicode locale implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(core_path, "Path to cldr-core directory", "core-path", 'r', "core-path");
    args_parser.add_option(dates_path, "Path to cldr-dates directory", "dates-path", 'd', "dates-path");
    args_parser.parse(arguments);

    auto generated_header_file = TRY(open_file(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(open_file(generated_implementation_path, Core::File::OpenMode::Write));

    CLDR cldr;
    TRY(parse_all_locales(core_path, dates_path, cldr));

    TRY(generate_unicode_locale_header(*generated_header_file, cldr));
    TRY(generate_unicode_locale_implementation(*generated_implementation_file, cldr));

    return 0;
}
