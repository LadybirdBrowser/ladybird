/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::HashSet;
use std::sync::OnceLock;

use ak::Utf16String;

use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{OptionDefault, OptionType, get_option, get_options_object};
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::date::TimeZoneIdentifier;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::intl_object::IntlObject;
use crate::runtime::intl::locale::as_locale;
use crate::runtime::intl::single_unit_identifiers::sanctioned_single_unit_identifiers;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::unicode::intl::{self as unicode, Extension, Keyword, LocaleExtension, LocaleID};
use crate::unicode::time_zone as unicode_time_zone;
use crate::utf16::Utf16View;

/// Variant<Empty, Utf16String>, the value of a Unicode extension key: null (Empty) or a string.
#[derive(Clone, PartialEq, Eq)]
pub enum LocaleKey {
    Empty,
    String(Utf16String),
}

pub type ResolvedLocaleKey = LocaleKey;

/// The record of the options ResolveOptions resolves, for ResolveLocale.
pub struct LocaleOptions {
    pub locale_matcher: Value,
    pub ca: Option<LocaleKey>, // [[Calendar]]
    pub co: Option<LocaleKey>, // [[Collation]]
    pub hc: Option<LocaleKey>, // [[HourCycle]]
    pub kf: Option<LocaleKey>, // [[CaseFirst]]
    pub kn: Option<LocaleKey>, // [[Numeric]]
    pub nu: Option<LocaleKey>, // [[NumberingSystem]]
    pub hour12: Value,
}

impl Default for LocaleOptions {
    fn default() -> Self {
        Self {
            locale_matcher: Value::UNDEFINED,
            ca: None,
            co: None,
            hc: None,
            kf: None,
            kn: None,
            nu: None,
            hour12: Value::UNDEFINED,
        }
    }
}

pub struct MatchedLocale {
    pub locale: Utf16String,
    pub extension: Option<LocaleExtension>,
}

pub struct ResolvedLocale {
    pub locale: Utf16String,
    pub icu_locale: Utf16String,
    pub ca: ResolvedLocaleKey, // [[Calendar]]
    pub co: ResolvedLocaleKey, // [[Collation]]
    pub hc: ResolvedLocaleKey, // [[HourCycle]]
    pub kf: ResolvedLocaleKey, // [[CaseFirst]]
    pub kn: ResolvedLocaleKey, // [[Numeric]]
    pub nu: ResolvedLocaleKey, // [[NumberingSystem]]
}

impl Default for ResolvedLocale {
    fn default() -> Self {
        Self {
            locale: Utf16String::default(),
            icu_locale: Utf16String::default(),
            ca: LocaleKey::Empty,
            co: LocaleKey::Empty,
            hc: LocaleKey::Empty,
            kf: LocaleKey::Empty,
            kn: LocaleKey::Empty,
            nu: LocaleKey::Empty,
        }
    }
}

pub struct ResolvedOptions {
    pub options: Gc<Object>,
    pub resolved_locale: ResolvedLocale,
    pub resolution_options: LocaleOptions,
}

/// SpecialBehaviors, the special behaviours ResolveOptions may be given.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct SpecialBehaviors(u8);

impl SpecialBehaviors {
    pub const NONE: Self = Self(0);
    pub const REQUIRE_OPTIONS: Self = Self(1 << 1);
    pub const COERCE_OPTIONS: Self = Self(1 << 2);

    pub fn has(self, flag: Self) -> bool {
        self.0 & flag.0 == flag.0
    }
}

impl core::ops::BitOr for SpecialBehaviors {
    type Output = Self;

