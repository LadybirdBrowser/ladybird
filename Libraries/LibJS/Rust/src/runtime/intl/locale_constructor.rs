/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{OptionDefault, OptionType, get_option, ordinary_create_from_constructor_of};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intl::abstract_operations::{
    canonicalize_unicode_locale_id, coerce_options_to_object, insert_unicode_extension_and_canonicalize,
    is_well_formed_language_tag, throw_invalid_language_tag, throw_option_is_not_valid_value, to_ascii_lowercase,
};
use crate::runtime::intl::locale::{Locale, as_locale, get_locale_variants, weekday_to_u_value};
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::unicode::intl::{self as unicode, Keyword, LocaleID};
use crate::utf16::Utf16View;

#[derive(Default)]
struct LocaleAndKeys {
    locale: Utf16String,
    ca: Option<Utf16String>,
    co: Option<Utf16String>,
    fw: Option<Utf16String>,
    hc: Option<Utf16String>,
    kf: Option<Utf16String>,
    kn: Option<Utf16String>,
    nu: Option<Utf16String>,
}

struct LocaleOptionsAndKeys {
    ca: Option<Utf16String>,
    co: Option<Utf16String>,
    fw: Option<Utf16String>,
    hc: Option<Utf16String>,
    kf: Option<Utf16String>,
    kn: Option<Utf16String>,
    nu: Option<Utf16String>,
}

