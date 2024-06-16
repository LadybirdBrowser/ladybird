/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/AnyOf.h>
#include <AK/CharacterTypes.h>
#include <AK/Find.h>
#include <AK/Function.h>
#include <AK/QuickSort.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/Locale.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibLocale/Locale.h>

namespace JS::Intl {

// 6.2.2 IsStructurallyValidLanguageTag ( locale ), https://tc39.es/ecma402/#sec-isstructurallyvalidlanguagetag
bool is_structurally_valid_language_tag(StringView locale)
{
    auto contains_duplicate_variant = [&](auto& variants) {
        if (variants.is_empty())
            return false;

        quick_sort(variants);

        for (size_t i = 0; i < variants.size() - 1; ++i) {
            if (variants[i].equals_ignoring_case(variants[i + 1]))
                return true;
        }

        return false;
    };

    // 1. Let lowerLocale be the ASCII-lowercase of locale.
    // NOTE: LibLocale's parsing is case-insensitive.

    // 2. If lowerLocale cannot be matched by the unicode_locale_id Unicode locale nonterminal, return false.
    auto locale_id = ::Locale::parse_unicode_locale_id(locale);
    if (!locale_id.has_value())
        return false;

    // 3. If lowerLocale uses any of the backwards compatibility syntax described in Unicode Technical Standard #35 Part 1 Core,
    //    Section 3.3 BCP 47 Conformance, return false.
    //    https://unicode.org/reports/tr35/#BCP_47_Conformance
    if (locale.contains('_') || locale_id->language_id.is_root || !locale_id->language_id.language.has_value())
        return false;

    // 4. Let languageId be the longest prefix of lowerLocale matched by the unicode_language_id Unicode locale nonterminal.
    auto& language_id = locale_id->language_id;

    // 5. Let variants be GetLocaleVariants(languageId).
    // 6. If variants is not undefined, then
    if (auto& variants = language_id.variants; !variants.is_empty()) {
        // a. If variants contains any duplicate subtags, return false.
        if (contains_duplicate_variant(variants))
            return false;
    }

    HashTable<char> unique_keys;

    // 7. Let allExtensions be the suffix of lowerLocale following languageId.
    // 8. If allExtensions contains a substring matched by the pu_extensions Unicode locale nonterminal, let extensions be
    //    the prefix of allExtensions preceding the longest such substring. Otherwise, let extensions be allExtensions.
    // 9. If extensions is not the empty String, then
    for (auto& extension : locale_id->extensions) {
        char key = extension.visit(
            [](::Locale::LocaleExtension const&) { return 'u'; },
            [](::Locale::TransformedExtension const&) { return 't'; },
            [](::Locale::OtherExtension const& ext) { return static_cast<char>(to_ascii_lowercase(ext.key)); });

        // a. If extensions contains any duplicate singleton subtags, return false.
        if (unique_keys.set(key) != HashSetResult::InsertedNewEntry)
            return false;

        // b. Let transformExtension be the longest substring of extensions matched by the transformed_extensions Unicode
        //    locale nonterminal. If there is no such substring, return true.
        if (auto* transformed = extension.get_pointer<::Locale::TransformedExtension>()) {
            // c. Assert: The substring of transformExtension from 0 to 3 is "-t-".
            // d. Let tPrefix be the substring of transformExtension from 3.

            // e. Let tlang be the longest prefix of tPrefix matched by the tlang Unicode locale nonterminal. If there is
            //    no such prefix, return true.
            auto& transformed_language = transformed->language;
            if (!transformed_language.has_value())
                continue;

            // f. Let tlangRefinements be the longest suffix of tlang following a non-empty prefix matched by the
            //    unicode_language_subtag Unicode locale nonterminal.
            auto& transformed_refinements = transformed_language->variants;

            // g. If tlangRefinements contains any duplicate substrings matched greedily by the unicode_variant_subtag
            //    Unicode locale nonterminal, return false.
            if (contains_duplicate_variant(transformed_refinements))
                return false;
        }
    }

    // 10. Return true.
    return true;
}

// 6.2.3 CanonicalizeUnicodeLocaleId ( locale ), https://tc39.es/ecma402/#sec-canonicalizeunicodelocaleid
String canonicalize_unicode_locale_id(StringView locale)
{
    return ::Locale::canonicalize_unicode_locale_id(locale);
}

// 6.3.1 IsWellFormedCurrencyCode ( currency ), https://tc39.es/ecma402/#sec-iswellformedcurrencycode
bool is_well_formed_currency_code(StringView currency)
{
    // 1. If the length of currency is not 3, return false.
    if (currency.length() != 3)
        return false;

    // 2. Let normalized be the ASCII-uppercase of currency.
    // 3. If normalized contains any code unit outside of 0x0041 through 0x005A (corresponding to Unicode characters LATIN CAPITAL LETTER A through LATIN CAPITAL LETTER Z), return false.
    if (!all_of(currency, is_ascii_alpha))
        return false;

    // 4. Return true.
    return true;
}

// 6.5.1 IsWellFormedUnitIdentifier ( unitIdentifier ), https://tc39.es/ecma402/#sec-iswellformedunitidentifier
bool is_well_formed_unit_identifier(StringView unit_identifier)
{
    // 6.5.2 IsSanctionedSingleUnitIdentifier ( unitIdentifier ), https://tc39.es/ecma402/#sec-issanctionedsingleunitidentifier
    constexpr auto is_sanctioned_single_unit_identifier = [](StringView unit_identifier) {
        // 1. If unitIdentifier is listed in Table 2 below, return true.
        // 2. Else, return false.
        static constexpr auto sanctioned_units = sanctioned_single_unit_identifiers();
        return find(sanctioned_units.begin(), sanctioned_units.end(), unit_identifier) != sanctioned_units.end();
    };

    // 1. If ! IsSanctionedSingleUnitIdentifier(unitIdentifier) is true, then
    if (is_sanctioned_single_unit_identifier(unit_identifier)) {
        // a. Return true.
        return true;
    }

    // 2. Let i be StringIndexOf(unitIdentifier, "-per-", 0).
    auto indices = unit_identifier.find_all("-per-"sv);

    // 3. If i is -1 or StringIndexOf(unitIdentifier, "-per-", i + 1) is not -1, then
    if (indices.size() != 1) {
        // a. Return false.
        return false;
    }

    // 4. Assert: The five-character substring "-per-" occurs exactly once in unitIdentifier, at index i.
    // NOTE: We skip this because the indices vector being of size 1 already verifies this invariant.

    // 5. Let numerator be the substring of unitIdentifier from 0 to i.
    auto numerator = unit_identifier.substring_view(0, indices[0]);

    // 6. Let denominator be the substring of unitIdentifier from i + 5.
    auto denominator = unit_identifier.substring_view(indices[0] + 5);

    // 7. If ! IsSanctionedSingleUnitIdentifier(numerator) and ! IsSanctionedSingleUnitIdentifier(denominator) are both true, then
    if (is_sanctioned_single_unit_identifier(numerator) && is_sanctioned_single_unit_identifier(denominator)) {
        // a. Return true.
        return true;
    }

    // 8. Return false.
    return false;
}

// 9.2.1 CanonicalizeLocaleList ( locales ), https://tc39.es/ecma402/#sec-canonicalizelocalelist
ThrowCompletionOr<Vector<String>> canonicalize_locale_list(VM& vm, Value locales)
{
    auto& realm = *vm.current_realm();

    // 1. If locales is undefined, then
    if (locales.is_undefined()) {
        // a. Return a new empty List.
        return Vector<String> {};
    }

    // 2. Let seen be a new empty List.
    Vector<String> seen;

    Object* object = nullptr;
    // 3. If Type(locales) is String or Type(locales) is Object and locales has an [[InitializedLocale]] internal slot, then
    if (locales.is_string() || (locales.is_object() && is<Locale>(locales.as_object()))) {
        // a. Let O be CreateArrayFromList(« locales »).
        object = Array::create_from(realm, { locales });
    }
    // 4. Else,
    else {
        // a. Let O be ? ToObject(locales).
        object = TRY(locales.to_object(vm));
    }

    // 5. Let len be ? ToLength(? Get(O, "length")).
    auto length_value = TRY(object->get(vm.names.length));
    auto length = TRY(length_value.to_length(vm));

    // 6. Let k be 0.
    // 7. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ToString(k).
        auto property_key = PropertyKey { k };

        // b. Let kPresent be ? HasProperty(O, Pk).
        auto key_present = TRY(object->has_property(property_key));

        // c. If kPresent is true, then
        if (key_present) {
            // i. Let kValue be ? Get(O, Pk).
            auto key_value = TRY(object->get(property_key));

            // ii. If Type(kValue) is not String or Object, throw a TypeError exception.
            if (!key_value.is_string() && !key_value.is_object())
                return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOrString, key_value);

            String tag;

            // iii. If Type(kValue) is Object and kValue has an [[InitializedLocale]] internal slot, then
            if (key_value.is_object() && is<Locale>(key_value.as_object())) {
                // 1. Let tag be kValue.[[Locale]].
                tag = static_cast<Locale const&>(key_value.as_object()).locale();
            }
            // iv. Else,
            else {
                // 1. Let tag be ? ToString(kValue).
                tag = TRY(key_value.to_string(vm));
            }

            // v. If ! IsStructurallyValidLanguageTag(tag) is false, throw a RangeError exception.
            if (!is_structurally_valid_language_tag(tag))
                return vm.throw_completion<RangeError>(ErrorType::IntlInvalidLanguageTag, tag);

            // vi. Let canonicalizedTag be ! CanonicalizeUnicodeLocaleId(tag).
            auto canonicalized_tag = canonicalize_unicode_locale_id(tag);

            // vii. If canonicalizedTag is not an element of seen, append canonicalizedTag as the last element of seen.
            if (!seen.contains_slow(canonicalized_tag))
                seen.append(move(canonicalized_tag));
        }