    fn bitor(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

/// Variant<StringView, bool>.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StringOrBoolean {
    String(&'static str),
    Boolean(bool),
}

/// Throws a RangeError that `value` is not a valid value for the option `property`, with the code units of both.
pub fn throw_option_is_not_valid_value<T>(
    vm: &Vm,
    value: Utf16View<'_>,
    property: &PropertyKey,
) -> ThrowCompletionOr<T> {
    vm.throw_completion(
        ErrorKind::RangeError,
        ErrorType::OptionIsNotValidValue,
        &[&value, property],
    )
}

/// Throws a RangeError that `tag` is not a structurally valid language tag, with the code units of `tag`.
pub fn throw_invalid_language_tag<T>(vm: &Vm, tag: Utf16View<'_>) -> ThrowCompletionOr<T> {
    vm.throw_completion(ErrorKind::RangeError, ErrorType::IntlInvalidLanguageTag, &[&tag])
}

/// AK's Utf16View::equals_ignoring_ascii_case.
pub fn equals_ignoring_ascii_case(lhs: Utf16View<'_>, rhs: Utf16View<'_>) -> bool {
    fn to_ascii_lowercase(code_unit: u16) -> u16 {
        if (u16::from(b'A')..=u16::from(b'Z')).contains(&code_unit) {
            code_unit + 0x20
        } else {
            code_unit
        }
    }

    lhs.length_in_code_units() == rhs.length_in_code_units()
        && lhs
            .code_units()
            .zip(rhs.code_units())
            .all(|(lhs, rhs)| to_ascii_lowercase(lhs) == to_ascii_lowercase(rhs))
}

/// The ASCII-lowercase of `string`, as AK's Utf16String::to_ascii_lowercase.
pub fn to_ascii_lowercase(string: Utf16View<'_>) -> Utf16String {
    let code_units: Vec<u16> = string
        .code_units()
        .map(|code_unit| {
            if (u16::from(b'A')..=u16::from(b'Z')).contains(&code_unit) {
                code_unit + 0x20
            } else {
                code_unit
            }
        })
        .collect();
    Utf16String::from_utf16(&code_units)
}

/// The ASCII-uppercase of `string`, as AK's Utf16String::to_ascii_uppercase.
pub fn to_ascii_uppercase(string: Utf16View<'_>) -> Utf16String {
    let code_units: Vec<u16> = string
        .code_units()
        .map(|code_unit| {
            if (u16::from(b'a')..=u16::from(b'z')).contains(&code_unit) {
                code_unit - 0x20
            } else {
                code_unit
            }
        })
        .collect();
    Utf16String::from_utf16(&code_units)
}

/// CreateArrayFromList of a list of strings.
pub fn create_array_from_string_list(vm: &Vm, realm: Gc<Realm>, list: &[Utf16String]) -> Gc<Array> {
    let values = MarkedVec::new(vm);
    for string in list {
        values.push(Value::from_string(PrimitiveString::create(vm, string.clone())));
    }
    Array::create_from_list(vm, realm, &values)
}

fn contains_duplicate_variant(variants: &[Utf16String]) -> bool {
    if variants.len() < 2 {
        return false;
    }

    let mut lowercase_variants: Vec<Utf16String> = variants
        .iter()
        .map(|variant| to_ascii_lowercase(Utf16View::of_string(variant)))
        .collect();

    lowercase_variants.sort_unstable_by(|lhs, rhs| {
        let (lhs, rhs) = (Utf16View::of_string(lhs), Utf16View::of_string(rhs));
        lhs.code_units().cmp(rhs.code_units())
    });

    lowercase_variants.windows(2).any(|pair| pair[0] == pair[1])
}

// 6.2.1 IsWellFormedLanguageTag ( locale ), https://tc39.es/ecma402/#sec-iswellformedlanguagetag
pub fn is_well_formed_language_tag(locale: Utf16View<'_>) -> bool {
    // 1. Let lowerLocale be the ASCII-lowercase of locale.
    // NOTE: LibUnicode's parsing is case-insensitive.

    // 2. If lowerLocale cannot be matched by the unicode_locale_id Unicode locale nonterminal, return false.
    let Some(mut locale_id) = unicode::parse_unicode_locale_id(locale) else {
        return false;
    };

    // 3. If lowerLocale uses any of the backwards compatibility syntax described in Unicode Technical Standard #35 Part 1 Core,
    //    Section 3.3 BCP 47 Conformance, return false.
    //    https://unicode.org/reports/tr35/#BCP_47_Conformance
    if locale.code_units().any(|code_unit| code_unit == u16::from(b'_'))
        || locale_id.language_id.is_root
        || locale_id.language_id.language.is_none()
    {
        return false;
    }

    // 4. Let languageId be the longest prefix of lowerLocale matched by the unicode_language_id Unicode locale nonterminal.
    let language_id = &mut locale_id.language_id;

    // 5. Let variants be GetLocaleVariants(languageId).
    // 6. If variants is not undefined, then
    if !language_id.variants.is_empty() {
        // a. If variants contains any duplicate subtags, return false.
        if contains_duplicate_variant(&language_id.variants) {
            return false;
        }
    }

    let mut unique_keys: HashSet<u8> = HashSet::new();

    // 7. Let allExtensions be the suffix of lowerLocale following languageId.
    // 8. If allExtensions contains a substring matched by the pu_extensions Unicode locale nonterminal, let extensions be
    //    the prefix of allExtensions preceding the longest such substring. Otherwise, let extensions be allExtensions.
    // 9. If extensions is not the empty String, then
    for extension in &mut locale_id.extensions {
        let key = match extension {
            Extension::Locale(_) => b'u',
            Extension::Transformed(_) => b't',
            Extension::Other(extension) => extension.key.to_ascii_lowercase(),
        };

        // a. If extensions contains any duplicate singleton subtags, return false.
        if !unique_keys.insert(key) {
            return false;
        }

        // b. Let transformExtension be the longest substring of extensions matched by the transformed_extensions Unicode
        //    locale nonterminal. If there is no such substring, return true.
        if let Extension::Transformed(transformed) = extension {
            // c. Assert: The substring of transformExtension from 0 to 3 is "-t-".
            // d. Let tPrefix be the substring of transformExtension from 3.

            // e. Let tlang be the longest prefix of tPrefix matched by the tlang Unicode locale nonterminal. If there is
            //    no such prefix, return true.
            let Some(transformed_language) = &mut transformed.language else {
                continue;
            };

            // f. Let tlangRefinements be the longest suffix of tlang following a non-empty prefix matched by the
            //    unicode_language_subtag Unicode locale nonterminal.
            // g. If tlangRefinements contains any duplicate substrings matched greedily by the unicode_variant_subtag
            //    Unicode locale nonterminal, return false.
            if contains_duplicate_variant(&transformed_language.variants) {
                return false;
            }
        }
    }

    // 10. Return true.
    true
}

// 6.2.2 CanonicalizeUnicodeLocaleId ( locale ), https://tc39.es/ecma402/#sec-canonicalizeunicodelocaleid
pub fn canonicalize_unicode_locale_id(vm: &Vm, locale: Utf16View<'_>) -> ThrowCompletionOr<Utf16String> {
    // AD-HOC: The specification treats canonicalization as infallible, but ICU refuses locales that exceed its own
    //         representation limits. Those are reported as a RangeError.
    match unicode::canonicalize_unicode_locale_id(locale) {
        Some(canonicalized_locale) => Ok(canonicalized_locale),
        None => vm.throw_completion(ErrorKind::RangeError, ErrorType::IntlUnsupportedLanguageTag, &[&locale]),
    }
}

// 6.3.1 IsWellFormedCurrencyCode ( currency ), https://tc39.es/ecma402/#sec-iswellformedcurrencycode
pub fn is_well_formed_currency_code(currency: Utf16View<'_>) -> bool {
    // 1. If the length of currency is not 3, return false.
    if currency.length_in_code_units() != 3 {
        return false;
    }

    // 2. Let normalized be the ASCII-uppercase of currency.
    // 3. If normalized contains any code unit outside of 0x0041 through 0x005A (corresponding to Unicode characters LATIN CAPITAL LETTER A through LATIN CAPITAL LETTER Z), return false.
    if !currency
        .code_units()
        .all(|code_unit| u8::try_from(code_unit).is_ok_and(|byte| byte.is_ascii_alphabetic()))
    {
        return false;
    }

    // 4. Return true.
    true
}

// 6.5.1 AvailableNamedTimeZoneIdentifiers ( ), https://tc39.es/ecma402/#sup-availablenamedtimezoneidentifiers
pub fn available_named_time_zone_identifiers() -> &'static [TimeZoneIdentifier] {
    // It is recommended that the result of AvailableNamedTimeZoneIdentifiers remains the same for the lifetime of the surrounding agent.
    static NAMED_TIME_ZONE_IDENTIFIERS: OnceLock<Vec<TimeZoneIdentifier>> = OnceLock::new();

    NAMED_TIME_ZONE_IDENTIFIERS.get_or_init(|| {
        // 1. Let identifiers be a List containing the String value of each Zone or Link name in the IANA Time Zone Database.
        let identifiers = unicode_time_zone::available_time_zones();

        // 2. Assert: No element of identifiers is an ASCII-case-insensitive match for any other element.
        // 3. Assert: Every element of identifiers identifies a Zone or Link name in the IANA Time Zone Database.
        // 4. Sort identifiers according to lexicographic code unit order.
        // NOTE: All of the above is handled by LibUnicode.

        // 5. Let result be a new empty List.
        let mut result = Vec::with_capacity(identifiers.len());

        let mut found_utc = false;

        // 6. For each element identifier of identifiers, do
        for identifier in identifiers {
            let identifier = Utf16String::from_utf16(identifier);

            // a. Let primary be identifier.
            let mut primary = identifier.clone();

            // b. If identifier is a Link name and identifier is not "UTC", then
            if Utf16View::of_string(&identifier) != "UTC"
                && let Some(resolved) = unicode_time_zone::resolve_primary_time_zone(Utf16View::of_string(&identifier))
                && identifier != resolved
            {
                // i. Set primary to the Zone name that identifier resolves to, according to the rules for resolving Link
                //    names in the IANA Time Zone Database.
                primary = resolved;

                // ii. NOTE: An implementation may need to resolve identifier iteratively.
            }

            // c. If primary is one of "Etc/UTC", "Etc/GMT", or "GMT", set primary to "UTC".
            let primary_view = Utf16View::of_string(&primary);
            if primary_view == "Etc/UTC" || primary_view == "Etc/GMT" || primary_view == "GMT" {
                primary = Utf16String::from_utf8("UTC");
            }

            if !found_utc && Utf16View::of_string(&identifier) == "UTC" && Utf16View::of_string(&primary) == "UTC" {
                found_utc = true;
            }

            // d. Let record be the Time Zone Identifier Record { [[Identifier]]: identifier, [[PrimaryIdentifier]]: primary }.
            let record = TimeZoneIdentifier {
                identifier,
                primary_identifier: primary,
            };

            // e. Append record to result.
            result.push(record);
        }

        // 7. Assert: result contains a Time Zone Identifier Record r such that r.[[Identifier]] is "UTC" and r.[[PrimaryIdentifier]] is "UTC".
        assert!(found_utc);

        // 8. Return result.
        result
    })
}

// 6.5.2 GetAvailableNamedTimeZoneIdentifier ( timeZoneIdentifier ), https://tc39.es/ecma402/#sec-getavailablenamedtimezoneidentifier
pub fn get_available_named_time_zone_identifier(
    time_zone_identifier: Utf16View<'_>,
) -> Option<&'static TimeZoneIdentifier> {
    // 1. For each element record of AvailableNamedTimeZoneIdentifiers(), do
    // a. If record.[[Identifier]] is an ASCII-case-insensitive match for timeZoneIdentifier, return record.
    // 2. Return EMPTY.
    available_named_time_zone_identifiers()
        .iter()
        .find(|record| equals_ignoring_ascii_case(Utf16View::of_string(&record.identifier), time_zone_identifier))
}