type Validator = fn(Utf16View<'_>) -> bool;

// NOTE: This is not an AO in the spec. This just serves to abstract very similar steps in UpdateLanguageId and the Intl.Locale constructor.
fn get_string_option(
    vm: &Vm,
    options: &Object,
    property: &PropertyKey,
    validator: Option<Validator>,
    values: &[&str],
    fallback: Option<Utf16String>,
) -> ThrowCompletionOr<Option<Utf16String>> {
    let option = get_option(vm, options, property, OptionType::String, values, OptionDefault::Empty)?;
    if option.is_undefined() {
        return Ok(fallback);
    }

    let option_string = option.as_string().utf16_string();
    let option_string_view = Utf16View::of_string(&option_string);
    if !option_string_view.has_ascii_storage() {
        return throw_option_is_not_valid_value(vm, option_string_view, property);
    }

    if let Some(validator) = validator
        && !validator(option_string_view)
    {
        return throw_option_is_not_valid_value(vm, option_string_view, property);
    }

    Ok(Some(option_string))
}

// 15.1.2 UpdateLanguageId ( tag, options ), https://tc39.es/ecma402/#sec-updatelanguageid
fn update_language_id(vm: &Vm, tag: Utf16View<'_>, options: &Object) -> ThrowCompletionOr<Utf16String> {
    let locale_id = unicode::parse_unicode_locale_id(tag).expect("the tag is a well-formed language tag");

    // 1. Let baseName be GetLocaleBaseName(tag).
    let base_name = &locale_id.language_id;

    // 2. Let language be ? GetOption(options, "language", STRING, EMPTY, GetLocaleLanguage(baseName)).
    // 3. If language cannot be matched by the unicode_language_subtag Unicode locale nonterminal, throw a RangeError exception.
    let language = get_string_option(
        vm,
        options,
        &vm.names.language,
        Some(unicode::is_unicode_language_subtag),
        &[],
        Some(
            base_name
                .language
                .clone()
                .expect("a well-formed language tag has a language"),
        ),
    )?;

    // 4. Let script be ? GetOption(options, "script", STRING, EMPTY, GetLocaleScript(baseName)).
    // 5. If script is not undefined, then
    //     a. If script cannot be matched by the unicode_script_subtag Unicode locale nonterminal, throw a RangeError exception.
    let script = get_string_option(
        vm,
        options,
        &vm.names.script,
        Some(unicode::is_unicode_script_subtag),
        &[],
        base_name.script.clone(),
    )?;

    // 6. Let region be ? GetOption(options, "region", STRING, EMPTY, GetLocaleRegion(baseName)).
    // 7. If region is not undefined, then
    //     a. If region cannot be matched by the unicode_region_subtag Unicode locale nonterminal, throw a RangeError exception.
    let region = get_string_option(
        vm,
        options,
        &vm.names.region,
        Some(unicode::is_unicode_region_subtag),
        &[],
        base_name.region.clone(),
    )?;

    // 8. Let variants be ? GetOption(options, "variants", STRING, EMPTY, GetLocaleVariants(baseName)).
    let variants = get_string_option(
        vm,
        options,
        &vm.names.variants,
        None,
        &[],
        get_locale_variants(&locale_id),
    )?;
    let mut variant_subtags: Vec<Utf16String> = Vec::new();

    // 9. If variants is not undefined, then
    if let Some(variants) = &variants {
        let invalid_variants =
            || throw_option_is_not_valid_value(vm, Utf16View::of_string(variants), &vm.names.variants);

        // a. If variants is the empty String, throw a RangeError exception.
        if variants.is_empty() {
            return invalid_variants();
        }

        // b. Let lowerVariants be the ASCII-lowercase of variants.
        let lower_variants = to_ascii_lowercase(Utf16View::of_string(variants));
        let lower_variants_view = Utf16View::of_string(&lower_variants);

        // c. Let variantSubtags be StringSplitToList(lowerVariants, "-").
        let mut start = 0;
        for index in 0..=lower_variants_view.length_in_code_units() {
            if index == lower_variants_view.length_in_code_units()
                || lower_variants_view.code_unit_at(index) == u16::from(b'-')
            {
                variant_subtags.push(
                    lower_variants_view
                        .substring_view(start, index - start)
                        .to_utf16_string(),
                );
                start = index + 1;
            }
        }

        let mut seen_variants: Vec<Utf16String> = Vec::new();
        let mut has_duplicate_variant = false;

        // d. For each element variant of variantSubtags, do
        for variant in &variant_subtags {
            if seen_variants.contains(variant) {
                has_duplicate_variant = true;
            } else {
                seen_variants.push(variant.clone());
            }

            // i. If variant cannot be matched by the unicode_variant_subtag Unicode locale nonterminal, throw a RangeError exception.
            if !unicode::is_unicode_variant_subtag(Utf16View::of_string(variant)) {
                return invalid_variants();
            }
        }

        // e. If variantSubtags contains any duplicate elements, throw a RangeError exception.
        if has_duplicate_variant {
            return invalid_variants();
        }
    }

    // 10. Let allExtensions be the suffix of tag following baseName.
    let LocaleID {
        extensions,
        private_use_extensions,
        ..
    } = locale_id;

    // 11. Let newTag be language.
    let mut new_tag = LocaleID::default();
    new_tag.language_id.language = language;

    // 12. If script is not undefined, set newTag to the string-concatenation of newTag, "-", and script.
    new_tag.language_id.script = script;

    // 13. If region is not undefined, set newTag to the string-concatenation of newTag, "-", and region.
    new_tag.language_id.region = region;

    // 14. If variants is not undefined, set newTag to the string-concatenation of newTag, "-", and variants.
    new_tag.language_id.variants = variant_subtags;

    // 15. Set newTag to the string-concatenation of newTag and allExtensions.
    new_tag.extensions = extensions;
    new_tag.private_use_extensions = private_use_extensions;

    // 16. Return newTag.
    Ok(new_tag.to_utf16_string())
}

fn option_field_from_key<'a>(value: &'a mut LocaleOptionsAndKeys, key: &str) -> &'a mut Option<Utf16String> {
    match key {
        "ca" => &mut value.ca,
        "co" => &mut value.co,
        "fw" => &mut value.fw,
        "hc" => &mut value.hc,
        "kf" => &mut value.kf,
        "kn" => &mut value.kn,
        "nu" => &mut value.nu,
        _ => unreachable!("{key} is not a locale extension key"),
    }
}

fn result_field_from_key<'a>(value: &'a mut LocaleAndKeys, key: &str) -> &'a mut Option<Utf16String> {
    match key {
        "ca" => &mut value.ca,
        "co" => &mut value.co,
        "fw" => &mut value.fw,
        "hc" => &mut value.hc,
        "kf" => &mut value.kf,
        "kn" => &mut value.kn,
        "nu" => &mut value.nu,
        _ => unreachable!("{key} is not a locale extension key"),
    }
}