        // d. Increase k by 1.
    }

    return seen;
}

// 9.2.2 BestAvailableLocale ( availableLocales, locale ), https://tc39.es/ecma402/#sec-bestavailablelocale
Optional<StringView> best_available_locale(StringView locale)
{
    // 1. Let candidate be locale.
    StringView candidate = locale;

    // 2. Repeat,
    while (true) {
        // a. If availableLocales contains candidate, return candidate.
        if (::Locale::is_locale_available(candidate))
            return candidate;

        // b. Let pos be the character index of the last occurrence of "-" (U+002D) within candidate. If that character does not occur, return undefined.
        auto pos = candidate.find_last('-');
        if (!pos.has_value())
            return {};

        // c. If pos ≥ 2 and the character "-" occurs at index pos-2 of candidate, decrease pos by 2.
        if ((*pos >= 2) && (candidate[*pos - 2] == '-'))
            pos = *pos - 2;

        // d. Let candidate be the substring of candidate from position 0, inclusive, to position pos, exclusive.
        candidate = candidate.substring_view(0, *pos);
    }
}

struct MatcherResult {
    String locale;
    Vector<::Locale::Extension> extensions {};
};

// 9.2.3 LookupMatcher ( availableLocales, requestedLocales ), https://tc39.es/ecma402/#sec-lookupmatcher
static MatcherResult lookup_matcher(Vector<String> const& requested_locales)
{
    // 1. Let result be a new Record.
    MatcherResult result {};

    // 2. For each element locale of requestedLocales, do
    for (auto const& locale : requested_locales) {
        auto locale_id = ::Locale::parse_unicode_locale_id(locale);
        VERIFY(locale_id.has_value());

        // a. Let noExtensionsLocale be the String value that is locale with any Unicode locale extension sequences removed.
        auto extensions = locale_id->remove_extension_type<::Locale::LocaleExtension>();
        auto no_extensions_locale = locale_id->to_string();

        // b. Let availableLocale be ! BestAvailableLocale(availableLocales, noExtensionsLocale).
        auto available_locale = best_available_locale(no_extensions_locale);

        // c. If availableLocale is not undefined, then
        if (available_locale.has_value()) {
            // i. Set result.[[locale]] to availableLocale.
            result.locale = MUST(String::from_utf8(*available_locale));

            // ii. If locale and noExtensionsLocale are not the same String value, then
            if (locale != no_extensions_locale) {
                // 1. Let extension be the String value consisting of the substring of the Unicode locale extension sequence within locale.
                // 2. Set result.[[extension]] to extension.
                result.extensions.extend(move(extensions));
            }

            // iii. Return result.
            return result;
        }
    }

    // 3. Let defLocale be ! DefaultLocale().
    // 4. Set result.[[locale]] to defLocale.
    result.locale = MUST(String::from_utf8(::Locale::default_locale()));

    // 5. Return result.
    return result;
}

