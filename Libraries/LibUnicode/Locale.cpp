/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/GenericLexer.h>
#include <AK/HashTable.h>
#include <AK/NeverDestroyed.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16StringBuilder.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/Locale.h>

#include <unicode/localebuilder.h>
#include <unicode/locid.h>

namespace Unicode {

template<typename ViewType>
static constexpr size_t view_length(ViewType const& view)
{
    if constexpr (IsSame<ViewType, Utf16View>)
        return view.length_in_code_units();
    else
        return view.length();
}

template<typename ViewType>
static constexpr u32 view_code_unit_at(ViewType const& view, size_t index)
{
    if constexpr (IsSame<ViewType, Utf16View>)
        return view.code_unit_at(index);
    else
        return static_cast<u8>(view[index]);
}

static Utf16String utf16_string_from_ascii_view(StringView string)
{
    return Utf16String::from_ascii_without_validation(string.bytes());
}

static Utf16String utf16_string_from_ascii_view(Utf16View string)
{
    return Utf16String::from_utf16(string);
}

template<typename ViewType>
static bool is_key(ViewType key)
{
    // key = alphanum alpha
    if (view_length(key) != 2)
        return false;
    return is_ascii_alphanumeric(view_code_unit_at(key, 0)) && is_ascii_alpha(view_code_unit_at(key, 1));
}

static bool is_single_type(StringView type)
{
    // type = alphanum{3,8} (sep alphanum{3,8})*
    // Note: Consecutive types are not handled here, that is left to the caller.
    if ((type.length() < 3) || (type.length() > 8))
        return false;
    return all_of(type, is_ascii_alphanumeric);
}

static bool is_single_type(Utf16View type)
{
    // type = alphanum{3,8} (sep alphanum{3,8})*
    // Note: Consecutive types are not handled here, that is left to the caller.
    if ((type.length_in_code_units() < 3) || (type.length_in_code_units() > 8))
        return false;
    return all_of(type, is_ascii_alphanumeric);
}

template<typename ViewType>
static bool is_attribute(ViewType type)
{
    // attribute = alphanum{3,8}
    if ((view_length(type) < 3) || (view_length(type) > 8))
        return false;
    return all_of(type, is_ascii_alphanumeric);
}

template<typename ViewType>
static bool is_transformed_key(ViewType key)
{
    // tkey = alpha digit
    if (view_length(key) != 2)
        return false;
    return is_ascii_alpha(view_code_unit_at(key, 0)) && is_ascii_digit(view_code_unit_at(key, 1));
}

template<typename ViewType>
static bool is_single_transformed_value(ViewType value)
{
    // tvalue = (sep alphanum{3,8})+
    // Note: Consecutive values are not handled here, that is left to the caller.
    if ((view_length(value) < 3) || (view_length(value) > 8))
        return false;
    return all_of(value, is_ascii_alphanumeric);
}

template<typename Lexer>
static Optional<typename Lexer::ViewType> consume_next_segment(Lexer& lexer, bool with_separator = true)
{
    constexpr auto is_separator = [](auto code_unit) { return code_unit == '-' || code_unit == '_'; };

    if (with_separator) {
        if (!lexer.next_is(is_separator))
            return {};
        lexer.ignore();
    }

    auto segment = lexer.consume_until(is_separator);
    if (segment.is_empty()) {
        lexer.retreat(with_separator);
        return {};
    }

    return segment;
}

bool is_type_identifier(StringView identifier)
{
    // type = alphanum{3,8} (sep alphanum{3,8})*
    GenericLexer lexer { identifier };

    while (true) {
        auto type = consume_next_segment(lexer, lexer.tell() > 0);
        if (!type.has_value())
            break;
        if (!is_single_type(*type))
            return false;
    }

    return lexer.is_eof() && (lexer.tell() > 0);
}

bool is_type_identifier(Utf16View identifier)
{
    // type = alphanum{3,8} (sep alphanum{3,8})*
    bool saw_type = false;
    bool is_valid = true;
    size_t start = 0;

    auto validate_type = [&](Utf16View type) {
        saw_type = true;
        if (type.is_empty() || !is_single_type(type)) {
            is_valid = false;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    };

    for (size_t i = 0; i < identifier.length_in_code_units(); ++i) {
        auto code_unit = identifier.code_unit_at(i);
        if (code_unit != '-' && code_unit != '_')
            continue;

        if (validate_type(identifier.substring_view(start, i - start)) == IterationDecision::Break)
            return false;
        start = i + 1;
    }

    if (validate_type(identifier.substring_view(start)) == IterationDecision::Break)
        return false;

    return saw_type && is_valid;
}

template<typename Lexer>
static Optional<LanguageID> parse_unicode_language_id_from_lexer(Lexer& lexer)
{
    // https://unicode.org/reports/tr35/#Unicode_language_identifier
    //
    // unicode_language_id = "root"
    //     OR
    // unicode_language_id = ((unicode_language_subtag (sep unicode_script_subtag)?) | unicode_script_subtag)
    //                       (sep unicode_region_subtag)?
    //                       (sep unicode_variant_subtag)*
    LanguageID language_id {};

    if (lexer.consume_specific("root"sv)) {
        language_id.is_root = true;
        return language_id;
    }

    enum class ParseState {
        ParsingLanguageOrScript,
        ParsingScript,
        ParsingRegion,
        ParsingVariant,
        Done,
    };

    auto state = ParseState::ParsingLanguageOrScript;

    while (!lexer.is_eof() && (state != ParseState::Done)) {
        auto segment = consume_next_segment(lexer, state != ParseState::ParsingLanguageOrScript);
        if (!segment.has_value())
            return {};

        switch (state) {
        case ParseState::ParsingLanguageOrScript:
            if (is_unicode_language_subtag(*segment)) {
                state = ParseState::ParsingScript;
                language_id.language = utf16_string_from_ascii_view(*segment);
            } else if (is_unicode_script_subtag(*segment)) {
                state = ParseState::ParsingRegion;
                language_id.script = utf16_string_from_ascii_view(*segment);
            } else {
                return {};
            }
            break;

        case ParseState::ParsingScript:
            if (is_unicode_script_subtag(*segment)) {
                state = ParseState::ParsingRegion;
                language_id.script = utf16_string_from_ascii_view(*segment);
                break;
            }

            state = ParseState::ParsingRegion;
            [[fallthrough]];

        case ParseState::ParsingRegion:
            if (is_unicode_region_subtag(*segment)) {
                state = ParseState::ParsingVariant;
                language_id.region = utf16_string_from_ascii_view(*segment);
                break;
            }

            state = ParseState::ParsingVariant;
            [[fallthrough]];

        case ParseState::ParsingVariant:
            if (is_unicode_variant_subtag(*segment)) {
                language_id.variants.append(utf16_string_from_ascii_view(*segment));
            } else {
                lexer.retreat(view_length(*segment) + 1);
                state = ParseState::Done;
            }
            break;

        default:
            VERIFY_NOT_REACHED();
        }
    }

    return language_id;
}

template<typename Lexer>
static Optional<LocaleExtension> parse_unicode_locale_extension(Lexer& lexer)
{
    // https://unicode.org/reports/tr35/#unicode_locale_extensions
    //
    // unicode_locale_extensions = sep [uU] ((sep keyword)+ | (sep attribute)+ (sep keyword)*)
    LocaleExtension locale_extension {};

    enum class ParseState {
        ParsingAttributeOrKeyword,
        ParsingAttribute,
        ParsingKeyword,
        Done,
    };

    auto state = ParseState::ParsingAttributeOrKeyword;

    while (!lexer.is_eof() && (state != ParseState::Done)) {
        auto segment = consume_next_segment(lexer);
        if (!segment.has_value())
            return {};

        if (state == ParseState::ParsingAttributeOrKeyword)
            state = is_key(*segment) ? ParseState::ParsingKeyword : ParseState::ParsingAttribute;

        switch (state) {
        case ParseState::ParsingAttribute:
            if (is_attribute(*segment)) {
                locale_extension.attributes.append(utf16_string_from_ascii_view(*segment));
                break;
            }

            state = ParseState::ParsingKeyword;
            [[fallthrough]];

        case ParseState::ParsingKeyword: {
            // keyword = key (sep type)?
            Keyword keyword { .key = utf16_string_from_ascii_view(*segment) };
            Vector<Utf16String> keyword_values;

            if (!is_key(*segment)) {
                lexer.retreat(view_length(*segment) + 1);
                state = ParseState::Done;
                break;
            }

            while (true) {
                auto type = consume_next_segment(lexer);

                if (!type.has_value() || !is_single_type(*type)) {
                    if (type.has_value())
                        lexer.retreat(view_length(*type) + 1);
                    break;
                }

                keyword_values.append(utf16_string_from_ascii_view(*type));
            }

            keyword.value = Utf16String::join("-"sv, keyword_values);

            locale_extension.keywords.append(move(keyword));
            break;
        }

        default:
            VERIFY_NOT_REACHED();
        }
    }

    if (locale_extension.attributes.is_empty() && locale_extension.keywords.is_empty())
        return {};
    return locale_extension;
}

template<typename Lexer>
static Optional<TransformedExtension> parse_transformed_extension(Lexer& lexer)
{
    // https://unicode.org/reports/tr35/#transformed_extensions
    //
    // transformed_extensions = sep [tT] ((sep tlang (sep tfield)*) | (sep tfield)+)
    TransformedExtension transformed_extension {};

    enum class ParseState {
        ParsingLanguageOrField,
        ParsingLanguage,
        ParsingField,
        Done,
    };

    auto state = ParseState::ParsingLanguageOrField;

    while (!lexer.is_eof() && (state != ParseState::Done)) {
        auto segment = consume_next_segment(lexer);
        if (!segment.has_value())
            return {};

        if (state == ParseState::ParsingLanguageOrField)
            state = is_unicode_language_subtag(*segment) ? ParseState::ParsingLanguage : ParseState::ParsingField;

        switch (state) {
        case ParseState::ParsingLanguage:
            lexer.retreat(view_length(*segment));

            if (auto language_id = parse_unicode_language_id_from_lexer(lexer); language_id.has_value()) {
                transformed_extension.language = language_id.release_value();
                state = ParseState::ParsingField;
                break;
            }

            return {};

        case ParseState::ParsingField: {
            // tfield = tkey tvalue;
            TransformedField field { .key = utf16_string_from_ascii_view(*segment) };
            Vector<Utf16String> field_values;

            if (!is_transformed_key(*segment)) {
                lexer.retreat(view_length(*segment) + 1);
                state = ParseState::Done;
                break;
            }

            while (true) {
                auto value = consume_next_segment(lexer);

                if (!value.has_value() || !is_single_transformed_value(*value)) {
                    if (value.has_value())
                        lexer.retreat(view_length(*value) + 1);
                    break;
                }

                field_values.append(utf16_string_from_ascii_view(*value));
            }

            if (field_values.is_empty())
                return {};

            field.value = Utf16String::join("-"sv, field_values);

            transformed_extension.fields.append(move(field));
            break;
        }

        default:
            VERIFY_NOT_REACHED();
        }
    }

    if (!transformed_extension.language.has_value() && transformed_extension.fields.is_empty())
        return {};
    return transformed_extension;
}

template<typename Lexer>
static Optional<OtherExtension> parse_other_extension(u32 key, Lexer& lexer)
{
    // https://unicode.org/reports/tr35/#other_extensions
    //
    // other_extensions = sep [alphanum-[tTuUxX]] (sep alphanum{2,8})+ ;
    Vector<Utf16String> other_values;

    if (!is_ascii_alphanumeric(key) || (key == 'x') || (key == 'X'))
        return {};

    OtherExtension other_extension { .key = static_cast<char>(key) };

    while (true) {
        auto segment = consume_next_segment(lexer);
        if (!segment.has_value())
            break;

        if ((view_length(*segment) < 2) || (view_length(*segment) > 8) || !all_of(*segment, is_ascii_alphanumeric)) {
            lexer.retreat(view_length(*segment) + 1);
            break;
        }

        other_values.append(utf16_string_from_ascii_view(*segment));
    }

    if (other_values.is_empty())
        return {};

    other_extension.value = Utf16String::join("-"sv, other_values);

    return other_extension;
}

template<typename Lexer>
static Optional<Extension> parse_extension(Lexer& lexer)
{
    // https://unicode.org/reports/tr35/#extensions
    //
    // extensions = unicode_locale_extensions | transformed_extensions | other_extensions
    size_t starting_position = lexer.tell();

    if (auto header = consume_next_segment(lexer); header.has_value() && (view_length(*header) == 1)) {
        switch (auto key = view_code_unit_at(*header, 0)) {
        case 'u':
        case 'U':
            if (auto extension = parse_unicode_locale_extension(lexer); extension.has_value())
                return Extension { extension.release_value() };
            break;

        case 't':
        case 'T':
            if (auto extension = parse_transformed_extension(lexer); extension.has_value())
                return Extension { extension.release_value() };
            break;

        default:
            if (auto extension = parse_other_extension(key, lexer); extension.has_value())
                return Extension { extension.release_value() };
            break;
        }
    }

    lexer.retreat(lexer.tell() - starting_position);
    return {};
}

template<typename Lexer>
static Vector<Utf16String> parse_private_use_extensions(Lexer& lexer)
{
    // https://unicode.org/reports/tr35/#pu_extensions
    //
    // pu_extensions = = sep [xX] (sep alphanum{1,8})+ ;
    size_t starting_position = lexer.tell();

    auto header = consume_next_segment(lexer);
    if (!header.has_value())
        return {};

    auto parse_values = [&]() {
        Vector<Utf16String> extensions;

        while (true) {
            auto segment = consume_next_segment(lexer);
            if (!segment.has_value())
                break;

            if ((view_length(*segment) < 1) || (view_length(*segment) > 8) || !all_of(*segment, is_ascii_alphanumeric)) {
                lexer.retreat(view_length(*segment) + 1);
                break;
            }

            extensions.append(utf16_string_from_ascii_view(*segment));
        }

        return extensions;
    };

    if ((view_length(*header) == 1) && ((view_code_unit_at(*header, 0) == 'x') || (view_code_unit_at(*header, 0) == 'X'))) {
        if (auto extensions = parse_values(); !extensions.is_empty())
            return extensions;
    }

    lexer.retreat(lexer.tell() - starting_position);
    return {};
}

template<typename Lexer>
static Optional<LocaleID> parse_unicode_locale_id_from_lexer(Lexer& lexer)
{
    // https://unicode.org/reports/tr35/#Unicode_locale_identifier
    //
    // unicode_locale_id = unicode_language_id
    //                     extensions*
    //                     pu_extensions?
    auto language_id = parse_unicode_language_id_from_lexer(lexer);
    if (!language_id.has_value())
        return {};

    LocaleID locale_id { language_id.release_value() };

    while (true) {
        auto extension = parse_extension(lexer);
        if (!extension.has_value())
            break;
        locale_id.extensions.append(extension.release_value());
    }

    locale_id.private_use_extensions = parse_private_use_extensions(lexer);

    if (!lexer.is_eof())
        return {};

    return locale_id;
}

Optional<LanguageID> parse_unicode_language_id(StringView language)
{
    GenericLexer lexer { language };

    auto language_id = parse_unicode_language_id_from_lexer(lexer);
    if (!lexer.is_eof())
        return {};

    return language_id;
}

Optional<LanguageID> parse_unicode_language_id(Utf16View language)
{
    Utf16GenericLexer lexer { language };

    auto language_id = parse_unicode_language_id_from_lexer(lexer);
    if (!lexer.is_eof())
        return {};

    return language_id;
}

Optional<LocaleID> parse_unicode_locale_id(StringView locale)
{
    GenericLexer lexer { locale };

    return parse_unicode_locale_id_from_lexer(lexer);
}

Optional<LocaleID> parse_unicode_locale_id(Utf16View locale)
{
    Utf16GenericLexer lexer { locale };

    return parse_unicode_locale_id_from_lexer(lexer);
}

Utf16String canonicalize_unicode_locale_id(StringView locale)
{
    return LocaleData::canonicalize(locale);
}

Utf16String canonicalize_unicode_locale_id(Utf16View locale)
{
    return LocaleData::canonicalize(locale.bytes());
}

Utf16String canonicalize_unicode_extension_values(StringView key, Utf16View value)
{
    VERIFY(value.has_ascii_storage());

    UErrorCode status = U_ZERO_ERROR;

    icu::LocaleBuilder builder;
    builder.setUnicodeLocaleKeyword(icu_string_piece(key), icu_string_piece(value.bytes()));

    auto locale = builder.build(status);
    verify_icu_success(status);

    auto result = locale.getUnicodeKeywordValue<StringBuilder>(icu_string_piece(key), status);
    verify_icu_success(status);

    return Utf16String::from_ascii_without_validation(result.string_view().bytes());
}

Utf16View default_locale()
{
    return Utf16View { "en"sv };
}

static void define_locales_without_scripts(HashTable<String>& locales)
{
    // https://tc39.es/ecma402/#sec-internal-slots
    // For locales that include a script subtag in addition to language and region, the corresponding locale without a
    // script subtag must also be supported.

    HashTable<String> new_locales;

    auto append_locale_without_script = [&](auto const& locale) {
        auto parsed_locale = parse_unicode_language_id(locale);
        if (!parsed_locale.has_value())
            return;
        if (!parsed_locale->language.has_value() || !parsed_locale->script.has_value() || !parsed_locale->region.has_value())
            return;

        auto locale_without_script = MUST(String::formatted("{}-{}", *parsed_locale->language, *parsed_locale->region));
        new_locales.set(move(locale_without_script));
    };

    for (auto const& locale : locales)
        append_locale_without_script(locale);

    for (auto const& new_locale : new_locales)
        locales.set(new_locale);
}

bool is_locale_available(StringView locale)
{
    static NeverDestroyed<HashTable<String>> available_locales { []() {
        i32 count = 0;
        auto const* locale_list = icu::Locale::getAvailableLocales(count);

        HashTable<String> available_locales;
        available_locales.ensure_capacity(static_cast<size_t>(count));

        for (i32 i = 0; i < count; ++i) {
            UErrorCode status = U_ZERO_ERROR;

            auto locale_name = locale_list[i].toLanguageTag<StringBuilder>(status);
            if (icu_failure(status))
                continue;

            available_locales.set(MUST(locale_name.to_string()));
        }

        define_locales_without_scripts(available_locales);
        return available_locales;
    }() };

    return available_locales->contains(locale);
}

Style style_from_string(StringView style)
{
    if (style == "narrow"sv)
        return Style::Narrow;
    if (style == "short"sv)
        return Style::Short;
    if (style == "long"sv)
        return Style::Long;
    VERIFY_NOT_REACHED();
}

Style style_from_string(Utf16View style)
{
    if (style == "narrow"sv)
        return Style::Narrow;
    if (style == "short"sv)
        return Style::Short;
    if (style == "long"sv)
        return Style::Long;
    VERIFY_NOT_REACHED();
}

Utf16String style_to_string(Style style)
{
    switch (style) {
    case Style::Narrow:
        return "narrow"_utf16;
    case Style::Short:
        return "short"_utf16;
    case Style::Long:
        return "long"_utf16;
    default:
        VERIFY_NOT_REACHED();
    }
}

static void apply_extensions_to_locale(icu::Locale& locale, icu::Locale const& locale_with_extensions)
{
    UErrorCode status = U_ZERO_ERROR;

    icu::LocaleBuilder builder;
    builder.setLocale(locale_with_extensions);
    builder.setLanguage(locale.getLanguage());
    builder.setRegion(locale.getCountry());
    builder.setScript(locale.getScript());
    builder.setVariant(locale.getVariant());

    locale = builder.build(status);
    verify_icu_success(status);
}

Optional<Utf16String> add_likely_subtags(Utf16View locale)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale.bytes());
    if (!locale_data.has_value())
        return {};