// 15.1.3 MakeLocaleRecord ( tag, options, localeExtensionKeys ), https://tc39.es/ecma402/#sec-makelocalerecord
fn make_locale_record(
    vm: &Vm,
    tag: Utf16View<'_>,
    mut options: LocaleOptionsAndKeys,
    locale_extension_keys: &[&str],
) -> ThrowCompletionOr<LocaleAndKeys> {
    let mut locale_id = unicode::parse_unicode_locale_id(tag).expect("the tag is a well-formed language tag");

    let mut attributes: Vec<Utf16String> = Vec::new();
    let mut keywords: Vec<Keyword> = Vec::new();

    // 1. If tag contains a substring that is a Unicode locale extension sequence, then
    if let Some(components) = locale_id.first_locale_extension_mut() {
        // a. Let extension be the String value consisting of the substring of the Unicode locale extension sequence within tag.
        // b. Let components be UnicodeExtensionComponents(extension).

        // c. Let attributes be components.[[Attributes]].
        attributes = core::mem::take(&mut components.attributes);

        // d. Let keywords be components.[[Keywords]].
        keywords = core::mem::take(&mut components.keywords);
    }
    // 2. Else,
    //     a. Let attributes be a new empty List.
    //     b. Let keywords be a new empty List.

    // 3. Let result be a new Record.
    let mut result = LocaleAndKeys::default();

    // 4. For each element key of localeExtensionKeys, do
    for &key in locale_extension_keys {
        let mut entry_index = None;
        let mut value = None;

        // a. If keywords contains an element whose [[Key]] is key, then
        if let Some(index) = keywords
            .iter()
            .position(|keyword| Utf16View::of_string(&keyword.key) == key)
        {
            // i. Let entry be the element of keywords whose [[Key]] is key.
            entry_index = Some(index);

            // ii. Let value be entry.[[Value]].
            value = Some(keywords[index].value.clone());
        }
        // b. Else,
        //     i. Let entry be empty.
        //     ii. Let value be undefined.

        // c. Assert: options has a field [[<key>]].
        // d. Let overrideValue be options.[[<key>]].
        let override_value = option_field_from_key(&mut options, key);

        // e. If overrideValue is not undefined, then
        if let Some(override_value) = override_value {
            // i. Set value to CanonicalizeUValue(key, overrideValue).
            let canonicalized = unicode::canonicalize_unicode_extension_values(
                Utf16View::Ascii(key.as_bytes()),
                Utf16View::of_string(override_value),
            );
            value = Some(canonicalized.clone());

            // ii. If entry is not empty, then
            if let Some(entry_index) = entry_index {
                // 1. Set entry.[[Value]] to value.
                keywords[entry_index].value = canonicalized;
            }
            // iii. Else,
            else {
                // 1. Append the Record { [[Key]]: key, [[Value]]: value } to keywords.
                keywords.push(Keyword {
                    key: Utf16String::from_utf8(key),
                    value: canonicalized,
                });
            }
        }

        // f. Set result.[[<key>]] to value.
        if let Some(value) = value {
            *result_field_from_key(&mut result, key) = Some(value);
        }
    }

    // 5. Let locale be the String value that is tag with any Unicode locale extension sequences removed.
    locale_id.remove_locale_extensions();
    let locale = locale_id.to_utf16_string();

    // 6. If attributes is not empty or keywords is not empty, then
    if !attributes.is_empty() || !keywords.is_empty() {
        // a. Set result.[[locale]] to InsertUnicodeExtensionAndCanonicalize(locale, attributes, keywords).
        result.locale = insert_unicode_extension_and_canonicalize(vm, locale_id, attributes, keywords)?;
    }
    // 7. Else,
    else {
        // a. Set result.[[locale]] to CanonicalizeUnicodeLocaleId(locale).
        result.locale = canonicalize_unicode_locale_id(vm, Utf16View::of_string(&locale))?;
    }

    // 8. Return result.
    Ok(result)
}

#[repr(C)]
#[derive(Trace)]
pub struct LocaleConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    LocaleConstructor,
    initialize: LocaleConstructor::initialize,
    call: LocaleConstructor::call,
    construct: LocaleConstructor::construct
);