// 6.6.1 IsWellFormedUnitIdentifier ( unitIdentifier ), https://tc39.es/ecma402/#sec-iswellformedunitidentifier
pub fn is_well_formed_unit_identifier(unit_identifier: Utf16View<'_>) -> bool {
    // 6.6.2 IsSanctionedSingleUnitIdentifier ( unitIdentifier ), https://tc39.es/ecma402/#sec-issanctionedsingleunitidentifier
    let is_sanctioned_single_unit_identifier = |unit_identifier: Utf16View<'_>| {
        // 1. If unitIdentifier is listed in Table 2 below, return true.
        // 2. Else, return false.
        sanctioned_single_unit_identifiers()
            .iter()
            .any(|sanctioned_unit| unit_identifier == *sanctioned_unit)
    };

    // 1. If ! IsSanctionedSingleUnitIdentifier(unitIdentifier) is true, then
    if is_sanctioned_single_unit_identifier(unit_identifier) {
        // a. Return true.
        return true;
    }

    let per = Utf16View::Ascii(b"-per-");

    // 2. Let i be StringIndexOf(unitIdentifier, "-per-", 0).
    let index = unit_identifier.find_code_unit_offset(per, 0);

    // 3. If i is -1 or StringIndexOf(unitIdentifier, "-per-", i + 1) is not -1, then
    let Some(index) = index else {
        // a. Return false.
        return false;
    };
    if unit_identifier.find_code_unit_offset(per, index + 1).is_some() {
        // a. Return false.
        return false;
    }

    // 4. Assert: The five-character substring "-per-" occurs exactly once in unitIdentifier, at index i.
    // NOTE: We skip this because the checks above already verify this invariant.

    // 5. Let numerator be the substring of unitIdentifier from 0 to i.
    let numerator = unit_identifier.substring_view(0, index);

    // 6. Let denominator be the substring of unitIdentifier from i + 5.
    let denominator = unit_identifier.substring_view(index + 5, unit_identifier.length_in_code_units() - index - 5);

    // 7. If ! IsSanctionedSingleUnitIdentifier(numerator) and ! IsSanctionedSingleUnitIdentifier(denominator) are both true, then
    if is_sanctioned_single_unit_identifier(numerator) && is_sanctioned_single_unit_identifier(denominator) {
        // a. Return true.
        return true;
    }

    // 8. Return false.
    false
}

