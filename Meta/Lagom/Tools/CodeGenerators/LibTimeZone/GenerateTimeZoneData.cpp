/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../LibUnicode/GeneratorUtil.h" // FIXME: Move this somewhere common.
#include <AK/ByteString.h>
#include <AK/DateConstants.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibTimeZone/TimeZone.h>

namespace {

struct DateTime {
    u16 year { 0 };
    Optional<u8> month;
    Optional<u8> day;

    Optional<u8> last_weekday;
    Optional<u8> after_weekday;
    Optional<u8> before_weekday;

    Optional<u8> hour;
    Optional<u8> minute;
    Optional<u8> second;
};

struct TimeZoneOffset {
    i64 offset { 0 };
    Optional<DateTime> until;

    Optional<ByteString> dst_rule;
    Optional<i32> dst_rule_index;
    i64 dst_offset { 0 };

    size_t standard_format { 0 };
    size_t daylight_format { 0 };
};

struct DaylightSavingsOffset {
    i64 offset { 0 };
    u16 year_from { 0 };
    Optional<u16> year_to;
    DateTime in_effect;

    size_t format { 0 };
};

struct TimeZoneData {
    UniqueStringStorage unique_strings;

    HashMap<ByteString, Vector<TimeZoneOffset>> time_zones;
    Vector<ByteString> time_zone_names;
    Vector<Alias> time_zone_aliases;

    HashMap<ByteString, Vector<DaylightSavingsOffset>> dst_offsets;
    Vector<ByteString> dst_offset_names;

    HashMap<ByteString, Vector<size_t>> time_zone_regions;
    Vector<ByteString> time_zone_region_names;

    Vector<TimeZone::TimeZoneIdentifier> time_zones_and_links;
};

}

template<>
struct AK::Formatter<DateTime> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, DateTime const& date_time)
    {
        return Formatter<FormatString>::format(builder,
            "{{ {}, {}, {}, {}, {}, {}, {}, {}, {} }}"sv,
            date_time.year,
            date_time.month.value_or(1),
            date_time.day.value_or(1),
            date_time.last_weekday.value_or(0),
            date_time.after_weekday.value_or(0),
            date_time.before_weekday.value_or(0),
            date_time.hour.value_or(0),
            date_time.minute.value_or(0),
            date_time.second.value_or(0));
    }
};

template<>
struct AK::Formatter<TimeZoneOffset> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, TimeZoneOffset const& time_zone_offset)
    {
        return Formatter<FormatString>::format(builder,
            "{{ {}, {}, {}, {}, {}, {}, {} }}"sv,
            time_zone_offset.offset,
            time_zone_offset.until.value_or({}),
            time_zone_offset.until.has_value(),
            time_zone_offset.dst_rule_index.value_or(-1),
            time_zone_offset.dst_offset,
            time_zone_offset.standard_format,
            time_zone_offset.daylight_format);
    }
};

template<>
struct AK::Formatter<DaylightSavingsOffset> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, DaylightSavingsOffset const& dst_offset)
    {
        auto format_time = [&](auto year) {
            return ByteString::formatted("AK::UnixDateTime::from_unix_time_parts({}, 1, 1, 0, 0, 0, 0)", year);
        };

        static ByteString max_year_as_time("max_year_as_time"sv);

        return Formatter<FormatString>::format(builder,
            "{{ {}, {}, {}, {}, {} }}"sv,
            dst_offset.offset,
            format_time(dst_offset.year_from),
            dst_offset.year_to.has_value()
                ? format_time(*dst_offset.year_to + 1)
                : max_year_as_time,
            dst_offset.in_effect,
            dst_offset.format);
    }
};

template<>
struct AK::Formatter<TimeZone::TimeZoneIdentifier> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, TimeZone::TimeZoneIdentifier const& time_zone)
    {
        return Formatter<FormatString>::format(builder,
            "{{ \"{}\"sv, IsLink::{} }}"sv,
            time_zone.name,
            time_zone.is_link == TimeZone::IsLink::Yes ? "Yes"sv : "No"sv);
    }
};

