/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/GenericLexer.h>
#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/Locale.h>

#include <unicode/localebuilder.h>
#include <unicode/locid.h>

namespace Unicode {

static bool is_key(StringView key)
{
    // key = alphanum alpha
    if (key.length() != 2)
        return false;
    return is_ascii_alphanumeric(key[0]) && is_ascii_alpha(key[1]);
}

static bool is_single_type(StringView type)
{
    // type = alphanum{3,8} (sep alphanum{3,8})*
    // Note: Consecutive types are not handled here, that is left to the caller.
    if ((type.length() < 3) || (type.length() > 8))
        return false;
    return all_of(type, is_ascii_alphanumeric);
}

static bool is_attribute(StringView type)
{
    // attribute = alphanum{3,8}
    if ((type.length() < 3) || (type.length() > 8))
        return false;
    return all_of(type, is_ascii_alphanumeric);
}

static bool is_transformed_key(StringView key)
{
    // tkey = alpha digit
    if (key.length() != 2)
        return false;
    return is_ascii_alpha(key[0]) && is_ascii_digit(key[1]);
}

static bool is_single_transformed_value(StringView value)
{
    // tvalue = (sep alphanum{3,8})+
    // Note: Consecutive values are not handled here, that is left to the caller.
    if ((value.length() < 3) || (value.length() > 8))
        return false;
    return all_of(value, is_ascii_alphanumeric);
}

static Optional<StringView> consume_next_segment(GenericLexer& lexer, bool with_separator = true)
{
    constexpr auto is_separator = is_any_of("-_"sv);

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

static Optional<LanguageID> parse_unicode_language_id(GenericLexer& lexer)
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
                language_id.language = MUST(String::from_utf8(*segment));
            } else if (is_unicode_script_subtag(*segment)) {
                state = ParseState::ParsingRegion;
                language_id.script = MUST(String::from_utf8(*segment));
            } else {
                return {};
            }
            break;

        case ParseState::ParsingScript:
            if (is_unicode_script_subtag(*segment)) {
                state = ParseState::ParsingRegion;
                language_id.script = MUST(String::from_utf8(*segment));
                break;
            }

            state = ParseState::ParsingRegion;
            [[fallthrough]];

        case ParseState::ParsingRegion:
            if (is_unicode_region_subtag(*segment)) {
                state = ParseState::ParsingVariant;
                language_id.region = MUST(String::from_utf8(*segment));
                break;
            }

            state = ParseState::ParsingVariant;
            [[fallthrough]];

        case ParseState::ParsingVariant:
            if (is_unicode_variant_subtag(*segment)) {
                language_id.variants.append(MUST(String::from_utf8(*segment)));
            } else {
                lexer.retreat(segment->length() + 1);
                state = ParseState::Done;
            }
            break;

        default:
            VERIFY_NOT_REACHED();
        }
    }

    return language_id;
}