// 9.2.1 CanonicalizeLocaleList ( locales ), https://tc39.es/ecma402/#sec-canonicalizelocalelist
pub fn canonicalize_locale_list(vm: &Vm, locales: Value) -> ThrowCompletionOr<Vec<Utf16String>> {
    let realm = vm.current_realm().expect("CanonicalizeLocaleList runs in a realm");

    // 1. If locales is undefined, then
    if locales.is_undefined() {
        // a. Return a new empty List.
        return Ok(Vec::new());
    }

    // 2. Let seen be a new empty List.
    let mut seen: Vec<Utf16String> = Vec::new();

    // 3. If Type(locales) is String or Type(locales) is Object and locales has an [[InitializedLocale]] internal slot, then
    let object: Gc<Object> = if locales.is_string() || as_locale(locales).is_some() {
        // a. Let O be CreateArrayFromList(« locales »).
        Array::create_from(vm, realm, &[locales]).upcast()
    }
    // 4. Else,
    else {
        // a. Let O be ? ToObject(locales).
        locales.to_object(vm)?
    };

    // 5. Let len be ? ToLength(? Get(O, "length")).
    let length_value = object.get(vm, &vm.names.length)?;
    let length = length_value.to_length(vm)?;

    // 6. Let k be 0.
    // 7. Repeat, while k < len,
    for k in 0..length {
        // a. Let Pk be ToString(k).
        let property_key = PropertyKey::from_number(k);

        // b. Let kPresent be ? HasProperty(O, Pk).
        let key_present = object.has_property(vm, &property_key)?;

        // c. If kPresent is true, then
        if key_present {
            // i. Let kValue be ? Get(O, Pk).
            let key_value = object.get(vm, &property_key)?;

            // ii. If Type(kValue) is not String or Object, throw a TypeError exception.
            if !key_value.is_string() && !key_value.is_object() {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOrString, &[&key_value]);
            }

            let canonicalized_tag;

            // iii. If Type(kValue) is Object and kValue has an [[InitializedLocale]] internal slot, then
            if let Some(locale) = as_locale(key_value) {
                // 1. Let tag be kValue.[[Locale]].
                let tag = locale.locale();

                // v. If IsWellFormedLanguageTag(tag) is false, throw a RangeError exception.
                if !is_well_formed_language_tag(Utf16View::of_string(&tag)) {
                    return throw_invalid_language_tag(vm, Utf16View::of_string(&tag));
                }

                // vi. Let canonicalizedTag be ! CanonicalizeUnicodeLocaleId(tag).
                canonicalized_tag = canonicalize_unicode_locale_id(vm, Utf16View::of_string(&tag))?;
            }
            // iv. Else,
            else {
                // 1. Let tag be ? ToString(kValue).
                let tag = key_value.to_utf16_string(vm)?;

                // v. If IsWellFormedLanguageTag(tag) is false, throw a RangeError exception.
                if !is_well_formed_language_tag(Utf16View::of_string(&tag)) {
                    return throw_invalid_language_tag(vm, Utf16View::of_string(&tag));
                }

                // vi. Let canonicalizedTag be ! CanonicalizeUnicodeLocaleId(tag).
                canonicalized_tag = canonicalize_unicode_locale_id(vm, Utf16View::of_string(&tag))?;
            }

            // vii. If canonicalizedTag is not an element of seen, append canonicalizedTag as the last element of seen.
            if !seen.contains(&canonicalized_tag) {
                seen.push(canonicalized_tag);
            }
        }

        // d. Increase k by 1.
    }

    // 8. Return seen.
    Ok(seen)
}

/// The offset of the last "-" in `string`, as AK's Utf16View::find_last_code_point_offset('-').
fn find_last_hyphen(string: Utf16View<'_>) -> Option<usize> {
    (0..string.length_in_code_units())
        .rev()
        .find(|&index| string.code_unit_at(index) == u16::from(b'-'))
}