    // ICU doesn't seem to handle maximizing locales that have keywords. For example, "und-x-private" should become
    // "en-Latn-US-x-private" (in the same manner that "und" becomes "en-Latn-US"). So here, we maximize the locale
    // without keywords, then add them back if needed.
    auto maximized = icu::Locale::createFromName(locale_data->locale().getBaseName());

    maximized.addLikelySubtags(status);
    if (icu_failure(status))
        return {};

    if (strlen(locale_data->locale().getName()) != strlen(locale_data->locale().getBaseName()))
        apply_extensions_to_locale(maximized, locale_data->locale());

    auto result = maximized.toLanguageTag<StringBuilder>(status);
    if (icu_failure(status))
        return {};

    return Utf16String::from_ascii_without_validation(result.string_view().bytes());
}

Optional<Utf16String> remove_likely_subtags(Utf16View locale)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale.bytes());
    if (!locale_data.has_value())
        return {};

    // ICU doesn't seem to handle minimizing locales that have keywords. For example, "und-x-private" should become
    // "en-x-private" (in the same manner that "und" becomes "en"). So here, we minimize the locale without keywords,
    // then add them back if needed.
    auto minimized = icu::Locale::createFromName(locale_data->locale().getBaseName());

    minimized.minimizeSubtags(status);
    if (icu_failure(status))
        return {};

    if (strlen(locale_data->locale().getName()) != strlen(locale_data->locale().getBaseName()))
        apply_extensions_to_locale(minimized, locale_data->locale());

    auto result = minimized.toLanguageTag<StringBuilder>(status);
    if (icu_failure(status))
        return {};

    return Utf16String::from_ascii_without_validation(result.string_view().bytes());
}