// 9.2.4 BestFitMatcher ( availableLocales, requestedLocales ), https://tc39.es/ecma402/#sec-bestfitmatcher
static MatcherResult best_fit_matcher(Vector<String> const& requested_locales)
{
    // The algorithm is implementation dependent, but should produce results that a typical user of the requested locales would
    // perceive as at least as good as those produced by the LookupMatcher abstract operation.
    return lookup_matcher(requested_locales);
}

// 9.2.6 InsertUnicodeExtensionAndCanonicalize ( locale, extension ), https://tc39.es/ecma402/#sec-insert-unicode-extension-and-canonicalize
String insert_unicode_extension_and_canonicalize(::Locale::LocaleID locale, ::Locale::LocaleExtension extension)
{
    // Note: This implementation differs from the spec in how the extension is inserted. The spec assumes
    // the input to this method is a string, and is written such that operations are performed on parts
    // of that string. LibUnicode gives us the parsed locale in a structure, so we can mutate that
    // structure directly.
    locale.extensions.append(move(extension));

    return JS::Intl::canonicalize_unicode_locale_id(locale.to_string());
}

template<typename T>
static auto& find_key_in_value(T& value, StringView key)
{
    if (key == "ca"sv)
        return value.ca;
    if (key == "co"sv)
        return value.co;
    if (key == "hc"sv)
        return value.hc;
    if (key == "kf"sv)
        return value.kf;
    if (key == "kn"sv)
        return value.kn;
    if (key == "nu"sv)
        return value.nu;

    // If you hit this point, you must add any missing keys from [[RelevantExtensionKeys]] to LocaleOptions and LocaleResult.
    VERIFY_NOT_REACHED();
}