// 9.2.3 LookupMatchingLocaleByPrefix ( availableLocales, requestedLocales ), https://tc39.es/ecma402/#sec-lookupmatchinglocalebyprefix
pub fn lookup_matching_locale_by_prefix(requested_locales: &[Utf16String]) -> Option<MatchedLocale> {
    // 1. For each element locale of requestedLocales, do
    for locale in requested_locales {
        let mut locale = locale.clone();
        let mut locale_id = unicode::parse_unicode_locale_id(Utf16View::of_string(&locale))
            .expect("a requested locale is a Unicode locale identifier");

        // a. Let extension be empty.
        let mut extension = None;
        // b. If locale contains a Unicode locale extension sequence, then
        let mut extensions = locale_id.remove_locale_extensions();
        if !extensions.is_empty() {
            assert!(extensions.len() == 1);

            // i. Set extension to the Unicode locale extension sequence of locale.
            extension = Some(extensions.remove(0));

            // ii. Set locale to the String value that is locale with any Unicode locale extension sequences removed.
            locale = locale_id.to_utf16_string();
        }

        // c. Let prefix be locale.
        let mut prefix = Utf16View::of_string(&locale);

        // d. Repeat, while prefix is not the empty String,
        while !prefix.is_empty() {
            // i. If availableLocales contains prefix, return the Record { [[locale]]: prefix, [[extension]]: extension }.
            if unicode::is_locale_available(prefix) {
                return Some(MatchedLocale {
                    locale: prefix.to_utf16_string(),
                    extension,
                });
            }

            // ii. If prefix contains "-" (code unit 0x002D HYPHEN-MINUS), let pos be the index into prefix of the last
            //     occurrence of "-"; else let pos be 0.
            let mut position = find_last_hyphen(prefix).unwrap_or(0);

            // iii. Repeat, while pos ≥ 2 and the substring of prefix from pos - 2 to pos - 1 is "-",
            while position >= 2 && prefix.code_unit_at(position - 2) == u16::from(b'-') {
                // 1. Set pos to pos - 2.
                position -= 2;
            }

            // iv. Set prefix to the substring of prefix from 0 to pos.
            prefix = prefix.substring_view(0, position);
        }
    }

    // 2. Return undefined.
    None
}

// 9.2.4 LookupMatchingLocaleByBestFit ( availableLocales, requestedLocales ), https://tc39.es/ecma402/#sec-lookupmatchinglocalebybestfit
pub fn lookup_matching_locale_by_best_fit(requested_locales: &[Utf16String]) -> Option<MatchedLocale> {
    // The algorithm is implementation dependent, but should produce results that a typical user of the requested locales
    // would consider at least as good as those produced by the LookupMatchingLocaleByPrefix algorithm.
    lookup_matching_locale_by_prefix(requested_locales)
}

// 9.2.6 InsertUnicodeExtensionAndCanonicalize ( locale, attributes, keywords ), https://tc39.es/ecma402/#sec-insert-unicode-extension-and-canonicalize
pub fn insert_unicode_extension_and_canonicalize(
    vm: &Vm,
    mut locale: LocaleID,
    attributes: Vec<Utf16String>,
    keywords: Vec<Keyword>,
) -> ThrowCompletionOr<Utf16String> {
    // Note: This implementation differs from the spec in how the extension is inserted. The spec assumes
    // the input to this method is a string, and is written such that operations are performed on parts
    // of that string. LibUnicode gives us the parsed locale in a structure, so we can mutate that
    // structure directly.
    locale
        .extensions
        .push(Extension::Locale(LocaleExtension { attributes, keywords }));

    // 10. Return CanonicalizeUnicodeLocaleId(newLocale).
    let locale_string = locale.to_utf16_string();
    canonicalize_unicode_locale_id(vm, Utf16View::of_string(&locale_string))
}

fn find_key_in_locale_options<'a>(value: &'a mut LocaleOptions, key: &str) -> &'a mut Option<LocaleKey> {
    match key {
        "ca" => &mut value.ca,
        "co" => &mut value.co,
        "hc" => &mut value.hc,
        "kf" => &mut value.kf,
        "kn" => &mut value.kn,
        "nu" => &mut value.nu,
        // If you hit this point, you must add any missing keys from [[RelevantExtensionKeys]] to LocaleOptions and ResolvedLocale.
        _ => unreachable!("{key} is not a relevant extension key"),
    }
}

fn find_key_in_resolved_locale<'a>(value: &'a mut ResolvedLocale, key: &str) -> &'a mut ResolvedLocaleKey {
    match key {
        "ca" => &mut value.ca,
        "co" => &mut value.co,
        "hc" => &mut value.hc,
        "kf" => &mut value.kf,
        "kn" => &mut value.kn,
        "nu" => &mut value.nu,
        // If you hit this point, you must add any missing keys from [[RelevantExtensionKeys]] to LocaleOptions and ResolvedLocale.
        _ => unreachable!("{key} is not a relevant extension key"),
    }
}

fn available_keyword_values(locale: Utf16View<'_>, key: &str) -> Vec<LocaleKey> {
    let key_locale_data = unicode::available_keyword_values(locale, Utf16View::Ascii(key.as_bytes()));

    let mut result: Vec<LocaleKey> = Vec::with_capacity(key_locale_data.len() + 1);

    if key == "hc" {
        // https://tc39.es/ecma402/#sec-intl.datetimeformat-internal-slots
        // [[LocaleData]].[[<locale>]].[[hc]] must be « null, "h11", "h12", "h23", "h24" ».
        result.push(LocaleKey::Empty);
    }

    result.extend(key_locale_data.into_iter().map(LocaleKey::String));
    result
}

