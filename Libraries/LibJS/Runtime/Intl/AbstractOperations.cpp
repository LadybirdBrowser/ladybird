/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/CharacterTypes.h>
#include <AK/Find.h>
#include <AK/QuickSort.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/Locale.h>
#include <LibJS/Runtime/Intl/SingleUnitIdentifiers.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibUnicode/TimeZone.h>
#include <LibUnicode/UnicodeKeywords.h>

namespace JS::Intl {

Optional<LocaleKey> locale_key_from_value(Value value)
{
    if (value.is_undefined())
        return OptionalNone {};
    if (value.is_null())
        return Empty {};
    if (value.is_string())
        return value.as_string().utf8_string();
    VERIFY_NOT_REACHED();
}

// 6.2.1 IsStructurallyValidLanguageTag ( locale ), https://tc39.es/ecma402/#sec-isstructurallyvalidlanguagetag
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
    // NOTE: LibUnicode's parsing is case-insensitive.

    // 2. If lowerLocale cannot be matched by the unicode_locale_id Unicode locale nonterminal, return false.
    auto locale_id = Unicode::parse_unicode_locale_id(locale);
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
            [](Unicode::LocaleExtension const&) { return 'u'; },
            [](Unicode::TransformedExtension const&) { return 't'; },
            [](Unicode::OtherExtension const& ext) { return static_cast<char>(to_ascii_lowercase(ext.key)); });

        // a. If extensions contains any duplicate singleton subtags, return false.
        if (unique_keys.set(key) != HashSetResult::InsertedNewEntry)
            return false;

        // b. Let transformExtension be the longest substring of extensions matched by the transformed_extensions Unicode
        //    locale nonterminal. If there is no such substring, return true.
        if (auto* transformed = extension.get_pointer<Unicode::TransformedExtension>()) {
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

// 6.2.2 CanonicalizeUnicodeLocaleId ( locale ), https://tc39.es/ecma402/#sec-canonicalizeunicodelocaleid
String canonicalize_unicode_locale_id(StringView locale)
{
    return Unicode::canonicalize_unicode_locale_id(locale);
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

// 6.5.1 AvailableNamedTimeZoneIdentifiers ( ), https://tc39.es/ecma402/#sup-availablenamedtimezoneidentifiers
Vector<TimeZoneIdentifier> const& available_named_time_zone_identifiers()
{
    // It is recommended that the result of AvailableNamedTimeZoneIdentifiers remains the same for the lifetime of the surrounding agent.
    static auto named_time_zone_identifiers = []() {
        // 1. Let identifiers be a List containing the String value of each Zone or Link name in the IANA Time Zone Database.
        auto const& identifiers = Unicode::available_time_zones();

        // 2. Assert: No element of identifiers is an ASCII-case-insensitive match for any other element.
        // 3. Assert: Every element of identifiers identifies a Zone or Link name in the IANA Time Zone Database.
        // 4. Sort identifiers according to lexicographic code unit order.
        // NOTE: All of the above is handled by LibUnicode.

        // 5. Let result be a new empty List.
        Vector<TimeZoneIdentifier> result;
        result.ensure_capacity(identifiers.size());

        bool found_utc = false;

        // 6. For each element identifier of identifiers, do
        for (auto const& identifier : identifiers) {
            // a. Let primary be identifier.
            auto primary = identifier;

            // b. If identifier is a Link name and identifier is not "UTC", then
            if (identifier != "UTC"sv) {
                if (auto resolved = Unicode::resolve_primary_time_zone(identifier); resolved.has_value() && identifier != resolved) {
                    // i. Set primary to the Zone name that identifier resolves to, according to the rules for resolving Link
                    //    names in the IANA Time Zone Database.
                    primary = resolved.release_value();

                    // ii. NOTE: An implementation may need to resolve identifier iteratively.
                }
            }

            // c. If primary is one of "Etc/UTC", "Etc/GMT", or "GMT", set primary to "UTC".
            if (primary.is_one_of("Etc/UTC"sv, "Etc/GMT"sv, "GMT"sv))
                primary = "UTC"_string;

            // d. Let record be the Time Zone Identifier Record { [[Identifier]]: identifier, [[PrimaryIdentifier]]: primary }.
            TimeZoneIdentifier record { .identifier = identifier, .primary_identifier = primary };

            // e. Append record to result.
            result.unchecked_append(move(record));

            if (!found_utc && identifier == "UTC"sv && primary == "UTC"sv)
                found_utc = true;
        }

        // 7. Assert: result contains a Time Zone Identifier Record r such that r.[[Identifier]] is "UTC" and r.[[PrimaryIdentifier]] is "UTC".
        VERIFY(found_utc);

        // 8. Return result.
        return result;
    }();

    return named_time_zone_identifiers;
}

// 6.5.2 GetAvailableNamedTimeZoneIdentifier ( timeZoneIdentifier ), https://tc39.es/ecma402/#sec-getavailablenamedtimezoneidentifier
Optional<TimeZoneIdentifier const&> get_available_named_time_zone_identifier(StringView time_zone_identifier)
{
    // 1. For each element record of AvailableNamedTimeZoneIdentifiers(), do
    for (auto const& record : available_named_time_zone_identifiers()) {
        // a. If record.[[Identifier]] is an ASCII-case-insensitive match for timeZoneIdentifier, return record.
        if (record.identifier.equals_ignoring_ascii_case(time_zone_identifier))
            return record;
    }

    // 2. Return EMPTY.
    return {};
}

// 6.6.1 IsWellFormedUnitIdentifier ( unitIdentifier ), https://tc39.es/ecma402/#sec-iswellformedunitidentifier
bool is_well_formed_unit_identifier(StringView unit_identifier)
{
    // 6.6.2 IsSanctionedSingleUnitIdentifier ( unitIdentifier ), https://tc39.es/ecma402/#sec-issanctionedsingleunitidentifier
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

    // 8. Return seen.
    return seen;
}

// 9.2.3 LookupMatchingLocaleByPrefix ( availableLocales, requestedLocales ), https://tc39.es/ecma402/#sec-lookupmatchinglocalebyprefix
Optional<MatchedLocale> lookup_matching_locale_by_prefix(ReadonlySpan<String> requested_locales)
{
    // 1. For each element locale of requestedLocales, do
    for (auto locale : requested_locales) {
        auto locale_id = Unicode::parse_unicode_locale_id(locale);
        VERIFY(locale_id.has_value());

        // a. Let extension be empty.
        Optional<Unicode::Extension> extension;
        String locale_without_extension;

        // b. If locale contains a Unicode locale extension sequence, then
        if (auto extensions = locale_id->remove_extension_type<Unicode::LocaleExtension>(); !extensions.is_empty()) {
            VERIFY(extensions.size() == 1);

            // i. Set extension to the Unicode locale extension sequence of locale.
            extension = extensions.take_first();

            // ii. Set locale to the String value that is locale with any Unicode locale extension sequences removed.
            locale = locale_id->to_string();
        }

        // c. Let prefix be locale.
        StringView prefix { locale };

        // d. Repeat, while prefix is not the empty String,
        while (!prefix.is_empty()) {
            // i. If availableLocales contains prefix, return the Record { [[locale]]: prefix, [[extension]]: extension }.
            if (Unicode::is_locale_available(prefix))
                return MatchedLocale { MUST(String::from_utf8(prefix)), move(extension) };

            // ii. If prefix contains "-" (code unit 0x002D HYPHEN-MINUS), let pos be the index into prefix of the last
            //     occurrence of "-"; else let pos be 0.
            auto position = prefix.find_last('-').value_or(0);

            // iii. Repeat, while pos ≥ 2 and the substring of prefix from pos - 2 to pos - 1 is "-",
            while (position >= 2 && prefix.substring_view(position - 2, 1) == '-') {
                // 1. Set pos to pos - 2.
                position -= 2;
            }

            // iv. Set prefix to the substring of prefix from 0 to pos.
            prefix = prefix.substring_view(0, position);
        }
    }

    // 2. Return undefined.
    return {};
}

// 9.2.4 LookupMatchingLocaleByBestFit ( availableLocales, requestedLocales ), https://tc39.es/ecma402/#sec-lookupmatchinglocalebybestfit
Optional<MatchedLocale> lookup_matching_locale_by_best_fit(ReadonlySpan<String> requested_locales)
{
    // The algorithm is implementation dependent, but should produce results that a typical user of the requested locales
    // would consider at least as good as those produced by the LookupMatchingLocaleByPrefix algorithm.
    return lookup_matching_locale_by_prefix(requested_locales);
}

// 9.2.6 InsertUnicodeExtensionAndCanonicalize ( locale, attributes, keywords ), https://tc39.es/ecma402/#sec-insert-unicode-extension-and-canonicalize
String insert_unicode_extension_and_canonicalize(Unicode::LocaleID locale, Vector<String> attributes, Vector<Unicode::Keyword> keywords)
{
    // Note: This implementation differs from the spec in how the extension is inserted. The spec assumes
    // the input to this method is a string, and is written such that operations are performed on parts
    // of that string. LibUnicode gives us the parsed locale in a structure, so we can mutate that
    // structure directly.
    locale.extensions.append(Unicode::LocaleExtension { move(attributes), move(keywords) });

    // 10. Return CanonicalizeUnicodeLocaleId(newLocale).
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

    // If you hit this point, you must add any missing keys from [[RelevantExtensionKeys]] to LocaleOptions and ResolvedLocale.
    VERIFY_NOT_REACHED();
}

static Vector<LocaleKey> available_keyword_values(StringView locale, StringView key)
{
    auto key_locale_data = Unicode::available_keyword_values(locale, key);

    Vector<LocaleKey> result;
    result.ensure_capacity(key_locale_data.size());

    for (auto& keyword : key_locale_data)
        result.unchecked_append(move(keyword));

    if (key == "hc"sv) {
        // https://tc39.es/ecma402/#sec-intl.datetimeformat-internal-slots
        // [[LocaleData]].[[<locale>]].[[hc]] must be « null, "h11", "h12", "h23", "h24" ».
        result.prepend(Empty {});
    }

    return result;
}

// 9.2.7 ResolveLocale ( availableLocales, requestedLocales, options, relevantExtensionKeys, localeData ), https://tc39.es/ecma402/#sec-resolvelocale
ResolvedLocale resolve_locale(ReadonlySpan<String> requested_locales, LocaleOptions const& options, ReadonlySpan<StringView> relevant_extension_keys)
{
    static auto true_string = "true"_string;

    // 1. Let matcher be options.[[localeMatcher]].
    auto const& matcher = options.locale_matcher;

    Optional<MatchedLocale> matcher_result;

    // 2. If matcher is "lookup", then
    if (matcher.is_string() && matcher.as_string().utf8_string_view() == "lookup"sv) {
        // a. Let r be LookupMatchingLocaleByPrefix(availableLocales, requestedLocales).
        matcher_result = lookup_matching_locale_by_prefix(requested_locales);
    }
    // 3. Else,
    else {
        // a. Let r be LookupMatchingLocaleByBestFit(availableLocales, requestedLocales).
        matcher_result = lookup_matching_locale_by_best_fit(requested_locales);
    }

    // 4. If r is undefined, set r to the Record { [[locale]]: DefaultLocale(), [[extension]]: empty }.
    if (!matcher_result.has_value())
        matcher_result = MatchedLocale { MUST(String::from_utf8(Unicode::default_locale())), {} };

    // 5. Let foundLocale be r.[[locale]].
    auto found_locale = move(matcher_result->locale);

    // 6. Let foundLocaleData be localeData.[[<foundLocale>]].
    // 7. Assert: Type(foundLocaleData) is Record.

    // 8. Let result be a new Record.
    // 9. Set result.[[LocaleData]] to foundLocaleData.
    ResolvedLocale result {};

    Vector<Unicode::Keyword> keywords;

    // 10. If r.[[extension]] is not empty, then
    if (matcher_result->extension.has_value()) {
        // a. Let components be UnicodeExtensionComponents(r.[[extension]]).
        auto& components = matcher_result->extension->get<Unicode::LocaleExtension>();

        // b. Let keywords be components.[[Keywords]].
        keywords = move(components.keywords);
    }
    // 11. Else,
    //     a. Let keywords be a new empty List.

    // 12. Let supportedKeywords be a new empty List.
    Vector<Unicode::Keyword> supported_keywords;

    Vector<Unicode::Keyword> icu_keywords;

    // 13. For each element key of relevantExtensionKeys, do
    for (auto const& key : relevant_extension_keys) {
        // a. Let keyLocaleData be foundLocaleData.[[<key>]].
        // b. Assert: keyLocaleData is a List.
        auto key_locale_data = available_keyword_values(found_locale, key);

        // c. Let value be keyLocaleData[0].
        // d. Assert: value is a String or value is null.
        auto value = key_locale_data[0];

        // e. Let supportedKeyword be empty.
        Optional<Unicode::Keyword> supported_keyword;

        // f. If keywords contains an element whose [[Key]] is key, then
        if (auto entry = keywords.find_if([&](auto const& entry) { return entry.key == key; }); entry != keywords.end()) {
            // i. Let entry be the element of keywords whose [[Key]] is key.
            // ii. Let requestedValue be entry.[[Value]].
            auto requested_value = entry->value;

            // iii. If requestedValue is not the empty String, then
            if (!requested_value.is_empty()) {
                // 1. If keyLocaleData contains requestedValue, then
                if (key_locale_data.contains_slow(requested_value)) {
                    // a. Set value to requestedValue.
                    value = move(requested_value);

                    // b. Set supportedKeyword to the Record { [[Key]]: key, [[Value]]: value }.
                    supported_keyword = Unicode::Keyword { MUST(String::from_utf8(key)), move(entry->value) };
                }
            }
            // iv. Else if keyLocaleData contains "true", then
            else if (key_locale_data.contains_slow(true_string)) {
                // 1. Set value to "true".
                value = true_string;

                // 2. Set supportedKeyword to the Record { [[Key]]: key, [[Value]]: "" }.
                supported_keyword = Unicode::Keyword { MUST(String::from_utf8(key)), {} };
            }
        }

        // g. Assert: options has a field [[<key>]].
        // h. Let optionsValue be options.[[<key>]].
        // i. Assert: optionsValue is a String, or optionsValue is either undefined or null.
        auto options_value = find_key_in_value(options, key);

        // j. If optionsValue is a String, then
        if (auto* options_string = options_value.has_value() ? options_value->get_pointer<String>() : nullptr) {
            // i. Let ukey be the ASCII-lowercase of key.
            // NOTE: `key` is always lowercase, and this step is likely to be removed:
            //        https://github.com/tc39/ecma402/pull/846#discussion_r1428263375

            // ii. Set optionsValue to CanonicalizeUValue(ukey, optionsValue).
            *options_string = Unicode::canonicalize_unicode_extension_values(key, *options_string);

            // iii. If optionsValue is the empty String, then
            if (options_string->is_empty()) {
                // 1. Set optionsValue to "true".
                *options_string = true_string;
            }
        }

        // k. If SameValue(optionsValue, value) is false and keyLocaleData contains optionsValue, then
        if (options_value.has_value() && (options_value != value) && key_locale_data.contains_slow(*options_value)) {
            // i. Set value to optionsValue.
            value = options_value.release_value();

            // ii. Set supportedKeyword to empty.
            supported_keyword.clear();
        }

        // l. If supportedKeyword is not empty, append supportedKeyword to supportedKeywords.
        if (supported_keyword.has_value())
            supported_keywords.append(supported_keyword.release_value());

        if (auto* value_string = value.get_pointer<String>())
            icu_keywords.empend(MUST(String::from_utf8(key)), *value_string);

        // m. Set result.[[<key>]] to value.
        find_key_in_value(result, key) = move(value);
    }

    // AD-HOC: For ICU, we need to form a locale with all relevant extension keys present.
    if (icu_keywords.is_empty()) {
        result.icu_locale = found_locale;
    } else {
        auto locale_id = Unicode::parse_unicode_locale_id(found_locale);
        VERIFY(locale_id.has_value());

        result.icu_locale = insert_unicode_extension_and_canonicalize(locale_id.release_value(), {}, move(icu_keywords));
    }

    // 14. If supportedKeywords is not empty, then
    if (!supported_keywords.is_empty()) {
        auto locale_id = Unicode::parse_unicode_locale_id(found_locale);
        VERIFY(locale_id.has_value());

        // a. Let supportedAttributes be a new empty List.
        // b. Set foundLocale to InsertUnicodeExtensionAndCanonicalize(foundLocale, supportedAttributes, supportedKeywords).
        found_locale = insert_unicode_extension_and_canonicalize(locale_id.release_value(), {}, move(supported_keywords));
    }

    // 15. Set result.[[Locale]] to foundLocale.
    result.locale = move(found_locale);

    // 16. Return result.
    return result;
}

// 9.2.8 FilterLocales ( availableLocales, requestedLocales, options ), https://tc39.es/ecma402/#sec-lookupsupportedlocales
ThrowCompletionOr<Array*> filter_locales(VM& vm, ReadonlySpan<String> requested_locales, Value options)
{
    auto& realm = *vm.current_realm();

    // 1. Set options to ? CoerceOptionsToObject(options).
    auto* options_object = TRY(coerce_options_to_object(vm, options));

    // 2. Let matcher be ? GetOption(options, "localeMatcher", string, « "lookup", "best fit" », "best fit").
    auto matcher = TRY(get_option(vm, *options_object, vm.names.localeMatcher, OptionType::String, { "lookup"sv, "best fit"sv }, "best fit"sv));

    // 3. Let subset be a new empty List.
    Vector<String> subset;

    // 4. For each element locale of requestedLocales, do
    for (auto const& locale : requested_locales) {
        Optional<MatchedLocale> match;

        // a. If matcher is "lookup", then
        if (matcher.as_string().utf8_string_view() == "lookup"sv) {
            // i. Let match be LookupMatchingLocaleByPrefix(availableLocales, « locale »).
            match = lookup_matching_locale_by_prefix({ { locale } });
        }
        // b. Else,
        else {
            // i. Let match be LookupMatchingLocaleByBestFit(availableLocales, « locale »).
            match = lookup_matching_locale_by_best_fit({ { locale } });
        }

        // c. If match is not undefined, append locale to subset.
        if (match.has_value())
            subset.append(locale);
    }

    // 5. Return CreateArrayFromList(subset).
    return Array::create_from<String>(realm, subset, [&vm](auto& locale) { return PrimitiveString::create(vm, move(locale)); }).ptr();
}

// 9.2.10 CoerceOptionsToObject ( options ), https://tc39.es/ecma402/#sec-coerceoptionstoobject
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

// NOTE: 9.2.11 GetOption has been removed and is being pulled in from ECMA-262 in the Temporal proposal.

// 9.2.12 GetBooleanOrStringNumberFormatOption ( options, property, stringValues, fallback ), https://tc39.es/ecma402/#sec-getbooleanorstringnumberformatoption
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

// 9.2.13 DefaultNumberOption ( value, minimum, maximum, fallback ), https://tc39.es/ecma402/#sec-defaultnumberoption
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

// 9.2.14 GetNumberOption ( options, property, minimum, maximum, fallback ), https://tc39.es/ecma402/#sec-getnumberoption
ThrowCompletionOr<Optional<int>> get_number_option(VM& vm, Object const& options, PropertyKey const& property, int minimum, int maximum, Optional<int> fallback)
{
    // 1. Assert: Type(options) is Object.

    // 2. Let value be ? Get(options, property).
    auto value = TRY(options.get(property));

    // 3. Return ? DefaultNumberOption(value, minimum, maximum, fallback).
    return default_number_option(vm, value, minimum, maximum, move(fallback));
}

}