static Optional<LocaleExtension> parse_unicode_locale_extension(GenericLexer& lexer)
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
                locale_extension.attributes.append(MUST(String::from_utf8(*segment)));
                break;
            }

            state = ParseState::ParsingKeyword;
            [[fallthrough]];

        case ParseState::ParsingKeyword: {
            // keyword = key (sep type)?
            Keyword keyword { .key = MUST(String::from_utf8(*segment)) };
            Vector<StringView> keyword_values;

            if (!is_key(*segment)) {
                lexer.retreat(segment->length() + 1);
                state = ParseState::Done;
                break;
            }

            while (true) {
                auto type = consume_next_segment(lexer);

                if (!type.has_value() || !is_single_type(*type)) {
                    if (type.has_value())
                        lexer.retreat(type->length() + 1);
                    break;
                }

                keyword_values.append(*type);
            }

            StringBuilder builder;
            builder.join('-', keyword_values);
            keyword.value = MUST(builder.to_string());

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

static Optional<TransformedExtension> parse_transformed_extension(GenericLexer& lexer)
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
            lexer.retreat(segment->length());

            if (auto language_id = parse_unicode_language_id(lexer); language_id.has_value()) {
                transformed_extension.language = language_id.release_value();
                state = ParseState::ParsingField;
                break;
            }

            return {};

        case ParseState::ParsingField: {
            // tfield = tkey tvalue;
            TransformedField field { .key = MUST(String::from_utf8(*segment)) };
            Vector<StringView> field_values;

            if (!is_transformed_key(*segment)) {
                lexer.retreat(segment->length() + 1);
                state = ParseState::Done;
                break;
            }

            while (true) {
                auto value = consume_next_segment(lexer);

                if (!value.has_value() || !is_single_transformed_value(*value)) {
                    if (value.has_value())
                        lexer.retreat(value->length() + 1);
                    break;
                }

                field_values.append(*value);
            }

            if (field_values.is_empty())
                return {};

            StringBuilder builder;
            builder.join('-', field_values);
            field.value = MUST(builder.to_string());

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

static Optional<OtherExtension> parse_other_extension(char key, GenericLexer& lexer)
{
    // https://unicode.org/reports/tr35/#other_extensions
    //
    // other_extensions = sep [alphanum-[tTuUxX]] (sep alphanum{2,8})+ ;
    OtherExtension other_extension { .key = key };
    Vector<StringView> other_values;

    if (!is_ascii_alphanumeric(key) || (key == 'x') || (key == 'X'))
        return {};

    while (true) {
        auto segment = consume_next_segment(lexer);
        if (!segment.has_value())
            break;

        if ((segment->length() < 2) || (segment->length() > 8) || !all_of(*segment, is_ascii_alphanumeric)) {
            lexer.retreat(segment->length() + 1);
            break;
        }

        other_values.append(*segment);
    }

    if (other_values.is_empty())
        return {};

    StringBuilder builder;
    builder.join('-', other_values);
    other_extension.value = MUST(builder.to_string());

    return other_extension;
}

static Optional<Extension> parse_extension(GenericLexer& lexer)
{
    // https://unicode.org/reports/tr35/#extensions
    //
    // extensions = unicode_locale_extensions | transformed_extensions | other_extensions
    size_t starting_position = lexer.tell();

    if (auto header = consume_next_segment(lexer); header.has_value() && (header->length() == 1)) {
        switch (char key = (*header)[0]) {
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

static Vector<String> parse_private_use_extensions(GenericLexer& lexer)
{
    // https://unicode.org/reports/tr35/#pu_extensions
    //
    // pu_extensions = = sep [xX] (sep alphanum{1,8})+ ;
    size_t starting_position = lexer.tell();

    auto header = consume_next_segment(lexer);
    if (!header.has_value())
        return {};

    auto parse_values = [&]() {
        Vector<String> extensions;

        while (true) {
            auto segment = consume_next_segment(lexer);
            if (!segment.has_value())
                break;

            if ((segment->length() < 1) || (segment->length() > 8) || !all_of(*segment, is_ascii_alphanumeric)) {
                lexer.retreat(segment->length() + 1);
                break;
            }

            extensions.append(MUST(String::from_utf8(*segment)));
        }

        return extensions;
    };

    if ((header->length() == 1) && (((*header)[0] == 'x') || ((*header)[0] == 'X'))) {
        if (auto extensions = parse_values(); !extensions.is_empty())
            return extensions;
    }

    lexer.retreat(lexer.tell() - starting_position);
    return {};
}

Optional<LanguageID> parse_unicode_language_id(StringView language)
{
    GenericLexer lexer { language };

    auto language_id = parse_unicode_language_id(lexer);
    if (!lexer.is_eof())
        return {};

    return language_id;
}

Optional<LocaleID> parse_unicode_locale_id(StringView locale)
{
    GenericLexer lexer { locale };

    // https://unicode.org/reports/tr35/#Unicode_locale_identifier
    //
    // unicode_locale_id = unicode_language_id
    //                     extensions*
    //                     pu_extensions?
    auto language_id = parse_unicode_language_id(lexer);
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

String canonicalize_unicode_locale_id(StringView locale)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    VERIFY(locale_data.has_value());

    locale_data->locale().canonicalize(status);
    VERIFY(icu_success(status));

    return locale_data->to_string();
}

String canonicalize_unicode_extension_values(StringView key, StringView value)
{
    UErrorCode status = U_ZERO_ERROR;

    icu::LocaleBuilder builder;
    builder.setUnicodeLocaleKeyword(icu_string_piece(key), icu_string_piece(value));

    auto locale = builder.build(status);
    VERIFY(icu_success(status));

    locale.canonicalize(status);
    VERIFY(icu_success(status));

    auto result = locale.getUnicodeKeywordValue<StringBuilder>(icu_string_piece(key), status);
    VERIFY(icu_success(status));

    return MUST(result.to_string());
}

StringView default_locale()
{
    return "en"sv;
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
    static auto available_locales = []() {
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
    }();

    return available_locales.contains(locale);
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

StringView style_to_string(Style style)
{
    switch (style) {
    case Style::Narrow:
        return "narrow"sv;
    case Style::Short:
        return "short"sv;
    case Style::Long:
        return "long"sv;
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
    VERIFY(icu_success(status));
}

Optional<String> add_likely_subtags(StringView locale)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
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

    return MUST(result.to_string());
}

Optional<String> remove_likely_subtags(StringView locale)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
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

    return MUST(result.to_string());
}

bool is_locale_character_ordering_right_to_left(StringView locale)
{
    auto locale_data = LocaleData::for_locale(locale);
    if (!locale_data.has_value())
        return false; // Default to left-to-right

    return static_cast<bool>(locale_data->locale().isRightToLeft());
}

String LanguageID::to_string() const
{
    StringBuilder builder;

    auto append_segment = [&](Optional<String> const& segment) {
        if (!segment.has_value())
            return;
        if (!builder.is_empty())
            builder.append('-');
        builder.append(*segment);
    };

    append_segment(language);
    append_segment(script);
    append_segment(region);
    for (auto const& variant : variants)
        append_segment(variant);

    return MUST(builder.to_string());
}

String LocaleID::to_string() const
{
    StringBuilder builder;

    auto append_segment = [&](auto const& segment) {
        if (segment.is_empty())
            return;
        if (!builder.is_empty())
            builder.append('-');
        builder.append(segment);
    };

    append_segment(language_id.to_string());

    for (auto const& extension : extensions) {
        extension.visit(
            [&](LocaleExtension const& ext) {
                builder.append("-u"sv);
                for (auto const& attribute : ext.attributes)
                    append_segment(attribute);
                for (auto const& keyword : ext.keywords) {
                    append_segment(keyword.key);
                    append_segment(keyword.value);
                }
            },
            [&](TransformedExtension const& ext) {
                builder.append("-t"sv);
                if (ext.language.has_value())
                    append_segment(ext.language->to_string());
                for (auto const& field : ext.fields) {
                    append_segment(field.key);
                    append_segment(field.value);
                }
            },
            [&](OtherExtension const& ext) {
                builder.appendff("-{}", ext.key);
                append_segment(ext.value);
            });
    }

    if (!private_use_extensions.is_empty()) {
        builder.append("-x"sv);
        for (auto const& extension : private_use_extensions)
            append_segment(extension);
    }

    return MUST(builder.to_string());
}

}