// 9.2.7 ResolveLocale ( availableLocales, requestedLocales, options, relevantExtensionKeys, localeData ), https://tc39.es/ecma402/#sec-resolvelocale
pub fn resolve_locale(
    vm: &Vm,
    requested_locales: &[Utf16String],
    options: &LocaleOptions,
    relevant_extension_keys: &[&str],
) -> ThrowCompletionOr<ResolvedLocale> {
    let true_string = LocaleKey::String(Utf16String::from_utf8("true"));

    // 1. Let matcher be options.[[localeMatcher]].
    let matcher = options.locale_matcher;

    // 2. If matcher is "lookup", then
    let matcher_result = if matcher.is_string() && matcher.as_string().utf16_string_view() == "lookup" {
        // a. Let r be LookupMatchingLocaleByPrefix(availableLocales, requestedLocales).
        lookup_matching_locale_by_prefix(requested_locales)
    }
    // 3. Else,
    else {
        // a. Let r be LookupMatchingLocaleByBestFit(availableLocales, requestedLocales).
        lookup_matching_locale_by_best_fit(requested_locales)
    };

    // 4. If r is undefined, set r to the Record { [[locale]]: DefaultLocale(), [[extension]]: empty }.
    let matcher_result = matcher_result.unwrap_or_else(|| MatchedLocale {
        locale: unicode::default_locale(),
        extension: None,
    });

    // 5. Let foundLocale be r.[[locale]].
    let mut found_locale = matcher_result.locale;

    // 6. Let foundLocaleData be localeData.[[<foundLocale>]].
    // 7. Assert: Type(foundLocaleData) is Record.

    // 8. Let result be a new Record.
    // 9. Set result.[[LocaleData]] to foundLocaleData.
    let mut result = ResolvedLocale::default();

    // 10. If r.[[extension]] is not empty, then
    //     a. Let components be UnicodeExtensionComponents(r.[[extension]]).
    //     b. Let keywords be components.[[Keywords]].
    // 11. Else,
    //     a. Let keywords be a new empty List.
    let keywords: Vec<Keyword> = matcher_result
        .extension
        .map(|extension| extension.keywords)
        .unwrap_or_default();

    // 12. Let supportedKeywords be a new empty List.
    let mut supported_keywords: Vec<Keyword> = Vec::new();

    let mut icu_keywords: Vec<Keyword> = Vec::new();

    // 13. For each element key of relevantExtensionKeys, do
    for &key in relevant_extension_keys {
        let key_string = Utf16String::from_utf8(key);

        // a. Let keyLocaleData be foundLocaleData.[[<key>]].
        // b. Assert: keyLocaleData is a List.
        let key_locale_data = available_keyword_values(Utf16View::of_string(&found_locale), key);

        // c. Let value be keyLocaleData[0].
        // d. Assert: value is a String or value is null.
        let mut value = key_locale_data[0].clone();

        // e. Let supportedKeyword be empty.
        let mut supported_keyword: Option<Keyword> = None;

        // f. If keywords contains an element whose [[Key]] is key, then
        if let Some(entry) = keywords.iter().find(|entry| Utf16View::of_string(&entry.key) == key) {
            // i. Let entry be the element of keywords whose [[Key]] is key.
            // ii. Let requestedValue be entry.[[Value]].
            let requested_value = entry.value.clone();

            // iii. If requestedValue is not the empty String, then
            if !requested_value.is_empty() {
                // 1. If keyLocaleData contains requestedValue, then
                let requested_value = LocaleKey::String(requested_value);
                if key_locale_data.contains(&requested_value) {
                    // a. Set value to requestedValue.
                    value = requested_value;

                    // b. Set supportedKeyword to the Record { [[Key]]: key, [[Value]]: value }.
                    supported_keyword = Some(Keyword {
                        key: key_string.clone(),
                        value: entry.value.clone(),
                    });
                }
            }
            // iv. Else if keyLocaleData contains "true", then
            else if key_locale_data.contains(&true_string) {
                // 1. Set value to "true".
                value = true_string.clone();

                // 2. Set supportedKeyword to the Record { [[Key]]: key, [[Value]]: "" }.
                supported_keyword = Some(Keyword {
                    key: key_string.clone(),
                    value: Utf16String::default(),
                });
            }
        }

        // g. Assert: options has a field [[<key>]].
        // h. Let optionsValue be options.[[<key>]].
        // i. Assert: optionsValue is a String, or optionsValue is either undefined or null.
        let mut options_value = find_key_in_locale_options_ref(options, key).cloned();

        // j. If optionsValue is a String, then
        if let Some(LocaleKey::String(options_string)) = &mut options_value {
            // i. Let ukey be the ASCII-lowercase of key.
            // NOTE: `key` is always lowercase, and this step is likely to be removed:
            //        https://github.com/tc39/ecma402/pull/846#discussion_r1428263375

            // ii. Set optionsValue to CanonicalizeUValue(ukey, optionsValue).
            let canonicalized = unicode::canonicalize_unicode_extension_values(
                Utf16View::Ascii(key.as_bytes()),
                Utf16View::of_string(options_string),
            );
            *options_string = canonicalized;

            // iii. If optionsValue is the empty String, then
            if options_string.is_empty() {
                // 1. Set optionsValue to "true".
                *options_string = Utf16String::from_utf8("true");
            }
        }

        // k. If SameValue(optionsValue, value) is false and keyLocaleData contains optionsValue, then
        if let Some(options_value) = options_value
            && options_value != value
            && key_locale_data.contains(&options_value)
        {
            // i. Set value to optionsValue.
            value = options_value;

            // ii. Set supportedKeyword to empty.
            supported_keyword = None;
        }

        // l. If supportedKeyword is not empty, append supportedKeyword to supportedKeywords.
        if let Some(supported_keyword) = supported_keyword {
            supported_keywords.push(supported_keyword);
        }

        if let LocaleKey::String(value_string) = &value {
            icu_keywords.push(Keyword {
                key: key_string.clone(),
                value: value_string.clone(),
            });
        }

        // m. Set result.[[<key>]] to value.
        *find_key_in_resolved_locale(&mut result, key) = value;
    }

    // AD-HOC: For ICU, we need to form a locale with all relevant extension keys present.
    if icu_keywords.is_empty() {
        result.icu_locale = found_locale.clone();
    } else {
        let locale_id = unicode::parse_unicode_locale_id(Utf16View::of_string(&found_locale))
            .expect("the found locale is a Unicode locale identifier");

        let icu_locale = insert_unicode_extension_and_canonicalize(vm, locale_id, Vec::new(), icu_keywords)?;
        result.icu_locale = icu_locale;
    }

    // 14. If supportedKeywords is not empty, then
    if !supported_keywords.is_empty() {
        let locale_id = unicode::parse_unicode_locale_id(Utf16View::of_string(&found_locale))
            .expect("the found locale is a Unicode locale identifier");

        // a. Let supportedAttributes be a new empty List.
        // b. Set foundLocale to InsertUnicodeExtensionAndCanonicalize(foundLocale, supportedAttributes, supportedKeywords).
        let supported_locale =
            insert_unicode_extension_and_canonicalize(vm, locale_id, Vec::new(), supported_keywords)?;
        found_locale = supported_locale;
    }

    // 15. Set result.[[Locale]] to foundLocale.
    result.locale = found_locale;

    // 16. Return result.
    Ok(result)
}