// 9.2.7 ResolveLocale ( availableLocales, requestedLocales, options, relevantExtensionKeys, localeData ), https://tc39.es/ecma402/#sec-resolvelocale
LocaleResult resolve_locale(Vector<String> const& requested_locales, LocaleOptions const& options, ReadonlySpan<StringView> relevant_extension_keys)
{
    // 1. Let matcher be options.[[localeMatcher]].
    auto const& matcher = options.locale_matcher;
    MatcherResult matcher_result;

    // 2. If matcher is "lookup", then
    if (matcher.is_string() && (matcher.as_string().utf8_string_view()) == "lookup"sv) {
        // a. Let r be ! LookupMatcher(availableLocales, requestedLocales).
        matcher_result = lookup_matcher(requested_locales);
    }
    // 3. Else,
    else {
        // a. Let r be ! BestFitMatcher(availableLocales, requestedLocales).
        matcher_result = best_fit_matcher(requested_locales);
    }

    // 4. Let foundLocale be r.[[locale]].
    auto found_locale = move(matcher_result.locale);

    // 5. Let result be a new Record.
    LocaleResult result {};

    // 6. Set result.[[dataLocale]] to foundLocale.
    result.data_locale = found_locale;

    // 7. If r has an [[extension]] field, then
    Vector<::Locale::Keyword> keywords;
    for (auto& extension : matcher_result.extensions) {
        if (!extension.has<::Locale::LocaleExtension>())
            continue;

        // a. Let components be ! UnicodeExtensionComponents(r.[[extension]]).
        auto& components = extension.get<::Locale::LocaleExtension>();
        // b. Let keywords be components.[[Keywords]].
        keywords = move(components.keywords);

        break;
    }

    // 8. Let supportedExtension be "-u".
    ::Locale::LocaleExtension supported_extension {};

    // 9. For each element key of relevantExtensionKeys, do
    for (auto const& key : relevant_extension_keys) {
        // a. Let foundLocaleData be localeData.[[<foundLocale>]].
        // b. Assert: Type(foundLocaleData) is Record.
        // c. Let keyLocaleData be foundLocaleData.[[<key>]].
        // d. Assert: Type(keyLocaleData) is List.
        auto key_locale_data = ::Locale::get_available_keyword_values(key);

        // e. Let value be keyLocaleData[0].
        // f. Assert: Type(value) is either String or Null.
        // NOTE: ECMA-402 assumes keyLocaleData is sorted by locale preference. Our list is sorted
        //       alphabetically, so we get the locale's preferred value from LibUnicode.
        Optional<String> value;
        if (auto preference = ::Locale::get_preferred_keyword_value_for_locale(found_locale, key); preference.has_value())
            value = MUST(String::from_utf8(*preference));

        // g. Let supportedExtensionAddition be "".
        Optional<::Locale::Keyword> supported_extension_addition {};

        // h. If r has an [[extension]] field, then
        for (auto& entry : keywords) {
            // i. If keywords contains an element whose [[Key]] is the same as key, then
            if (entry.key != key)
                continue;

            // 1. Let entry be the element of keywords whose [[Key]] is the same as key.
            // 2. Let requestedValue be entry.[[Value]].
            auto requested_value = entry.value;

            // 3. If requestedValue is not the empty String, then
            if (!requested_value.is_empty()) {
                // a. If keyLocaleData contains requestedValue, then
                if (key_locale_data.contains_slow(requested_value)) {
                    // i. Let value be requestedValue.
                    value = move(requested_value);

                    // ii. Let supportedExtensionAddition be the string-concatenation of "-", key, "-", and value.
                    supported_extension_addition = ::Locale::Keyword { MUST(String::from_utf8(key)), move(entry.value) };
                }
            }
            // 4. Else if keyLocaleData contains "true", then
            else if (key_locale_data.contains_slow("true"sv)) {
                // a. Let value be "true".
                value = "true"_string;

                // b. Let supportedExtensionAddition be the string-concatenation of "-" and key.
                supported_extension_addition = ::Locale::Keyword { MUST(String::from_utf8(key)), {} };
            }

            break;
        }

        // i. If options has a field [[<key>]], then
        // i. Let optionsValue be options.[[<key>]].
        // ii. Assert: Type(optionsValue) is either String, Undefined, or Null.
        auto options_value = find_key_in_value(options, key);

        // iii. If Type(optionsValue) is String, then
        if (options_value.has_value()) {
            // 1. Let optionsValue be the string optionsValue after performing the algorithm steps to transform Unicode extension values to canonical syntax per Unicode Technical Standard #35 LDML § 3.2.1 Canonical Unicode Locale Identifiers, treating key as ukey and optionsValue as uvalue productions.
            // 2. Let optionsValue be the string optionsValue after performing the algorithm steps to replace Unicode extension values with their canonical form per Unicode Technical Standard #35 LDML § 3.2.1 Canonical Unicode Locale Identifiers, treating key as ukey and optionsValue as uvalue productions.
            ::Locale::canonicalize_unicode_extension_values(key, *options_value);

            // 3. If optionsValue is the empty String, then
            if (options_value->is_empty()) {
                // a. Let optionsValue be "true".
                options_value = "true"_string;
            }
        }

        // iv. If SameValue(optionsValue, value) is false and keyLocaleData contains optionsValue, then
        if (options_value.has_value() && (options_value != value) && key_locale_data.contains_slow(*options_value)) {
            // 1. Let value be optionsValue.
            value = move(options_value);

            // 2. Let supportedExtensionAddition be "".
            supported_extension_addition.clear();
        }

        // j. Set result.[[<key>]] to value.
        find_key_in_value(result, key) = move(value);

        // k. Set supportedExtension to the string-concatenation of supportedExtension and supportedExtensionAddition.
        if (supported_extension_addition.has_value())
            supported_extension.keywords.append(supported_extension_addition.release_value());
    }

    // 10. If supportedExtension is not "-u", then
    if (!supported_extension.keywords.is_empty()) {
        auto locale_id = ::Locale::parse_unicode_locale_id(found_locale);
        VERIFY(locale_id.has_value());

        // a. Set foundLocale to InsertUnicodeExtensionAndCanonicalize(foundLocale, supportedExtension).
        found_locale = insert_unicode_extension_and_canonicalize(locale_id.release_value(), move(supported_extension));
    }

    // 11. Set result.[[locale]] to foundLocale.
    result.locale = move(found_locale);

    // 12. Return result.
    return result;
}

