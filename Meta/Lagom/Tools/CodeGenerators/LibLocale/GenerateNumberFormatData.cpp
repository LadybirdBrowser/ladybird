/*
 * Copyright (c) 2021-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../LibUnicode/GeneratorUtil.h" // FIXME: Move this somewhere common.
#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/HashFunctions.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <AK/Traits.h>
#include <AK/Utf8View.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibFileSystem/FileSystem.h>
#include <LibLocale/NumberFormat.h>
#include <math.h>

using NumericSymbolList = Vector<size_t>;

struct NumberSystem {
    unsigned hash() const { return int_hash(symbols); }
    bool operator==(NumberSystem const& other) const { return (symbols == other.symbols); }

    size_t symbols { 0 };
};

template<>
struct AK::Formatter<NumberSystem> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, NumberSystem const& system)
    {
        return Formatter<FormatString>::format(builder, "{{ {} }}"sv, system.symbols);
    }
};

template<>
struct AK::Traits<NumberSystem> : public DefaultTraits<NumberSystem> {
    static unsigned hash(NumberSystem const& s) { return s.hash(); }
};

struct LocaleData {
    Vector<size_t> number_systems;
};

struct CLDR {
    UniqueStringStorage unique_strings;
    UniqueStorage<NumericSymbolList> unique_symbols;
    UniqueStorage<NumberSystem> unique_systems;

    HashMap<ByteString, Array<u32, 10>> number_system_digits;
    Vector<ByteString> number_systems;

    HashMap<ByteString, LocaleData> locales;
};

static ErrorOr<void> parse_number_system_digits(ByteString core_supplemental_path, CLDR& cldr)
{
    LexicalPath number_systems_path(move(core_supplemental_path));
    number_systems_path = number_systems_path.append("numberingSystems.json"sv);

    auto number_systems = TRY(read_json_file(number_systems_path.string()));
    auto const& supplemental_object = number_systems.as_object().get_object("supplemental"sv).value();
    auto const& number_systems_object = supplemental_object.get_object("numberingSystems"sv).value();

    number_systems_object.for_each_member([&](auto const& number_system, auto const& digits_object) {
        auto type = digits_object.as_object().get_byte_string("_type"sv).value();
        if (type != "numeric"sv)
            return;

        auto digits = digits_object.as_object().get_byte_string("_digits"sv).value();

        Utf8View utf8_digits { digits };
        VERIFY(utf8_digits.length() == 10);

        auto& number_system_digits = cldr.number_system_digits.ensure(number_system);
        size_t index = 0;

        for (u32 digit : utf8_digits)
            number_system_digits[index++] = digit;

        if (!cldr.number_systems.contains_slow(number_system))
            cldr.number_systems.append(number_system);
    });

    return {};
}

static ErrorOr<void> parse_number_systems(ByteString locale_numbers_path, CLDR& cldr, LocaleData& locale)
{
    LexicalPath numbers_path(move(locale_numbers_path));
    numbers_path = numbers_path.append("numbers.json"sv);

    auto numbers = TRY(read_json_file(numbers_path.string()));
    auto const& main_object = numbers.as_object().get_object("main"sv).value();
    auto const& locale_object = main_object.get_object(numbers_path.parent().basename()).value();
    auto const& locale_numbers_object = locale_object.get_object("numbers"sv).value();
    auto const& minimum_grouping_digits = locale_numbers_object.get_byte_string("minimumGroupingDigits"sv).value();

    Vector<Optional<NumberSystem>> number_systems;
    number_systems.resize(cldr.number_systems.size());

    auto ensure_number_system = [&](auto const& system) -> NumberSystem& {
        auto system_index = cldr.number_systems.find_first_index(system).value();
        VERIFY(system_index < number_systems.size());

        auto& number_system = number_systems.at(system_index);
        if (!number_system.has_value())
            number_system = NumberSystem {};

        return number_system.value();
    };

    auto numeric_symbol_from_string = [&](StringView numeric_symbol) -> Optional<Locale::NumericSymbol> {
        if (numeric_symbol == "approximatelySign"sv)
            return Locale::NumericSymbol::ApproximatelySign;
        if (numeric_symbol == "decimal"sv)
            return Locale::NumericSymbol::Decimal;
        if (numeric_symbol == "exponential"sv)
            return Locale::NumericSymbol::Exponential;
        if (numeric_symbol == "group"sv)
            return Locale::NumericSymbol::Group;
        if (numeric_symbol == "infinity"sv)
            return Locale::NumericSymbol::Infinity;
        if (numeric_symbol == "minusSign"sv)
            return Locale::NumericSymbol::MinusSign;
        if (numeric_symbol == "nan"sv)
            return Locale::NumericSymbol::NaN;
        if (numeric_symbol == "percentSign"sv)
            return Locale::NumericSymbol::PercentSign;
        if (numeric_symbol == "plusSign"sv)
            return Locale::NumericSymbol::PlusSign;
        if (numeric_symbol == "timeSeparator"sv)
            return Locale::NumericSymbol::TimeSeparator;
        return {};
    };

    locale_numbers_object.for_each_member([&](auto const& key, JsonValue const& value) {
        constexpr auto symbols_prefix = "symbols-numberSystem-"sv;
        constexpr auto misc_patterns_prefix = "miscPatterns-numberSystem-"sv;

        if (key.starts_with(symbols_prefix)) {
            auto system = key.substring(symbols_prefix.length());
            auto& number_system = ensure_number_system(system);

            NumericSymbolList symbols;

            value.as_object().for_each_member([&](auto const& symbol, JsonValue const& localization) {
                auto numeric_symbol = numeric_symbol_from_string(symbol);
                if (!numeric_symbol.has_value())
                    return;

                if (to_underlying(*numeric_symbol) >= symbols.size())
                    symbols.resize(to_underlying(*numeric_symbol) + 1);

                auto symbol_index = cldr.unique_strings.ensure(localization.as_string());
                symbols[to_underlying(*numeric_symbol)] = symbol_index;
            });

            // The range separator does not appear in the symbols list, we have to extract it from
            // the range pattern.
            auto misc_patterns_key = ByteString::formatted("{}{}", misc_patterns_prefix, system);
            auto misc_patterns = locale_numbers_object.get_object(misc_patterns_key).value();
            auto range_separator = misc_patterns.get_byte_string("range"sv).value();

            auto begin_index = range_separator.find("{0}"sv).value() + "{0}"sv.length();
            auto end_index = range_separator.find("{1}"sv).value();
            range_separator = range_separator.substring(begin_index, end_index - begin_index);

            if (to_underlying(Locale::NumericSymbol::RangeSeparator) >= symbols.size())
                symbols.resize(to_underlying(Locale::NumericSymbol::RangeSeparator) + 1);

            auto symbol_index = cldr.unique_strings.ensure(move(range_separator));
            symbols[to_underlying(Locale::NumericSymbol::RangeSeparator)] = symbol_index;

            number_system.symbols = cldr.unique_symbols.ensure(move(symbols));
        }
    });

    locale.number_systems.ensure_capacity(number_systems.size());

    for (auto& number_system : number_systems) {
        size_t system_index = 0;
        if (number_system.has_value())
            system_index = cldr.unique_systems.ensure(number_system.release_value());

        locale.number_systems.append(system_index);
    }

    // locale.minimum_grouping_digits = minimum_grouping_digits.template to_number<u8>().value();
    return {};
}

static ErrorOr<void> parse_all_locales(ByteString core_path, ByteString numbers_path, CLDR& cldr)
{
    LexicalPath core_supplemental_path(move(core_path));
    core_supplemental_path = core_supplemental_path.append("supplemental"sv);
    VERIFY(FileSystem::is_directory(core_supplemental_path.string()));

    TRY(parse_number_system_digits(core_supplemental_path.string(), cldr));

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

        auto& locale = cldr.locales.ensure(language);
        TRY(parse_number_systems(numbers_path, cldr, locale));
        return IterationDecision::Continue;
    }));

    return {};
}

static ByteString format_identifier(StringView, ByteString const& identifier)
{
    return identifier.to_titlecase();
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

    generate_enum(generator, format_identifier, "NumberSystem"sv, {}, cldr.number_systems);

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
    generator.set("numeric_symbol_list_index_type"sv, cldr.unique_symbols.type_that_fits());

    generator.append(R"~~~(
#include <AK/Array.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <LibLocale/Locale.h>
#include <LibLocale/LocaleData.h>
#include <LibLocale/NumberFormat.h>
#include <LibLocale/NumberFormatData.h>

namespace Locale {
)~~~");

    cldr.unique_strings.generate(generator);

    generator.append(R"~~~(
struct NumberSystemData {
    @numeric_symbol_list_index_type@ symbols { 0 };
};
)~~~");

    cldr.unique_symbols.generate(generator, cldr.unique_strings.type_that_fits(), "s_numeric_symbol_lists"sv);
    cldr.unique_systems.generate(generator, "NumberSystemData"sv, "s_number_systems"sv, 10);

    auto locales = cldr.locales.keys();
    quick_sort(locales);

    auto append_map = [&](ByteString name, auto type, auto const& map) {
        generator.set("name", move(name));
        generator.set("type", type);
        generator.set("size", ByteString::number(map.size()));

        generator.append(R"~~~(
static constexpr Array<@type@, @size@> @name@ { {)~~~");

        bool first = true;
        for (auto const& item : map) {
            generator.append(first ? " "sv : ", "sv);
            if constexpr (requires { item.value; })
                generator.append(ByteString::number(item.value));
            else
                generator.append(ByteString::number(item));
            first = false;
        }

        generator.append(" } };");
    };

    generate_mapping(generator, cldr.number_system_digits, "u32"sv, "s_number_systems_digits"sv, "s_number_systems_digits_{}"sv, nullptr, [&](auto const& name, auto const& value) { append_map(name, "u32"sv, value); });
    generate_mapping(generator, cldr.locales, cldr.unique_systems.type_that_fits(), "s_locale_number_systems"sv, "s_number_systems_{}"sv, nullptr, [&](auto const& name, auto const& value) { append_map(name, cldr.unique_systems.type_that_fits(), value.number_systems); });

    generator.append(R"~~~(
static Optional<NumberSystem> keyword_to_number_system(KeywordNumbers keyword)
{
    switch (keyword) {)~~~");

    for (auto const& number_system : cldr.number_systems) {
        generator.set("name"sv, format_identifier({}, number_system));
        generator.append(R"~~~(
    case KeywordNumbers::@name@:
        return NumberSystem::@name@;)~~~");
    }

    generator.append(R"~~~(
    default:
        return {};
    }
}

Optional<ReadonlySpan<u32>> get_digits_for_number_system(StringView system)
{
    auto number_system_keyword = keyword_nu_from_string(system);
    if (!number_system_keyword.has_value())
        return {};

    auto number_system_value = keyword_to_number_system(*number_system_keyword);
    if (!number_system_value.has_value())
        return {};

    auto number_system_index = to_underlying(*number_system_value);
    return s_number_systems_digits[number_system_index];
}

static NumberSystemData const* find_number_system(StringView locale, StringView system)
{
    auto locale_value = locale_from_string(locale);
    if (!locale_value.has_value())
        return nullptr;

    auto locale_index = to_underlying(*locale_value) - 1; // Subtract 1 because 0 == Locale::None.
    auto const& number_systems = s_locale_number_systems.at(locale_index);

    auto lookup_number_system = [&](auto number_system) -> NumberSystemData const* {
        auto number_system_keyword = keyword_nu_from_string(number_system);
        if (!number_system_keyword.has_value())
            return nullptr;

        auto number_system_value = keyword_to_number_system(*number_system_keyword);
        if (!number_system_value.has_value())
            return nullptr;

        auto number_system_index = to_underlying(*number_system_value);
        number_system_index = number_systems.at(number_system_index);

        if (number_system_index == 0)
            return nullptr;

        return &s_number_systems.at(number_system_index);
    };

    if (auto const* number_system = lookup_number_system(system))
        return number_system;

    auto default_number_system = get_preferred_keyword_value_for_locale(locale, "nu"sv);
    if (!default_number_system.has_value())
        return nullptr;

    return lookup_number_system(*default_number_system);
}

Optional<StringView> get_number_system_symbol(StringView locale, StringView system, NumericSymbol symbol)
{
    if (auto const* number_system = find_number_system(locale, system); number_system != nullptr) {
        auto symbols = s_numeric_symbol_lists.at(number_system->symbols);

        auto symbol_index = to_underlying(symbol);
        if (symbol_index >= symbols.size())
            return {};

        return decode_string(symbols[symbol_index]);
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