fn find_key_in_locale_options_ref<'a>(value: &'a LocaleOptions, key: &str) -> Option<&'a LocaleKey> {
    match key {
        "ca" => value.ca.as_ref(),
        "co" => value.co.as_ref(),
        "hc" => value.hc.as_ref(),
        "kf" => value.kf.as_ref(),
        "kn" => value.kn.as_ref(),
        "nu" => value.nu.as_ref(),
        // If you hit this point, you must add any missing keys from [[RelevantExtensionKeys]] to LocaleOptions and ResolvedLocale.
        _ => unreachable!("{key} is not a relevant extension key"),
    }
}

// 9.2.8 ResolveOptions ( constructor, localeData, locales, options [ , specialBehaviours [ , modifyResolutionOptions ] ] ), https://tc39.es/ecma402/#sec-resolveoptions
pub fn resolve_options(
    vm: &Vm,
    object: &dyn IntlObject,
    locales: Value,
    options_value: Value,
    special_behaviours: SpecialBehaviors,
    modify_resolution_options: Option<&dyn Fn(&mut LocaleOptions)>,
) -> ThrowCompletionOr<ResolvedOptions> {
    // 1. Let requestedLocales be ? CanonicalizeLocaleList(locales).
    let requested_locales = canonicalize_locale_list(vm, locales)?;

    // 2. If specialBehaviours is present and contains REQUIRE-OPTIONS and options is undefined, throw a TypeError exception.
    if special_behaviours.has(SpecialBehaviors::REQUIRE_OPTIONS) && options_value.is_undefined() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::IsUndefined, &[&"options"]);
    }

    // 3. If specialBehaviours is present and contains COERCE-OPTIONS, set options to ? CoerceOptionsToObject(options).
    //    Otherwise, set options to ? GetOptionsObject(options).
    let options = if special_behaviours.has(SpecialBehaviors::COERCE_OPTIONS) {
        coerce_options_to_object(vm, options_value)?
    } else {
        get_options_object(vm, options_value)?
    };

    // 4. Let matcher be ? GetOption(options, "localeMatcher", STRING, « "lookup", "best fit" », "best fit").
    let matcher = get_option(
        vm,
        &options,
        &vm.names.localeMatcher,
        OptionType::String,
        &["lookup", "best fit"],
        OptionDefault::string("best fit"),
    )?;

    // 5. Let opt be the Record { [[localeMatcher]]: matcher }.
    let mut opt = LocaleOptions {
        locale_matcher: matcher,
        ..LocaleOptions::default()
    };

    // 6. For each Resolution Option Descriptor desc of constructor.[[ResolutionOptionDescriptors]], do
    for descriptor in object.resolution_option_descriptors(vm) {
        // a. If desc has a [[Type]] field, let type be desc.[[Type]]. Otherwise, let type be STRING.
        let type_ = descriptor.type_;

        // b. If desc has a [[Values]] field, let values be desc.[[Values]]. Otherwise, let values be EMPTY.
        let values = descriptor.values;

        // c. Let value be ? GetOption(options, desc.[[Property]], type, values, undefined).
        let value = get_option(vm, &options, descriptor.property, type_, values, OptionDefault::Empty)?;
        let mut locale_key = None;

        // d. If value is not undefined, then
        if !value.is_undefined() {
            // i. Set value to ! ToString(value).
            let value_string = value.to_utf16_string(vm).must();
            let value_string_view = Utf16View::of_string(&value_string);

            // ii. If value cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
            if !value_string_view.has_ascii_storage() || !unicode::is_type_identifier(value_string_view) {
                return throw_option_is_not_valid_value(vm, value_string_view, descriptor.property);
            }

            locale_key = Some(LocaleKey::String(value_string));
        }

        // e. Let key be desc.[[Key]].
        let key = descriptor.key;

        // f. Set opt.[[<key>]] to value.
        if *descriptor.property == vm.names.hour12 {
            opt.hour12 = value;
        } else {
            *find_key_in_locale_options(&mut opt, key) = locale_key;
        }
    }

    // 7. If modifyResolutionOptions is present, perform ! modifyResolutionOptions(opt).
    if let Some(modify_resolution_options) = modify_resolution_options {
        modify_resolution_options(&mut opt);
    }

    // 8. Let resolution be ResolveLocale(constructor.[[AvailableLocales]], requestedLocales, opt, constructor.[[RelevantExtensionKeys]], localeData).
    let resolution = resolve_locale(vm, &requested_locales, &opt, object.relevant_extension_keys())?;

    // 9. Return the Record { [[Options]]: options, [[ResolvedLocale]]: resolution, [[ResolutionOptions]]: opt }.
    Ok(ResolvedOptions {
        options,
        resolved_locale: resolution,
        resolution_options: opt,
    })
}

