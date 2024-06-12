/*
 * Copyright (c) 2021-2022, Tim Flynn <trflynn89@serenityos.org>
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
#include <AK/QuickSort.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibFileSystem/FileSystem.h>

static ByteString format_identifier(StringView owner, ByteString identifier)
{
    identifier = identifier.replace("-"sv, "_"sv, ReplaceMode::All);

    if (all_of(identifier, is_ascii_digit))
        return ByteString::formatted("{}_{}", owner[0], identifier);
    if (is_ascii_lower_alpha(identifier[0]))
        return ByteString::formatted("{:c}{}", to_ascii_uppercase(identifier[0]), identifier.substring_view(1));
    return identifier;
}

using KeywordList = Vector<size_t>;

struct LocaleData {
    size_t calendar_keywords { 0 };
    size_t collation_case_keywords { 0 };
    size_t collation_numeric_keywords { 0 };
    size_t number_system_keywords { 0 };
    size_t text_layout { 0 };
};

struct CLDR {
    UniqueStringStorage unique_strings;
    UniqueStorage<KeywordList> unique_keyword_lists;

    HashMap<ByteString, LocaleData> locales;
    Vector<Alias> locale_aliases;

    HashMap<ByteString, Vector<ByteString>> keywords;
    HashMap<ByteString, Vector<Alias>> keyword_aliases;
    HashMap<ByteString, ByteString> keyword_names;
};

// Some parsing is expected to fail. For example, the CLDR contains language mappings
// with locales such as "en-GB-oed" that are canonically invalid locale IDs.
#define TRY_OR_DISCARD(expression)                                                                   \
    ({                                                                                               \
        auto&& _temporary_result = (expression);                                                     \
        if (_temporary_result.is_error())                                                            \
            return;                                                                                  \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        _temporary_result.release_value();                                                           \
    })

// NOTE: We return a pointer only because ErrorOr cannot store references. You may safely assume the pointer is non-null.
ErrorOr<JsonValue const*> read_json_file_with_cache(ByteString const& path)
{
    static HashMap<ByteString, JsonValue> parsed_json_cache;

    if (auto parsed_json = parsed_json_cache.get(path); parsed_json.has_value())
        return &parsed_json.value();

    auto parsed_json = TRY(read_json_file(path));
    TRY(parsed_json_cache.try_set(path, move(parsed_json)));

    return &parsed_json_cache.get(path).value();
}

static ErrorOr<void> parse_unicode_extension_keywords(ByteString bcp47_path, CLDR& cldr)
{
    constexpr auto desired_keywords = Array { "ca"sv, "co"sv, "hc"sv, "kf"sv, "kn"sv, "nu"sv };
    auto keywords = TRY(read_json_file(bcp47_path));

    auto const& keyword_object = keywords.as_object().get_object("keyword"sv).value();
    auto unicode_object = keyword_object.get_object("u"sv);
    if (!unicode_object.has_value())
        return {};

    unicode_object->for_each_member([&](auto const& key, auto const& value) {
        if (!desired_keywords.span().contains_slow(key))
            return;

        auto const& name = value.as_object().get_byte_string("_alias"sv).value();
        cldr.keyword_names.set(key, name);

        auto& keywords = cldr.keywords.ensure(key);

        // FIXME: ECMA-402 requires the list of supported collation types to include "default", but
        //        that type does not appear in collation.json.
        if (key == "co" && !keywords.contains_slow("default"sv))
            keywords.append("default"sv);

        value.as_object().for_each_member([&](auto const& keyword, auto const& properties) {
            if (!properties.is_object())
                return;

            // Filter out values not permitted by ECMA-402.
            // https://tc39.es/ecma402/#sec-intl-collator-internal-slots
            if (key == "co"sv && keyword.is_one_of("search"sv, "standard"sv))
                return;
            // https://tc39.es/ecma402/#sec-intl.numberformat-internal-slots
            if (key == "nu"sv && keyword.is_one_of("finance"sv, "native"sv, "traditio"sv))
                return;

            if (auto const& preferred = properties.as_object().get_byte_string("_preferred"sv); preferred.has_value()) {
                cldr.keyword_aliases.ensure(key).append({ preferred.value(), keyword });
                return;
            }

            if (auto const& alias = properties.as_object().get_byte_string("_alias"sv); alias.has_value())
                cldr.keyword_aliases.ensure(key).append({ keyword, alias.value() });

            keywords.append(keyword);
        });
    });

    return {};
}

static Optional<ByteString> find_keyword_alias(StringView key, StringView calendar, CLDR& cldr)
{
    auto it = cldr.keyword_aliases.find(key);
    if (it == cldr.keyword_aliases.end())
        return {};

    auto alias = it->value.find_if([&](auto const& alias) { return calendar == alias.alias; });
    if (alias == it->value.end())
        return {};

    return alias->name;
}

static ErrorOr<void> parse_number_system_keywords(ByteString locale_numbers_path, CLDR& cldr, LocaleData& locale)
{
    LexicalPath numbers_path(move(locale_numbers_path));
    numbers_path = numbers_path.append("numbers.json"sv);

    auto numbers = TRY(read_json_file(numbers_path.string()));
    auto const& main_object = numbers.as_object().get_object("main"sv).value();
    auto const& locale_object = main_object.get_object(numbers_path.parent().basename()).value();
    auto const& locale_numbers_object = locale_object.get_object("numbers"sv).value();
    auto const& default_numbering_system_object = locale_numbers_object.get_byte_string("defaultNumberingSystem"sv).value();
    auto const& other_numbering_systems_object = locale_numbers_object.get_object("otherNumberingSystems"sv).value();

    KeywordList keywords {};

    auto append_numbering_system = [&](ByteString system_name) {
        if (auto system_alias = find_keyword_alias("nu"sv, system_name, cldr); system_alias.has_value())
            system_name = system_alias.release_value();

        auto index = cldr.unique_strings.ensure(move(system_name));
        if (!keywords.contains_slow(index))
            keywords.append(move(index));
    };

    append_numbering_system(default_numbering_system_object);

    other_numbering_systems_object.for_each_member([&](auto const&, JsonValue const& value) {
        append_numbering_system(value.as_string());
    });

    locale_numbers_object.for_each_member([&](auto const& key, JsonValue const& value) {
        if (!key.starts_with("defaultNumberingSystem-alt-"sv))
            return;
        append_numbering_system(value.as_string());
    });

    locale.number_system_keywords = cldr.unique_keyword_lists.ensure(move(keywords));
    return {};
}

static ErrorOr<void> parse_calendar_keywords(ByteString locale_dates_path, CLDR& cldr, LocaleData& locale)
{
    KeywordList keywords {};

    TRY(Core::Directory::for_each_entry(locale_dates_path, Core::DirIterator::SkipParentAndBaseDir, [&](auto& entry, auto& directory) -> ErrorOr<IterationDecision> {
        if (!entry.name.starts_with("ca-"sv))
            return IterationDecision::Continue;

        // The generic calendar is not a supported Unicode calendar key, so skip it:
        // https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Intl/Locale/calendar#unicode_calendar_keys
        if (entry.name == "ca-generic.json"sv)
            return IterationDecision::Continue;

        auto locale_calendars_path = LexicalPath::join(directory.path().string(), entry.name).string();
        LexicalPath calendars_path(move(locale_calendars_path));

        auto calendars = TRY(read_json_file(calendars_path.string()));
        auto const& main_object = calendars.as_object().get_object("main"sv).value();
        auto const& locale_object = main_object.get_object(calendars_path.parent().basename()).value();
        auto const& dates_object = locale_object.get_object("dates"sv).value();
        auto const& calendars_object = dates_object.get_object("calendars"sv).value();

        calendars_object.for_each_member([&](auto calendar_name, JsonValue const&) {
            if (auto calendar_alias = find_keyword_alias("ca"sv, calendar_name, cldr); calendar_alias.has_value())
                calendar_name = calendar_alias.release_value();

            keywords.append(cldr.unique_strings.ensure(calendar_name));
        });

        return IterationDecision::Continue;
    }));

    locale.calendar_keywords = cldr.unique_keyword_lists.ensure(move(keywords));
    return {};
}

static void fill_in_collation_keywords(CLDR& cldr, LocaleData& locale)
{
    // FIXME: If collation data becomes available in the CLDR, parse per-locale ordering from there.
    auto create_list_with_default_first = [&](auto key, auto default_value) {
        auto& values = cldr.keywords.find(key)->value;

        quick_sort(values, [&](auto const& lhs, auto const& rhs) {
            if (lhs == default_value)
                return true;
            if (rhs == default_value)
                return false;
            return lhs < rhs;
        });

        KeywordList keywords;
        keywords.ensure_capacity(values.size());

        for (auto const& value : values)
            keywords.append(cldr.unique_strings.ensure(value));

        return cldr.unique_keyword_lists.ensure(move(keywords));
    };

    static auto kf_index = create_list_with_default_first("kf"sv, "upper"sv);
    static auto kn_index = create_list_with_default_first("kn"sv, "true"sv);

    locale.collation_case_keywords = kf_index;
    locale.collation_numeric_keywords = kn_index;
}

static ErrorOr<void> parse_default_content_locales(ByteString core_path, CLDR& cldr)
{
    LexicalPath default_content_path(move(core_path));
    default_content_path = default_content_path.append("defaultContent.json"sv);

    auto default_content = TRY(read_json_file(default_content_path.string()));
    auto const& default_content_array = default_content.as_object().get_array("defaultContent"sv).value();

    default_content_array.for_each([&](JsonValue const& value) {
        auto locale = value.as_string();
        StringView default_locale = locale;

        while (true) {
            if (cldr.locales.contains(default_locale))
                break;

            auto pos = default_locale.find_last('-');
            if (!pos.has_value())
                return;

            default_locale = default_locale.substring_view(0, *pos);
        }

        if (default_locale != locale)
            cldr.locale_aliases.append({ default_locale, move(locale) });
    });

    return {};
}

static ErrorOr<void> define_aliases_without_scripts(CLDR& cldr)
{
    // From ECMA-402: https://tc39.es/ecma402/#sec-internal-slots
    //
    //     For locales that include a script subtag in addition to language and region, the
    //     corresponding locale without a script subtag must also be supported.
    //
    // So we define aliases for locales that contain all three subtags, but we must also take
    // care to handle when the locale itself or the locale without a script subtag are an alias
    // by way of default-content locales.
    auto find_alias = [&](auto const& locale) {
        return cldr.locale_aliases.find_if([&](auto const& alias) { return locale == alias.alias; });
    };

    auto append_alias_without_script = [&](auto const& locale) -> ErrorOr<void> {
        auto parsed_locale = TRY(CanonicalLanguageID::parse(cldr.unique_strings, locale));
        if ((parsed_locale.language == 0) || (parsed_locale.script == 0) || (parsed_locale.region == 0))
            return {};

        auto locale_without_script = ByteString::formatted("{}-{}",
            cldr.unique_strings.get(parsed_locale.language),
            cldr.unique_strings.get(parsed_locale.region));

        if (cldr.locales.contains(locale_without_script))
            return {};
        if (find_alias(locale_without_script) != cldr.locale_aliases.end())
            return {};

        if (auto it = find_alias(locale); it != cldr.locale_aliases.end())
            cldr.locale_aliases.append({ it->name, locale_without_script });
        else
            cldr.locale_aliases.append({ locale, locale_without_script });

        return {};
    };

    for (auto const& locale : cldr.locales)
        TRY(append_alias_without_script(locale.key));
    for (auto const& locale : cldr.locale_aliases)
        TRY(append_alias_without_script(locale.alias));

    return {};
}

static ErrorOr<void> parse_all_locales(ByteString bcp47_path, ByteString core_path, ByteString numbers_path, ByteString dates_path, CLDR& cldr)
{
    LexicalPath core_supplemental_path(core_path);
    core_supplemental_path = core_supplemental_path.append("supplemental"sv);
    VERIFY(FileSystem::is_directory(core_supplemental_path.string()));

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

    TRY(Core::Directory::for_each_entry(TRY(String::formatted("{}/bcp47", bcp47_path)), Core::DirIterator::SkipParentAndBaseDir, [&](auto& entry, auto& directory) -> ErrorOr<IterationDecision> {
        auto bcp47_path = LexicalPath::join(directory.path().string(), entry.name).string();
        TRY(parse_unicode_extension_keywords(move(bcp47_path), cldr));
        return IterationDecision::Continue;
    }));

    TRY(Core::Directory::for_each_entry(TRY(String::formatted("{}/main", numbers_path)), Core::DirIterator::SkipParentAndBaseDir, [&](auto& entry, auto& directory) -> ErrorOr<IterationDecision> {
        auto numbers_path = LexicalPath::join(directory.path().string(), entry.name).string();
        auto language = TRY(remove_variants_from_path(numbers_path));

        auto& locale = cldr.locales.ensure(language);
        TRY(parse_number_system_keywords(numbers_path, cldr, locale));
        fill_in_collation_keywords(cldr, locale);
        return IterationDecision::Continue;
    }));

    TRY(Core::Directory::for_each_entry(TRY(String::formatted("{}/main", dates_path)), Core::DirIterator::SkipParentAndBaseDir, [&](auto& entry, auto& directory) -> ErrorOr<IterationDecision> {
        auto dates_path = LexicalPath::join(directory.path().string(), entry.name).string();
        auto language = TRY(remove_variants_from_path(dates_path));

        auto& locale = cldr.locales.ensure(language);
        TRY(parse_calendar_keywords(dates_path, cldr, locale));
        return IterationDecision::Continue;
    }));

    TRY(parse_default_content_locales(move(core_path), cldr));
    TRY(define_aliases_without_scripts(cldr));

    return {};
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

    auto locales = cldr.locales.keys();
    auto keywords = cldr.keywords.keys();

    generate_enum(generator, format_identifier, "Locale"sv, "None"sv, locales, cldr.locale_aliases);
    generate_enum(generator, format_identifier, "Key"sv, {}, keywords);

    for (auto& keyword : cldr.keywords) {
        auto const& keyword_name = cldr.keyword_names.find(keyword.key)->value;
        auto enum_name = ByteString::formatted("Keyword{}", format_identifier({}, keyword_name));

        if (auto aliases = cldr.keyword_aliases.find(keyword.key); aliases != cldr.keyword_aliases.end())
            generate_enum(generator, format_identifier, enum_name, {}, keyword.value, aliases->value);
        else
            generate_enum(generator, format_identifier, enum_name, {}, keyword.value);
    }

    generator.append(R"~~~(
}
)~~~");

    TRY(file.write_until_depleted(generator.as_string_view().bytes()));
    return {};
}

static ErrorOr<void> generate_unicode_locale_implementation(Core::InputBufferedFile& file, CLDR& cldr)
{
    auto string_index_type = cldr.unique_strings.type_that_fits();

    StringBuilder builder;
    SourceGenerator generator { builder };
    generator.set("string_index_type"sv, string_index_type);
    generator.set("locales_size"sv, ByteString::number(cldr.locales.size()));

    generator.append(R"~~~(
#include <AK/Array.h>
#include <AK/BinarySearch.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibLocale/DateTimeFormat.h>
#include <LibLocale/Locale.h>
#include <LibLocale/LocaleData.h>
#include <LibUnicode/CurrencyCode.h>

namespace Locale {
)~~~");

    cldr.unique_strings.generate(generator);

    generate_available_values(generator, "get_available_calendars"sv, cldr.keywords.find("ca"sv)->value, cldr.keyword_aliases.find("ca"sv)->value,
        [](auto calendar) {
            // FIXME: Remove this filter when we support all calendars.
            return calendar.is_one_of("gregory"sv, "iso8601"sv);
        });
    generate_available_values(generator, "get_available_collation_case_orderings"sv, cldr.keywords.find("kf"sv)->value, cldr.keyword_aliases.find("kf"sv)->value);
    generate_available_values(generator, "get_available_collation_numeric_orderings"sv, cldr.keywords.find("kn"sv)->value, cldr.keyword_aliases.find("kn"sv)->value);
    generate_available_values(generator, "get_available_collation_types"sv, cldr.keywords.find("co"sv)->value, cldr.keyword_aliases.find("co"sv)->value,
        [](auto collation) {
            // FIXME: Remove this filter when we support all collation types.
            return collation == "default"sv;
        });
    generate_available_values(generator, "get_available_hour_cycles"sv, cldr.keywords.find("hc"sv)->value);
    generate_available_values(generator, "get_available_number_systems"sv, cldr.keywords.find("nu"sv)->value);

    generator.append(R"~~~(
ReadonlySpan<StringView> get_available_keyword_values(StringView key)
{
    auto key_value = key_from_string(key);
    if (!key_value.has_value())
        return {};

    switch (*key_value) {
    case Key::Ca:
        return get_available_calendars();
    case Key::Co:
        return get_available_collation_types();
    case Key::Hc:
        return get_available_hour_cycles();
    case Key::Kf:
        return get_available_collation_case_orderings();
    case Key::Kn:
        return get_available_collation_numeric_orderings();
    case Key::Nu:
        return get_available_number_systems();
    }

    VERIFY_NOT_REACHED();
}
)~~~");

    cldr.unique_keyword_lists.generate(generator, string_index_type, "s_keyword_lists"sv);

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

    append_mapping(locales, cldr.locales, cldr.unique_keyword_lists.type_that_fits(), "s_calendar_keywords"sv, [&](auto const& locale) { return locale.calendar_keywords; });
    append_mapping(locales, cldr.locales, cldr.unique_keyword_lists.type_that_fits(), "s_collation_case_keywords"sv, [&](auto const& locale) { return locale.collation_case_keywords; });
    append_mapping(locales, cldr.locales, cldr.unique_keyword_lists.type_that_fits(), "s_collation_numeric_keywords"sv, [&](auto const& locale) { return locale.collation_numeric_keywords; });
    append_mapping(locales, cldr.locales, cldr.unique_keyword_lists.type_that_fits(), "s_number_system_keywords"sv, [&](auto const& locale) { return locale.number_system_keywords; });

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

    TRY(append_from_string("Locale"sv, "locale"sv, cldr.locales.keys(), cldr.locale_aliases));
    TRY(append_from_string("Key"sv, "key"sv, cldr.keywords.keys()));

    for (auto const& keyword : cldr.keywords) {
        auto const& keyword_name = cldr.keyword_names.find(keyword.key)->value;
        auto enum_name = ByteString::formatted("Keyword{}", format_identifier({}, keyword_name));
        auto enum_snake = ByteString::formatted("keyword_{}", keyword.key);

        if (auto aliases = cldr.keyword_aliases.find(keyword.key); aliases != cldr.keyword_aliases.end())
            TRY(append_from_string(enum_name, enum_snake, keyword.value, aliases->value));
        else
            TRY(append_from_string(enum_name, enum_snake, keyword.value));
    }

    generator.append(R"~~~(
static ReadonlySpan<@string_index_type@> find_keyword_indices(StringView locale, StringView key)
{
    auto locale_value = locale_from_string(locale);
    if (!locale_value.has_value())
        return {};

    auto key_value = key_from_string(key);
    if (!key_value.has_value())
        return {};

    auto locale_index = to_underlying(*locale_value) - 1; // Subtract 1 because 0 == Locale::None.
    size_t keywords_index = 0;

    switch (*key_value) {
    case Key::Ca:
        keywords_index = s_calendar_keywords.at(locale_index);
        break;
    case Key::Kf:
        keywords_index = s_collation_case_keywords.at(locale_index);
        break;
    case Key::Kn:
        keywords_index = s_collation_numeric_keywords.at(locale_index);
        break;
    case Key::Nu:
        keywords_index = s_number_system_keywords.at(locale_index);
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    return s_keyword_lists.at(keywords_index);
}

Optional<StringView> get_preferred_keyword_value_for_locale(StringView locale, StringView key)
{
    // Hour cycle keywords are region-based rather than locale-based, so they need to be handled specially.
    // FIXME: Calendar keywords are also region-based, and will need to be handled here when we support non-Gregorian calendars:
    //        https://github.com/unicode-org/cldr-json/blob/main/cldr-json/cldr-core/supplemental/calendarPreferenceData.json
    if (key == "hc"sv) {
        if (auto hour_cycle = default_hour_cycle(locale); hour_cycle.has_value())
            return hour_cycle_to_string(*hour_cycle);
        return {};
    }

    // FIXME: Generate locale-preferred collation data when available in the CLDR.
    if (key == "co"sv) {
        auto collations = get_available_collation_types();
        if (collations.is_empty())
            return OptionalNone {};

        return Optional<StringView> { collations[0] };
    }

    auto keyword_indices = find_keyword_indices(locale, key);
    if (keyword_indices.is_empty())
        return OptionalNone {};

    return Optional<StringView> { decode_string(keyword_indices[0]) };
}

Vector<StringView> get_keywords_for_locale(StringView locale, StringView key)
{
    // Hour cycle keywords are region-based rather than locale-based, so they need to be handled specially.
    // FIXME: Calendar keywords are also region-based, and will need to be handled here when we support non-Gregorian calendars:
    //        https://github.com/unicode-org/cldr-json/blob/main/cldr-json/cldr-core/supplemental/calendarPreferenceData.json
    if (key == "hc"sv) {
        if (auto hour_cycle = default_hour_cycle(locale); hour_cycle.has_value())
            return { hour_cycle_to_string(*hour_cycle) };
        return {};
    }

    // FIXME: Generate locale-preferred collation data when available in the CLDR.
    if (key == "co"sv)
        return Vector<StringView> { get_available_collation_types() };

    auto keyword_indices = find_keyword_indices(locale, key);

    Vector<StringView> keywords;
    keywords.ensure_capacity(keyword_indices.size());

    for (auto keyword : keyword_indices)
        keywords.unchecked_append(decode_string(keyword));

    return keywords;
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
    StringView bcp47_path;
    StringView core_path;
    StringView numbers_path;
    StringView dates_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the Unicode locale header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Unicode locale implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(bcp47_path, "Path to cldr-bcp47 directory", "bcp47-path", 'b', "bcp47-path");
    args_parser.add_option(core_path, "Path to cldr-core directory", "core-path", 'r', "core-path");
    args_parser.add_option(numbers_path, "Path to cldr-numbers directory", "numbers-path", 'n', "numbers-path");
    args_parser.add_option(dates_path, "Path to cldr-dates directory", "dates-path", 'd', "dates-path");
    args_parser.parse(arguments);

    auto generated_header_file = TRY(open_file(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(open_file(generated_implementation_path, Core::File::OpenMode::Write));

    CLDR cldr;
    TRY(parse_all_locales(bcp47_path, core_path, numbers_path, dates_path, cldr));

    TRY(generate_unicode_locale_header(*generated_header_file, cldr));
    TRY(generate_unicode_locale_implementation(*generated_implementation_file, cldr));

    return 0;
}