static Optional<DateTime> parse_date_time(ReadonlySpan<StringView> segments)
{
    auto comment_index = find_index(segments.begin(), segments.end(), "#"sv);
    if (comment_index != segments.size())
        segments = segments.slice(0, comment_index);
    if (segments.is_empty())
        return {};

    DateTime date_time {};
    date_time.year = segments[0].to_number<unsigned>().value();

    if (segments.size() > 1)
        date_time.month = find_index(short_month_names.begin(), short_month_names.end(), segments[1]) + 1;

    if (segments.size() > 2) {
        if (segments[2].starts_with("last"sv)) {
            auto weekday = segments[2].substring_view("last"sv.length());
            date_time.last_weekday = find_index(short_day_names.begin(), short_day_names.end(), weekday);
        } else if (auto index = segments[2].find(">="sv); index.has_value()) {
            auto weekday = segments[2].substring_view(0, *index);
            date_time.after_weekday = find_index(short_day_names.begin(), short_day_names.end(), weekday);

            auto day = segments[2].substring_view(*index + ">="sv.length());
            date_time.day = day.to_number<unsigned>().value();
        } else if (auto index = segments[2].find("<="sv); index.has_value()) {
            auto weekday = segments[2].substring_view(0, *index);
            date_time.before_weekday = find_index(short_day_names.begin(), short_day_names.end(), weekday);

            auto day = segments[2].substring_view(*index + "<="sv.length());
            date_time.day = day.to_number<unsigned>().value();
        } else {
            date_time.day = segments[2].to_number<unsigned>().value();
        }
    }

    if (segments.size() > 3) {
        // FIXME: Some times end with a letter, e.g. "2:00u" and "2:00s". Figure out what this means and handle it.
        auto time_segments = segments[3].split_view(':');

        date_time.hour = time_segments[0].to_number<int>().value();
        date_time.minute = time_segments.size() > 1 ? time_segments[1].substring_view(0, 2).to_number<unsigned>().value() : 0;
        date_time.second = time_segments.size() > 2 ? time_segments[2].substring_view(0, 2).to_number<unsigned>().value() : 0;
    }

    return date_time;
}

static i64 parse_time_offset(StringView segment)
{
    auto segments = segment.split_view(':');

    i64 hours = segments[0].to_number<int>().value();
    i64 minutes = segments.size() > 1 ? segments[1].to_number<unsigned>().value() : 0;
    i64 seconds = segments.size() > 2 ? segments[2].to_number<unsigned>().value() : 0;

    i64 sign = ((hours < 0) || (segments[0] == "-0"sv)) ? -1 : 1;
    return (hours * 3600) + sign * ((minutes * 60) + seconds);
}

static void parse_dst_rule(StringView segment, TimeZoneOffset& time_zone)
{
    if (segment.contains(':'))
        time_zone.dst_offset = parse_time_offset(segment);
    else if (segment != "-"sv)
        time_zone.dst_rule = segment;
}

static void parse_format(StringView format, TimeZoneData& time_zone_data, TimeZoneOffset& time_zone)
{
    auto formats = format.replace("%s"sv, "{}"sv, ReplaceMode::FirstOnly).split('/');
    VERIFY(formats.size() <= 2);

    time_zone.standard_format = time_zone_data.unique_strings.ensure(formats[0]);

    if (formats.size() == 2)
        time_zone.daylight_format = time_zone_data.unique_strings.ensure(formats[1]);
    else
        time_zone.daylight_format = time_zone.standard_format;
}