bool is_locale_character_ordering_right_to_left(Utf16View locale)
{
    auto locale_data = LocaleData::for_locale(locale.bytes());
    if (!locale_data.has_value())
        return false; // Default to left-to-right

    return static_cast<bool>(locale_data->locale().isRightToLeft());
}

static void append_to_builder(StringBuilder& builder, StringView string)
{
    builder.append(string);
}

static void append_to_builder(Utf16StringBuilder& builder, StringView string)
{
    builder.append_ascii(string);
}

static void append_to_builder(StringBuilder& builder, Utf16String const& string)
{
    builder.append(string.utf16_view());
}

static void append_to_builder(Utf16StringBuilder& builder, Utf16String const& string)
{
    builder.append(string.utf16_view());
}

template<typename Builder>
static void append_language_id_to_builder(Builder& builder, LanguageID const& language_id)
{
    auto append_segment = [&](Utf16String const& segment) {
        if (!builder.is_empty())
            append_to_builder(builder, "-"sv);
        append_to_builder(builder, segment);
    };

    auto append_optional_segment = [&](Optional<Utf16String> const& segment) {
        if (!segment.has_value())
            return;
        append_segment(*segment);
    };

    append_optional_segment(language_id.language);
    append_optional_segment(language_id.script);
    append_optional_segment(language_id.region);
    for (auto const& variant : language_id.variants)
        append_segment(variant);
}