// 9.2.8 LookupSupportedLocales ( availableLocales, requestedLocales ), https://tc39.es/ecma402/#sec-lookupsupportedlocales
static Vector<String> lookup_supported_locales(Vector<String> const& requested_locales)
{
    // 1. Let subset be a new empty List.
    Vector<String> subset;

    // 2. For each element locale of requestedLocales, do
    for (auto const& locale : requested_locales) {
        auto locale_id = ::Locale::parse_unicode_locale_id(locale);
        VERIFY(locale_id.has_value());

        // a. Let noExtensionsLocale be the String value that is locale with any Unicode locale extension sequences removed.
        locale_id->remove_extension_type<::Locale::LocaleExtension>();
        auto no_extensions_locale = locale_id->to_string();

        // b. Let availableLocale be ! BestAvailableLocale(availableLocales, noExtensionsLocale).
        auto available_locale = best_available_locale(no_extensions_locale);

        // c. If availableLocale is not undefined, append locale to the end of subset.
        if (available_locale.has_value())
            subset.append(locale);
    }

    // 3. Return subset.
    return subset;
}

// 9.2.9 BestFitSupportedLocales ( availableLocales, requestedLocales ), https://tc39.es/ecma402/#sec-bestfitsupportedlocales
static Vector<String> best_fit_supported_locales(Vector<String> const& requested_locales)
{
    // The BestFitSupportedLocales abstract operation returns the subset of the provided BCP 47
    // language priority list requestedLocales for which availableLocales has a matching locale
    // when using the Best Fit Matcher algorithm. Locales appear in the same order in the returned
    // list as in requestedLocales. The steps taken are implementation dependent.

    // :yakbrain:
    return lookup_supported_locales(requested_locales);
}