static Vector<TimeZoneOffset>& parse_zone(StringView zone_line, TimeZoneData& time_zone_data)
{
    auto segments = zone_line.split_view_if([](char ch) { return (ch == '\t') || (ch == ' '); });

    // "Zone" NAME STDOFF RULES FORMAT [UNTIL]
    VERIFY(segments[0] == "Zone"sv);
    auto name = segments[1];

    TimeZoneOffset time_zone {};
    time_zone.offset = parse_time_offset(segments[2]);
    parse_dst_rule(segments[3], time_zone);
    parse_format(segments[4], time_zone_data, time_zone);

    if (segments.size() > 5)
        time_zone.until = parse_date_time(segments.span().slice(5));

    auto& time_zones = time_zone_data.time_zones.ensure(name);
    time_zones.append(move(time_zone));

    if (!time_zone_data.time_zone_names.contains_slow(name)) {
        time_zone_data.time_zone_names.append(name);
        time_zone_data.time_zones_and_links.append({ time_zone_data.time_zone_names.last(), TimeZone::IsLink::No });
    }

    return time_zones;
}

static void parse_zone_continuation(StringView zone_line, TimeZoneData& time_zone_data, Vector<TimeZoneOffset>& time_zones)
{
    auto segments = zone_line.split_view_if([](char ch) { return (ch == '\t') || (ch == ' '); });

    // STDOFF RULES FORMAT [UNTIL]
    TimeZoneOffset time_zone {};
    time_zone.offset = parse_time_offset(segments[0]);
    parse_dst_rule(segments[1], time_zone);
    parse_format(segments[2], time_zone_data, time_zone);

    if (segments.size() > 3)
        time_zone.until = parse_date_time(segments.span().slice(3));

    time_zones.append(move(time_zone));
}

static void parse_link(StringView link_line, TimeZoneData& time_zone_data)
{
    auto segments = link_line.split_view_if([](char ch) { return (ch == '\t') || (ch == ' '); });

    // Link TARGET LINK-NAME
    VERIFY(segments[0] == "Link"sv);
    auto target = segments[1];
    auto alias = segments[2];

    time_zone_data.time_zone_aliases.append({ target, alias });
    time_zone_data.time_zones_and_links.append({ time_zone_data.time_zone_aliases.last().alias, TimeZone::IsLink::Yes });
}

static void parse_rule(StringView rule_line, TimeZoneData& time_zone_data)
{
    auto segments = rule_line.split_view_if([](char ch) { return (ch == '\t') || (ch == ' '); });

    // Rule NAME FROM TO TYPE IN ON AT SAVE LETTER/S
    VERIFY(segments[0] == "Rule"sv);
    auto name = segments[1];

    DaylightSavingsOffset dst_offset {};
    dst_offset.offset = parse_time_offset(segments[8]);
    dst_offset.year_from = segments[2].to_number<unsigned>().value();

    if (segments[3] == "only")
        dst_offset.year_to = dst_offset.year_from;
    else if (segments[3] != "max"sv)
        dst_offset.year_to = segments[3].to_number<unsigned>().value();

    auto in_effect = Array { "0"sv, segments[5], segments[6], segments[7] };
    dst_offset.in_effect = parse_date_time(in_effect).release_value();

    if (segments[9] != "-"sv)
        dst_offset.format = time_zone_data.unique_strings.ensure(segments[9]);

    auto& dst_offsets = time_zone_data.dst_offsets.ensure(name);
    dst_offsets.append(move(dst_offset));

    if (!time_zone_data.dst_offset_names.contains_slow(name))
        time_zone_data.dst_offset_names.append(name);
}

static ErrorOr<void> parse_time_zones(StringView time_zone_path, TimeZoneData& time_zone_data)
{
    // For reference, the man page for `zic` has the best documentation of the TZDB file format.
    auto file = TRY(open_file(time_zone_path, Core::File::OpenMode::Read));
    Array<u8, 1024> buffer {};

    Vector<TimeZoneOffset>* last_parsed_zone = nullptr;

    while (TRY(file->can_read_line())) {
        auto line = TRY(file->read_line(buffer));

        if (line.is_empty() || line.trim_whitespace(TrimMode::Left).starts_with('#'))
            continue;

        if (line.starts_with("Zone"sv)) {
            last_parsed_zone = &parse_zone(line, time_zone_data);
        } else if (line.starts_with('\t')) {
            VERIFY(last_parsed_zone != nullptr);
            parse_zone_continuation(line, time_zone_data, *last_parsed_zone);
        } else {
            last_parsed_zone = nullptr;

            if (line.starts_with("Link"sv))
                parse_link(line, time_zone_data);
            else if (line.starts_with("Rule"sv))
                parse_rule(line, time_zone_data);
        }
    }

    return {};
}