impl LocaleConstructor {
    // 15.1 The Intl.Locale Constructor, https://tc39.es/ecma402/#sec-intl-locale-constructor
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<LocaleConstructor> {
        realm.create_object(
            vm,
            LocaleConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Locale.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 14.2.1 Intl.Locale.prototype, https://tc39.es/ecma402/#sec-Intl.Locale.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().intl_locale_prototype(vm)),
            PropertyAttributes::new(0),
        );
        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 15.1.1 Intl.Locale ( tag [ , options ] ), https://tc39.es/ecma402/#sec-Intl.Locale
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Intl.Locale"],
        )
    }

    // 15.1.1 Intl.Locale ( tag [ , options ] ), https://tc39.es/ecma402/#sec-Intl.Locale
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let names = &vm.names;

        let tag_value = vm.argument(0);
        let options_value = vm.argument(1);

        // 2. Let localeExtensionKeys be %Intl.Locale%.[[LocaleExtensionKeys]].
        let locale_extension_keys = Locale::locale_extension_keys();

        // 3. Let internalSlotsList be « [[InitializedLocale]], [[Locale]], [[Calendar]], [[Collation]], [[FirstDayOfWeek]], [[HourCycle]], [[NumberingSystem]] ».
        // 4. If localeExtensionKeys contains "kf", then
        //     a. Append [[CaseFirst]] to internalSlotsList.
        // 5. If localeExtensionKeys contains "kn", then
        //     a. Append [[Numeric]] to internalSlotsList.
        // 6. Let locale be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.Locale.prototype%", internalSlotsList).
        let locale = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::intl_locale_prototype,
            |prototype| Locale::new(vm, prototype),
        )?;

        // 7. If tag is not a String and tag is not an Object, throw a TypeError exception.
        if !tag_value.is_string() && !tag_value.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOrString, &[&"tag"]);
        }

        // 8. If tag is an Object and tag has an [[InitializedLocale]] internal slot, then
        //     a. Let tag be tag.[[Locale]].
        let (mut tag, tag_is_canonicalized) = if let Some(locale_tag) = as_locale(tag_value) {
            (locale_tag.locale(), false)
        }
        // 9. Else,
        else {
            // a. Let tag be ? ToString(tag).
            let tag_string = tag_value.to_utf16_string(vm)?;

            // 11. If IsWellFormedLanguageTag(tag) is false, throw a RangeError exception.
            if !is_well_formed_language_tag(Utf16View::of_string(&tag_string)) {
                return throw_invalid_language_tag(vm, Utf16View::of_string(&tag_string));
            }

            // 13. Set tag to CanonicalizeUnicodeLocaleId(tag).
            (
                canonicalize_unicode_locale_id(vm, Utf16View::of_string(&tag_string))?,
                true,
            )
        };

        // 10. Set options to ? CoerceOptionsToObject(options).
        let options = coerce_options_to_object(vm, options_value)?;

        // 11. If IsWellFormedLanguageTag(tag) is false, throw a RangeError exception.
        if !tag_is_canonicalized && !is_well_formed_language_tag(Utf16View::of_string(&tag)) {
            return throw_invalid_language_tag(vm, Utf16View::of_string(&tag));
        }

        // 12. NOTE: Because LanguageId canonicalization can alter tag in arbitrary ways according to Alias Rules from
        //     supplementalMetadata.xml, it is necessary to perform such canonicalization before applying overrides from
        //     options.

        // 13. Set tag to CanonicalizeUnicodeLocaleId(tag).
        if !tag_is_canonicalized {
            tag = canonicalize_unicode_locale_id(vm, Utf16View::of_string(&tag))?;
        }

        // 14. Set tag to ? UpdateLanguageId(tag, options).
        tag = update_language_id(vm, Utf16View::of_string(&tag), &options)?;

        // 15. Let opt be a new Record.
        // 16. Let calendar be ? GetOption(options, "calendar", STRING, EMPTY, undefined).
        // 17. If calendar is not undefined, then
        //     a. If calendar cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
        // 18. Set opt.[[ca]] to calendar.
        let ca = get_string_option(
            vm,
            &options,
            &names.calendar,
            Some(unicode::is_type_identifier),
            &[],
            None,
        )?;

        // 19. Let collation be ? GetOption(options, "collation", STRING, EMPTY, undefined).
        // 20. If collation is not undefined, then
        //     a. If collation cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
        // 21. Set opt.[[co]] to collation.
        let co = get_string_option(
            vm,
            &options,
            &names.collation,
            Some(unicode::is_type_identifier),
            &[],
            None,
        )?;

        // 22. Let fw be ? GetOption(options, "firstDayOfWeek", STRING, EMPTY, undefined).
        let mut first_day_of_week = get_string_option(vm, &options, &names.firstDayOfWeek, None, &[], None)?;

        // 23. If fw is not undefined, then
        if let Some(first_day_of_week_string) = &first_day_of_week {
            // a. Set fw to WeekdayToUValue(fw).
            let first_day_of_week_u_value = weekday_to_u_value(Utf16View::of_string(first_day_of_week_string));

            // b. If fw cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
            if !unicode::is_type_identifier(Utf16View::of_string(&first_day_of_week_u_value)) {
                return throw_option_is_not_valid_value(
                    vm,
                    Utf16View::of_string(&first_day_of_week_u_value),
                    &names.firstDayOfWeek,
                );
            }

            first_day_of_week = Some(first_day_of_week_u_value);
        }

        // 24. Set opt.[[fw]] to fw.
        let fw = first_day_of_week;

        // 25. Let hc be ? GetOption(options, "hourCycle", STRING, « "h11", "h12", "h23", "h24" », undefined).
        // 26. Set opt.[[hc]] to hc.
        let hc = get_string_option(
            vm,
            &options,
            &names.hourCycle,
            None,
            &["h11", "h12", "h23", "h24"],
            None,
        )?;

        // 27. Let kf be ? GetOption(options, "caseFirst", STRING, « "upper", "lower", "false" », undefined).
        // 28. Set opt.[[kf]] to kf.
        let kf = get_string_option(vm, &options, &names.caseFirst, None, &["upper", "lower", "false"], None)?;

        // 29. Let kn be ? GetOption(options, "numeric", BOOLEAN, EMPTY, undefined).
        let kn = get_option(
            vm,
            &options,
            &names.numeric,
            OptionType::Boolean,
            &[],
            OptionDefault::Empty,
        )?;

        // 30. If kn is not undefined, set kn to ! ToString(kn).
        // 31. Set opt.[[kn]] to kn.
        let kn = (!kn.is_undefined()).then(|| Utf16String::from_utf8(if kn.as_bool() { "true" } else { "false" }));

        // 32. Let numberingSystem be ? GetOption(options, "numberingSystem", STRING, EMPTY, undefined).
        // 33. If numberingSystem is not undefined, then
        //     a. If numberingSystem cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
        // 34. Set opt.[[nu]] to numberingSystem.
        let nu = get_string_option(
            vm,
            &options,
            &names.numberingSystem,
            Some(unicode::is_type_identifier),
            &[],
            None,
        )?;

        let opt = LocaleOptionsAndKeys {
            ca,
            co,
            fw,
            hc,
            kf,
            kn,
            nu,
        };

        // 35. Let r be MakeLocaleRecord(tag, opt, localeExtensionKeys).
        let result = make_locale_record(vm, Utf16View::of_string(&tag), opt, locale_extension_keys)?;

        // 36. Set locale.[[Locale]] to r.[[locale]].
        locale.set_locale(result.locale);

        // 37. Set locale.[[Calendar]] to r.[[ca]].
        if let Some(calendar) = result.ca {
            locale.set_calendar(calendar);
        }

        // 38. Set locale.[[Collation]] to r.[[co]].
        if let Some(collation) = result.co {
            locale.set_collation(collation);
        }

        // 39. Set locale.[[FirstDayOfWeek]] to r.[[fw]].
        if let Some(first_day_of_week) = result.fw {
            locale.set_first_day_of_week(first_day_of_week);
        }

        // 40. Set locale.[[HourCycle]] to r.[[hc]].
        if let Some(hour_cycle) = result.hc {
            locale.set_hour_cycle(hour_cycle);
        }

        // 41. If localeExtensionKeys contains "kf", then
        if locale_extension_keys.contains(&"kf") {
            // a. Set locale.[[CaseFirst]] to r.[[kf]].
            if let Some(case_first) = result.kf {
                locale.set_case_first(case_first);
            }
        }

        // 42. If localeExtensionKeys contains "kn", then
        if locale_extension_keys.contains(&"kn") {
            // a. If SameValue(r.[[kn]], "true") is true or r.[[kn]] is the empty String, then
            if result
                .kn
                .as_ref()
                .is_some_and(|kn| Utf16View::of_string(kn) == "true" || kn.is_empty())
            {
                // i. Set locale.[[Numeric]] to true.
                locale.set_numeric(true);
            }
            // b. Else,
            else {
                // i. Set locale.[[Numeric]] to false.
                locale.set_numeric(false);
            }
        }

        // 43. Set locale.[[NumberingSystem]] to r.[[nu]].
        if let Some(numbering_system) = result.nu {
            locale.set_numbering_system(numbering_system);
        }

        // 44. Return locale.
        Ok(locale.upcast())
    }
}