String LanguageID::to_string() const
{
    StringBuilder builder;
    append_language_id_to_builder(builder, *this);

    return MUST(builder.to_string());
}

Utf16String LanguageID::to_utf16_string() const
{
    Utf16StringBuilder builder;
    append_language_id_to_builder(builder, *this);

    return builder.to_string();
}

template<typename Builder>
static void append_locale_id_to_builder(Builder& builder, LocaleID const& locale_id)
{
    auto append_segment = [&](auto const& segment) {
        if (segment.is_empty())
            return;
        if (!builder.is_empty())
            append_to_builder(builder, "-"sv);
        append_to_builder(builder, segment);
    };

    append_language_id_to_builder(builder, locale_id.language_id);

    for (auto const& extension : locale_id.extensions) {
        extension.visit(
            [&](LocaleExtension const& ext) {
                append_to_builder(builder, "-u"sv);
                for (auto const& attribute : ext.attributes)
                    append_segment(attribute);
                for (auto const& keyword : ext.keywords) {
                    append_segment(keyword.key);
                    append_segment(keyword.value);
                }
            },
            [&](TransformedExtension const& ext) {
                append_to_builder(builder, "-t"sv);
                if (ext.language.has_value())
                    append_language_id_to_builder(builder, *ext.language);
                for (auto const& field : ext.fields) {
                    append_segment(field.key);
                    append_segment(field.value);
                }
            },
            [&](OtherExtension const& ext) {
                append_to_builder(builder, "-"sv);
                builder.append_code_unit(ext.key);
                append_segment(ext.value);
            });
    }

    if (!locale_id.private_use_extensions.is_empty()) {
        append_to_builder(builder, "-x"sv);
        for (auto const& extension : locale_id.private_use_extensions)
            append_segment(extension);
    }
}

String LocaleID::to_string() const
{
    StringBuilder builder;
    append_locale_id_to_builder(builder, *this);

    return MUST(builder.to_string());
}

Utf16String LocaleID::to_utf16_string() const
{
    Utf16StringBuilder builder;
    append_locale_id_to_builder(builder, *this);

    return builder.to_string();
}

}