static ErrorOr<void> parse_time_zone_coordinates(Core::InputBufferedFile& file, TimeZoneData& time_zone_data)
{
    Array<u8, 1024> buffer {};

    while (TRY(file.can_read_line())) {
        auto line = TRY(file.read_line(buffer));

        if (line.is_empty() || line.trim_whitespace(TrimMode::Left).starts_with('#'))
            continue;

        auto segments = line.split_view('\t');
        auto regions = segments[0];
        auto zone = segments[2];

        VERIFY(time_zone_data.time_zones.contains(zone));

        TRY(regions.for_each_split_view(',', SplitBehavior::Nothing, [&](auto region) -> ErrorOr<void> {
            auto index = time_zone_data.unique_strings.ensure(zone);
            TRY(time_zone_data.time_zone_regions.ensure(region).try_append(index));

            if (!time_zone_data.time_zone_region_names.contains_slow(region))
                TRY(time_zone_data.time_zone_region_names.try_append(region));
            return {};
        }));
    }

    return {};
}

static void set_dst_rule_indices(TimeZoneData& time_zone_data)
{
    for (auto& time_zone : time_zone_data.time_zones) {
        for (auto& time_zone_offset : time_zone.value) {
            if (!time_zone_offset.dst_rule.has_value())
                continue;

            auto dst_rule_index = time_zone_data.dst_offset_names.find_first_index(*time_zone_offset.dst_rule);
            time_zone_offset.dst_rule_index = static_cast<i32>(dst_rule_index.value());
        }
    }
}

static ByteString format_identifier(StringView owner, ByteString identifier)
{
    constexpr auto gmt_time_zones = Array { "Etc/GMT"sv, "GMT"sv };

    for (auto gmt_time_zone : gmt_time_zones) {
        if (identifier.starts_with(gmt_time_zone)) {
            auto offset = identifier.substring_view(gmt_time_zone.length());

            if (offset.starts_with('+'))
                identifier = ByteString::formatted("{}_Ahead_{}", gmt_time_zone, offset.substring_view(1));
            else if (offset.starts_with('-'))
                identifier = ByteString::formatted("{}_Behind_{}", gmt_time_zone, offset.substring_view(1));
        }
    }

    identifier = identifier.replace("-"sv, "_"sv, ReplaceMode::All);
    identifier = identifier.replace("/"sv, "_"sv, ReplaceMode::All);

    if (all_of(identifier, is_ascii_digit))
        return ByteString::formatted("{}_{}", owner[0], identifier);
    if (is_ascii_lower_alpha(identifier[0]))
        return ByteString::formatted("{:c}{}", to_ascii_uppercase(identifier[0]), identifier.substring_view(1));
    return identifier;
}