// 9.2.10 SupportedLocales ( availableLocales, requestedLocales, options ), https://tc39.es/ecma402/#sec-supportedlocales
ThrowCompletionOr<Array*> supported_locales(VM& vm, Vector<String> const& requested_locales, Value options)
{
    auto& realm = *vm.current_realm();

    // 1. Set options to ? CoerceOptionsToObject(options).
    auto* options_object = TRY(coerce_options_to_object(vm, options));

    // 2. Let matcher be ? GetOption(options, "localeMatcher", string, « "lookup", "best fit" », "best fit").
    auto matcher = TRY(get_option(vm, *options_object, vm.names.localeMatcher, OptionType::String, { "lookup"sv, "best fit"sv }, "best fit"sv));

    Vector<String> supported_locales;

    // 3. If matcher is "best fit", then
    if (matcher.as_string().utf8_string_view() == "best fit"sv) {
        // a. Let supportedLocales be BestFitSupportedLocales(availableLocales, requestedLocales).
        supported_locales = best_fit_supported_locales(requested_locales);
    }
    // 4. Else,
    else {
        // a. Let supportedLocales be LookupSupportedLocales(availableLocales, requestedLocales).
        supported_locales = lookup_supported_locales(requested_locales);
    }

    // 5. Return CreateArrayFromList(supportedLocales).
    return Array::create_from<String>(realm, supported_locales, [&vm](auto& locale) { return PrimitiveString::create(vm, move(locale)); }).ptr();
}

// 9.2.12 CoerceOptionsToObject ( options ), https://tc39.es/ecma402/#sec-coerceoptionstoobject
ThrowCompletionOr<Object*> coerce_options_to_object(VM& vm, Value options)
{
    auto& realm = *vm.current_realm();

    // 1. If options is undefined, then
    if (options.is_undefined()) {
        // a. Return OrdinaryObjectCreate(null).
        return Object::create(realm, nullptr).ptr();
    }

    // 2. Return ? ToObject(options).
    return TRY(options.to_object(vm)).ptr();
}

// NOTE: 9.2.13 GetOption has been removed and is being pulled in from ECMA-262 in the Temporal proposal.

// 9.2.14 GetBooleanOrStringNumberFormatOption ( options, property, stringValues, fallback ), https://tc39.es/ecma402/#sec-getbooleanorstringnumberformatoption
ThrowCompletionOr<StringOrBoolean> get_boolean_or_string_number_format_option(VM& vm, Object const& options, PropertyKey const& property, ReadonlySpan<StringView> string_values, StringOrBoolean fallback)
{
    // 1. Let value be ? Get(options, property).
    auto value = TRY(options.get(property));

    // 2. If value is undefined, return fallback.
    if (value.is_undefined())
        return fallback;

    // 3. If value is true, return true.
    if (value.is_boolean() && value.as_bool())
        return StringOrBoolean { true };

    // 4. If ToBoolean(value) is false, return false.
    if (!value.to_boolean())
        return StringOrBoolean { false };

    // 5. Let value be ? ToString(value).
    auto value_string = TRY(value.to_string(vm));

    // 6. If stringValues does not contain value, throw a RangeError exception.
    auto it = find(string_values.begin(), string_values.end(), value_string.bytes_as_string_view());
    if (it == string_values.end())
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, value_string, property.as_string());

    // 7. Return value.
    return StringOrBoolean { *it };
}