// 9.2.9 FilterLocales ( availableLocales, requestedLocales, options ), https://tc39.es/ecma402/#sec-lookupsupportedlocales
pub fn filter_locales(
    vm: &Vm,
    requested_locales: &[Utf16String],
    options_value: Value,
) -> ThrowCompletionOr<Gc<Array>> {
    let realm = vm.current_realm().expect("FilterLocales runs in a realm");

    // 1. Set options to ? CoerceOptionsToObject(options).
    let options = coerce_options_to_object(vm, options_value)?;

    // 2. Let matcher be ? GetOption(options, "localeMatcher", string, « "lookup", "best fit" », "best fit").
    let matcher = get_option(
        vm,
        &options,
        &vm.names.localeMatcher,
        OptionType::String,
        &["lookup", "best fit"],
        OptionDefault::string("best fit"),
    )?;

    // 3. Let subset be a new empty List.
    let mut subset: Vec<Utf16String> = Vec::new();

    // 4. For each element locale of requestedLocales, do
    for locale in requested_locales {
        let locale_list = core::slice::from_ref(locale);

        // a. If matcher is "lookup", then
        let match_ = if matcher.as_string().utf16_string_view() == "lookup" {
            // i. Let match be LookupMatchingLocaleByPrefix(availableLocales, « locale »).
            lookup_matching_locale_by_prefix(locale_list)
        }
        // b. Else,
        else {
            // i. Let match be LookupMatchingLocaleByBestFit(availableLocales, « locale »).
            lookup_matching_locale_by_best_fit(locale_list)
        };

        // c. If match is not undefined, append locale to subset.
        if match_.is_some() {
            subset.push(locale.clone());
        }
    }

    // 5. Return CreateArrayFromList(subset).
    Ok(create_array_from_string_list(vm, realm, &subset))
}

// 9.2.11 CoerceOptionsToObject ( options ), https://tc39.es/ecma402/#sec-coerceoptionstoobject
pub fn coerce_options_to_object(vm: &Vm, options: Value) -> ThrowCompletionOr<Gc<Object>> {
    let realm = vm.current_realm().expect("CoerceOptionsToObject runs in a realm");

    // 1. If options is undefined, then
    if options.is_undefined() {
        // a. Return OrdinaryObjectCreate(null).
        return Ok(Object::create(vm, realm, None));
    }

    // 2. Return ? ToObject(options).
    options.to_object(vm)
}

// NOTE: 9.2.12 GetOption has been removed and is being pulled in from ECMA-262 in the Temporal proposal.

// 9.2.13 GetBooleanOrStringNumberFormatOption ( options, property, stringValues, fallback ), https://tc39.es/ecma402/#sec-getbooleanorstringnumberformatoption
pub fn get_boolean_or_string_number_format_option(
    vm: &Vm,
    options: &Object,
    property: &PropertyKey,
    string_values: &[&'static str],
    fallback: StringOrBoolean,
) -> ThrowCompletionOr<StringOrBoolean> {
    // 1. Let value be ? Get(options, property).
    let value = options.get(vm, property)?;

    // 2. If value is undefined, return fallback.
    if value.is_undefined() {
        return Ok(fallback);
    }

    // 3. If value is true, return true.
    if value.is_boolean() && value.as_bool() {
        return Ok(StringOrBoolean::Boolean(true));
    }

    // 4. If ToBoolean(value) is false, return false.
    if !value.to_boolean() {
        return Ok(StringOrBoolean::Boolean(false));
    }

    // 5. Let value be ? ToString(value).
    let value_string = value.to_utf16_string(vm)?;

    // 6. If stringValues does not contain value, throw a RangeError exception.
    let value_string_view = Utf16View::of_string(&value_string);
    let Some(allowed_value) = string_values
        .iter()
        .find(|allowed_value| value_string_view == **allowed_value)
    else {
        return throw_option_is_not_valid_value(vm, value_string_view, property);
    };

    // 7. Return value.
    Ok(StringOrBoolean::String(allowed_value))
}

// 9.2.14 DefaultNumberOption ( value, minimum, maximum, fallback ), https://tc39.es/ecma402/#sec-defaultnumberoption
pub fn default_number_option(
    vm: &Vm,
    value: Value,
    minimum: i32,
    maximum: i32,
    fallback: Option<i32>,
) -> ThrowCompletionOr<Option<i32>> {
    // 1. If value is undefined, return fallback.
    if value.is_undefined() {
        return Ok(fallback);
    }

    // 2. Set value to ? ToNumber(value).
    let value = value.to_number(vm)?;

    // 3. If value is NaN or less than minimum or greater than maximum, throw a RangeError exception.
    if value.is_nan() || value.as_f64() < f64::from(minimum) || value.as_f64() > f64::from(maximum) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::IntlNumberIsNaNOrOutOfRange,
            &[&value, &minimum, &maximum],
        );
    }

    // 4. Return floor(value).
    Ok(Some(value.as_f64().floor() as i32))
}

// 9.2.15 GetNumberOption ( options, property, minimum, maximum, fallback ), https://tc39.es/ecma402/#sec-getnumberoption
pub fn get_number_option(
    vm: &Vm,
    options: &Object,
    property: &PropertyKey,
    minimum: i32,
    maximum: i32,
    fallback: Option<i32>,
) -> ThrowCompletionOr<Option<i32>> {
    // 1. Assert: Type(options) is Object.

    // 2. Let value be ? Get(options, property).
    let value = options.get(vm, property)?;

    // 3. Return ? DefaultNumberOption(value, minimum, maximum, fallback).
    default_number_option(vm, value, minimum, maximum, fallback)
}
