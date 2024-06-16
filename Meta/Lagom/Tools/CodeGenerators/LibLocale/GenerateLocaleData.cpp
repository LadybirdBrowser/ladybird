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

struct LocaleData {
};

struct CLDR {
    UniqueStringStorage unique_strings;

    HashMap<ByteString, LocaleData> locales;
    Vector<Alias> locale_aliases;
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

static ErrorOr<void> parse_all_locales(ByteString core_path, ByteString numbers_path, CLDR& cldr)
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

    TRY(Core::Directory::for_each_entry(TRY(String::formatted("{}/main", numbers_path)), Core::DirIterator::SkipParentAndBaseDir, [&](auto& entry, auto& directory) -> ErrorOr<IterationDecision> {
        auto numbers_path = LexicalPath::join(directory.path().string(), entry.name).string();
        auto language = TRY(remove_variants_from_path(numbers_path));

        cldr.locales.ensure(language);
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
    generate_enum(generator, format_identifier, "Locale"sv, "None"sv, locales, cldr.locale_aliases);

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

    auto locales = cldr.locales.keys();
    quick_sort(locales);

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
    StringView numbers_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(generated_header_path, "Path to the Unicode locale header file to generate", "generated-header-path", 'h', "generated-header-path");
    args_parser.add_option(generated_implementation_path, "Path to the Unicode locale implementation file to generate", "generated-implementation-path", 'c', "generated-implementation-path");
    args_parser.add_option(core_path, "Path to cldr-core directory", "core-path", 'r', "core-path");
    args_parser.add_option(numbers_path, "Path to cldr-numbers directory", "numbers-path", 'n', "numbers-path");
    args_parser.parse(arguments);

    auto generated_header_file = TRY(open_file(generated_header_path, Core::File::OpenMode::Write));
    auto generated_implementation_file = TRY(open_file(generated_implementation_path, Core::File::OpenMode::Write));

    CLDR cldr;
    TRY(parse_all_locales(core_path, numbers_path, cldr));

    TRY(generate_unicode_locale_header(*generated_header_file, cldr));
    TRY(generate_unicode_locale_implementation(*generated_implementation_file, cldr));

    return 0;
}