// 9.2.15 DefaultNumberOption ( value, minimum, maximum, fallback ), https://tc39.es/ecma402/#sec-defaultnumberoption
ThrowCompletionOr<Optional<int>> default_number_option(VM& vm, Value value, int minimum, int maximum, Optional<int> fallback)
{
    // 1. If value is undefined, return fallback.
    if (value.is_undefined())
        return fallback;

    // 2. Set value to ? ToNumber(value).
    value = TRY(value.to_number(vm));

    // 3. If value is NaN or less than minimum or greater than maximum, throw a RangeError exception.
    if (value.is_nan() || (value.as_double() < minimum) || (value.as_double() > maximum))
        return vm.throw_completion<RangeError>(ErrorType::IntlNumberIsNaNOrOutOfRange, value, minimum, maximum);

    // 4. Return floor(value).
    return floor(value.as_double());
}

// 9.2.16 GetNumberOption ( options, property, minimum, maximum, fallback ), https://tc39.es/ecma402/#sec-getnumberoption
ThrowCompletionOr<Optional<int>> get_number_option(VM& vm, Object const& options, PropertyKey const& property, int minimum, int maximum, Optional<int> fallback)
{
    // 1. Assert: Type(options) is Object.

    // 2. Let value be ? Get(options, property).
    auto value = TRY(options.get(property));

    // 3. Return ? DefaultNumberOption(value, minimum, maximum, fallback).
    return default_number_option(vm, value, minimum, maximum, move(fallback));
}

// 9.2.17 PartitionPattern ( pattern ), https://tc39.es/ecma402/#sec-partitionpattern
Vector<PatternPartition> partition_pattern(StringView pattern)
{
    // 1. Let result be a new empty List.
    Vector<PatternPartition> result;

    // 2. Let beginIndex be StringIndexOf(pattern, "{", 0).
    auto begin_index = pattern.find('{', 0);

    // 3. Let endIndex be 0.
    size_t end_index = 0;

    // 4. Let nextIndex be 0.
    size_t next_index = 0;

    // 5. Let length be the number of code units in pattern.
    // 6. Repeat, while beginIndex is an integer index into pattern,
    while (begin_index.has_value()) {
        // a. Set endIndex to StringIndexOf(pattern, "}", beginIndex).
        end_index = pattern.find('}', *begin_index).value();

        // b. Assert: endIndex is greater than beginIndex.
        VERIFY(end_index > *begin_index);

        // c. If beginIndex is greater than nextIndex, then
        if (*begin_index > next_index) {
            // i. Let literal be a substring of pattern from position nextIndex, inclusive, to position beginIndex, exclusive.
            auto literal = pattern.substring_view(next_index, *begin_index - next_index);

            // ii. Append a new Record { [[Type]]: "literal", [[Value]]: literal } as the last element of the list result.
            result.append({ "literal"sv, MUST(String::from_utf8(literal)) });
        }

        // d. Let p be the substring of pattern from position beginIndex, exclusive, to position endIndex, exclusive.
        auto partition = pattern.substring_view(*begin_index + 1, end_index - *begin_index - 1);

        // e. Append a new Record { [[Type]]: p, [[Value]]: undefined } as the last element of the list result.
        result.append({ partition, {} });

        // f. Set nextIndex to endIndex + 1.
        next_index = end_index + 1;

        // g. Set beginIndex to StringIndexOf(pattern, "{", nextIndex).
        begin_index = pattern.find('{', next_index);
    }

    // 7. If nextIndex is less than length, then
    if (next_index < pattern.length()) {
        // a. Let literal be the substring of pattern from position nextIndex, inclusive, to position length, exclusive.
        auto literal = pattern.substring_view(next_index);

        // b. Append a new Record { [[Type]]: "literal", [[Value]]: literal } as the last element of the list result.
        result.append({ "literal"sv, MUST(String::from_utf8(literal)) });
    }

    // 8. Return result.
    return result;
}

}