static ErrorOr<void> generate_time_zone_data_header(Core::InputBufferedFile& file, TimeZoneData& time_zone_data)
{
    StringBuilder builder;
    SourceGenerator generator { builder };

    generator.append(R"~~~(
#pragma once

#include <AK/Types.h>

namespace TimeZone {
)~~~");

    generate_enum(generator, format_identifier, "TimeZone"sv, {}, time_zone_data.time_zone_names, time_zone_data.time_zone_aliases);
    generate_enum(generator, format_identifier, "DaylightSavingsRule"sv, {}, time_zone_data.dst_offset_names);
    generate_enum(generator, format_identifier, "Region"sv, {}, time_zone_data.time_zone_region_names);

    generator.append(R"~~~(
}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

static ErrorOr<void> generate_time_zone_data_implementation(Core::InputBufferedFile& file, TimeZoneData& time_zone_data)
{
    StringBuilder builder;
    SourceGenerator generator { builder };
    generator.set("string_index_type"sv, time_zone_data.unique_strings.type_that_fits());

    set_dst_rule_indices(time_zone_data);

    generator.append(R"~~~(
#include <AK/Array.h>
#include <AK/BinarySearch.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/NumericLimits.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <LibTimeZone/TimeZone.h>
#include <LibTimeZone/TimeZoneData.h>

namespace TimeZone {

static constexpr auto max_year_as_time = AK::UnixDateTime::from_unix_time_parts(NumericLimits<u16>::max(), 1, 1, 0, 0, 0, 0);

struct DateTime {
    AK::UnixDateTime time_since_epoch() const
    {
        // FIXME: This implementation does not take last_weekday, after_weekday, or before_weekday into account.
        return AK::UnixDateTime::from_unix_time_parts(year, month, day, hour, minute, second, 0);
    }

    u16 year { 0 };
    u8 month { 1 };
    u8 day { 1 };

    u8 last_weekday { 0 };
    u8 after_weekday { 0 };
    u8 before_weekday { 0 };

    u8 hour { 0 };
    u8 minute { 0 };
    u8 second { 0 };
};

struct TimeZoneOffset {
    i64 offset { 0 };

    DateTime until {};
    bool has_until { false };

    i32 dst_rule { -1 };
    i64 dst_offset { 0 };

    @string_index_type@ standard_format { 0 };
    @string_index_type@ daylight_format { 0 };
};

struct DaylightSavingsOffset {
    AK::UnixDateTime time_in_effect(AK::UnixDateTime time) const
    {
        auto in_effect = this->in_effect;
        in_effect.year = seconds_since_epoch_to_year(time.seconds_since_epoch());

        return in_effect.time_since_epoch();
    }

    i64 offset { 0 };
    AK::UnixDateTime year_from {};
    AK::UnixDateTime year_to {};
    DateTime in_effect {};

    @string_index_type@ format { 0 };
};
)~~~");

    time_zone_data.unique_strings.generate(generator);

    auto append_offsets = [&](auto const& name, auto type, auto const& offsets) {
        generator.set("name", name);
        generator.set("type", type);
        generator.set("size", ByteString::number(offsets.size()));

        generator.append(R"~~~(
static constexpr Array<@type@, @size@> @name@ { {
)~~~");

        for (auto const& offset : offsets)
            generator.append(ByteString::formatted("    {},\n", offset));

        generator.append("} };\n");
    };

    generate_mapping(generator, time_zone_data.time_zone_names, "TimeZoneOffset"sv, "s_time_zone_offsets"sv, "s_time_zone_offsets_{}"sv, format_identifier,
        [&](auto const& name, auto const& value) {
            auto const& time_zone_offsets = time_zone_data.time_zones.find(value)->value;
            append_offsets(name, "TimeZoneOffset"sv, time_zone_offsets);
        });

    generate_mapping(generator, time_zone_data.dst_offset_names, "DaylightSavingsOffset"sv, "s_dst_offsets"sv, "s_dst_offsets_{}"sv, format_identifier,
        [&](auto const& name, auto const& value) {
            auto const& dst_offsets = time_zone_data.dst_offsets.find(value)->value;
            append_offsets(name, "DaylightSavingsOffset"sv, dst_offsets);
        });

    generate_mapping(generator, time_zone_data.time_zone_region_names, time_zone_data.unique_strings.type_that_fits(), "s_regional_time_zones"sv, "s_regional_time_zones_{}"sv, format_identifier,
        [&](auto const& name, auto const& value) {
            auto const& time_zones = time_zone_data.time_zone_regions.find(value)->value;

            generator.set("name", name);
            generator.set("size", ByteString::number(time_zones.size()));

            generator.append(R"~~~(
static constexpr Array<@string_index_type@, @size@> @name@ { {)~~~");

            bool first = true;
            for (auto const& time_zone : time_zones) {
                generator.append(first ? " "sv : ", "sv);
                generator.append(ByteString::number(time_zone));
                first = false;
            }

            generator.append(" } };");
        });

    auto append_string_conversions = [&](StringView enum_title, StringView enum_snake, auto const& values, Vector<Alias> const& aliases = {}) -> ErrorOr<void> {
        HashValueMap<ByteString> hashes;
        TRY(hashes.try_ensure_capacity(values.size()));

        auto hash = [](auto const& value) {
            return CaseInsensitiveASCIIStringViewTraits::hash(value);
        };

        for (auto const& value : values)
            hashes.set(hash(value), format_identifier(enum_title, value));
        for (auto const& alias : aliases)
            hashes.set(hash(alias.alias), format_identifier(enum_title, alias.alias));

        ValueFromStringOptions options {};
        options.sensitivity = CaseSensitivity::CaseInsensitive;

        generate_value_from_string(generator, "{}_from_string"sv, enum_title, enum_snake, move(hashes), options);
        generate_value_to_string(generator, "{}_to_string"sv, enum_title, enum_snake, format_identifier, values);

        return {};
    };

    TRY(append_string_conversions("TimeZone"sv, "time_zone"sv, time_zone_data.time_zone_names, time_zone_data.time_zone_aliases));
    TRY(append_string_conversions("Region"sv, "region"sv, time_zone_data.time_zone_region_names));

    generator.append(R"~~~(
static Array<DaylightSavingsOffset const*, 2> find_dst_offsets(TimeZoneOffset const& time_zone_offset, AK::UnixDateTime time)
{
    auto const& dst_rules = s_dst_offsets[time_zone_offset.dst_rule];

    DaylightSavingsOffset const* standard_offset = nullptr;
    DaylightSavingsOffset const* daylight_offset = nullptr;
    DaylightSavingsOffset const* last_offset = nullptr;

    auto preferred_rule = [&](auto* current_offset, auto& new_offset) {
        if (!current_offset)
            return &new_offset;

        auto new_time_in_effect = new_offset.time_in_effect(time);
        return (time >= new_time_in_effect) ? &new_offset : current_offset;
    };

    for (size_t index = 0; (index < dst_rules.size()) && (!standard_offset || !daylight_offset); ++index) {
        auto const& dst_rule = dst_rules[index];

        if (last_offset == nullptr)
            last_offset = &dst_rule;
        else if (dst_rule.time_in_effect(dst_rule.year_to) > last_offset->time_in_effect(last_offset->year_to))
            last_offset = &dst_rule;

        if ((time < dst_rule.year_from) || (time >= dst_rule.year_to))
            continue;

        if (dst_rule.offset == 0)
            standard_offset = preferred_rule(standard_offset, dst_rule);
        else
            daylight_offset = preferred_rule(daylight_offset, dst_rule);
    }

    // If there isn't a standard or daylight rule in effect, fall back to the last rule given in the TZDB.
    if (!standard_offset) {
        VERIFY(last_offset != nullptr);
        standard_offset = last_offset;
    }

    return { standard_offset, daylight_offset ? daylight_offset : standard_offset };
}

static Offset get_active_dst_offset(TimeZoneOffset const& time_zone_offset, AK::UnixDateTime time)
{
    auto offsets = find_dst_offsets(time_zone_offset, time);
    if (offsets[0] == offsets[1])
        return { offsets[0]->offset, InDST::No };

    auto standard_time_in_effect = offsets[0]->time_in_effect(time);
    auto daylight_time_in_effect = offsets[1]->time_in_effect(time);

    if (daylight_time_in_effect < standard_time_in_effect) {
        if ((time < daylight_time_in_effect) || (time >= standard_time_in_effect))
            return { offsets[0]->offset, InDST::No };
    } else {
        if ((time >= standard_time_in_effect) && (time < daylight_time_in_effect))
            return { offsets[0]->offset, InDST::No };
    }

    return { offsets[1]->offset, InDST::Yes };
}

static TimeZoneOffset const& find_time_zone_offset(TimeZone time_zone, AK::UnixDateTime time)
{
    auto const& time_zone_offsets = s_time_zone_offsets[to_underlying(time_zone)];

    size_t index = 0;
    for (; index < time_zone_offsets.size(); ++index) {
        auto const& time_zone_offset = time_zone_offsets[index];

        if (!time_zone_offset.has_until || (time_zone_offset.until.time_since_epoch() > time))
            break;
    }

    VERIFY(index < time_zone_offsets.size());
    return time_zone_offsets[index];
}

Optional<Offset> get_time_zone_offset(TimeZone time_zone, AK::UnixDateTime time)
{
    auto const& time_zone_offset = find_time_zone_offset(time_zone, time);

    Offset dst_offset {};
    if (time_zone_offset.dst_rule != -1) {
        dst_offset = get_active_dst_offset(time_zone_offset, time);
    } else {
        auto in_dst = time_zone_offset.dst_offset == 0 ? InDST::No : InDST::Yes;
        dst_offset = { time_zone_offset.dst_offset, in_dst };
    }

    dst_offset.seconds += time_zone_offset.offset;
    return dst_offset;
}

Vector<StringView> time_zones_in_region(StringView region)
{
    auto region_value = region_from_string(region);
    if (!region_value.has_value())
        return {};

    auto region_index = to_underlying(*region_value);

    auto const& regional_time_zones = s_regional_time_zones[region_index];

    Vector<StringView> time_zones;
    time_zones.ensure_capacity(regional_time_zones.size());

    for (auto time_zone : regional_time_zones)
        time_zones.unchecked_append(decode_string(time_zone));

    return time_zones;
}
)~~~");

    quick_sort(time_zone_data.time_zones_and_links, [](auto const& lhs, auto const& rhs) {
        return lhs.name < rhs.name;
    });

    generator.set("time_zones_and_links_size", MUST(String::number(time_zone_data.time_zones_and_links.size())));

    generator.append(R"~~~(
ReadonlySpan<TimeZoneIdentifier> all_time_zones()
{
    static constexpr Array<TimeZoneIdentifier, @time_zones_and_links_size@> time_zones_and_links { {)~~~");

    bool first = true;
    for (auto const& zone : time_zone_data.time_zones_and_links) {
        generator.append(first ? " "sv : ", "sv);
        generator.append(MUST(String::formatted("{}", zone)));
        first = false;
    }
    generator.append(R"~~~( } };

    return time_zones_and_links.span();
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
    StringView time_zone_coordinates_path;
    Vector<StringView> time_zone_paths;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the time zone data header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the time zone data implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(time_zone_coordinates_path, "Path to the time zone data coordinates file", "time-zone-coordinates-path", 'z', "time-zone-coordinates-path");
    args_parser.add_positional_argument(time_zone_paths, "Paths to the time zone database files", "time-zone-paths");
    args_parser.parse(arguments);

    auto generated_header_file = TRY(open_file(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(open_file(generated_implementation_path, Core::File::OpenMode::Write));
    auto time_zone_coordinates_file = TRY(open_file(time_zone_coordinates_path, Core::File::OpenMode::Read));

    TimeZoneData time_zone_data {};
    for (auto time_zone_path : time_zone_paths)
        TRY(parse_time_zones(time_zone_path, time_zone_data));

    TRY(parse_time_zone_coordinates(*time_zone_coordinates_file, time_zone_data));

    TRY(generate_time_zone_data_header(*generated_header_file, time_zone_data));
    TRY(generate_time_zone_data_implementation(*generated_implementation_file, time_zone_data));

    return 0;
}
