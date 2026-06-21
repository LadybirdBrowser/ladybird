/*
 * Copyright (c) 2021-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/Locale.h>
#include <LibJS/Runtime/Intl/LocaleConstructor.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(LocaleConstructor);

struct LocaleAndKeys {
    Utf16String locale;
    Optional<Utf16String> ca;
    Optional<Utf16String> co;
    Optional<Utf16String> fw;
    Optional<Utf16String> hc;
    Optional<Utf16String> kf;
    Optional<Utf16String> kn;
    Optional<Utf16String> nu;
};

struct LocaleOptionsAndKeys {
    Optional<Utf16String> ca;
    Optional<Utf16String> co;
    Optional<Utf16String> fw;
    Optional<Utf16String> hc;
    Optional<Utf16String> kf;
    Optional<Utf16String> kn;
    Optional<Utf16String> nu;
};

static bool is_unicode_language_subtag(Utf16View subtag)
{
    return Unicode::is_unicode_language_subtag(subtag);
}

static bool is_unicode_script_subtag(Utf16View subtag)
{
    return Unicode::is_unicode_script_subtag(subtag);
}

static bool is_unicode_region_subtag(Utf16View subtag)
{
    return Unicode::is_unicode_region_subtag(subtag);
}

static bool is_type_identifier(Utf16View identifier)
{
    return Unicode::is_type_identifier(identifier);
}

// NOTE: This is not an AO in the spec. This just serves to abstract very similar steps in UpdateLanguageId and the Intl.Locale constructor.
static ThrowCompletionOr<Optional<Utf16String>> get_string_option(VM& vm, Object const& options, PropertyKey const& property, Function<bool(Utf16View)> validator, ReadonlySpan<StringView> values = {}, Optional<Utf16String> const& fallback = {})
{
    auto option = TRY(get_option(vm, options, property, OptionType::String, values, Empty {}));
    if (option.is_undefined())
        return fallback;

    auto option_string_view = option.as_string().utf16_string_view();
    if (!option_string_view.has_ascii_storage())
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, option, property);

    if (validator && !validator(option_string_view))
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, option, property);

    return Utf16String::from_utf16(option_string_view);
}

// 15.1.2 UpdateLanguageId ( tag, options ), https://tc39.es/ecma402/#sec-updatelanguageid
static ThrowCompletionOr<Utf16String> update_language_id(VM& vm, Utf16View tag, Object const& options)
{
    auto locale_id = Unicode::parse_unicode_locale_id(tag);
    VERIFY(locale_id.has_value());

    // 1. Let baseName be GetLocaleBaseName(tag).
    auto const& base_name = locale_id->language_id;

    // 2. Let language be ? GetOption(options, "language", STRING, EMPTY, GetLocaleLanguage(baseName)).
    // 3. If language cannot be matched by the unicode_language_subtag Unicode locale nonterminal, throw a RangeError exception.
    auto language = TRY(get_string_option(vm, options, vm.names.language, is_unicode_language_subtag, {}, *base_name.language));

    // 4. Let script be ? GetOption(options, "script", STRING, EMPTY, GetLocaleScript(baseName)).
    // 5. If script is not undefined, then
    //     a. If script cannot be matched by the unicode_script_subtag Unicode locale nonterminal, throw a RangeError exception.
    auto script = TRY(get_string_option(vm, options, vm.names.script, is_unicode_script_subtag, {}, base_name.script));

    // 6. Let region be ? GetOption(options, "region", STRING, EMPTY, GetLocaleRegion(baseName)).
    // 7. If region is not undefined, then
    //     a. If region cannot be matched by the unicode_region_subtag Unicode locale nonterminal, throw a RangeError exception.
    auto region = TRY(get_string_option(vm, options, vm.names.region, is_unicode_region_subtag, {}, base_name.region));

    // 8. Let variants be ? GetOption(options, "variants", STRING, EMPTY, GetLocaleVariants(baseName)).
    auto variants = TRY(get_string_option(vm, options, vm.names.variants, nullptr, {}, get_locale_variants(*locale_id)));
    Vector<Utf16String> variant_subtags;

    // 9. If variants is not undefined, then
    if (variants.has_value()) {
        // a. If variants is the empty String, throw a RangeError exception.
        if (variants->is_empty())
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, *variants, vm.names.variants);

        // b. Let lowerVariants be the ASCII-lowercase of variants.
        auto lower_variants = variants->to_ascii_lowercase();

        // c. Let variantSubtags be StringSplitToList(lowerVariants, "-").
        lower_variants.utf16_view().for_each_split_view('-', SplitBehavior::KeepEmpty, [&](auto variant) {
            variant_subtags.append(Utf16String::from_utf16(variant));
            return IterationDecision::Continue;
        });

        HashTable<Utf16String> seen_variants;
        bool has_duplicate_variant = false;

        // d. For each element variant of variantSubtags, do
        for (auto const& variant : variant_subtags) {
            has_duplicate_variant |= seen_variants.set(variant) != HashSetResult::InsertedNewEntry;

            // i. If variant cannot be matched by the unicode_variant_subtag Unicode locale nonterminal, throw a RangeError exception.
            if (!Unicode::is_unicode_variant_subtag(variant))
                return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, *variants, vm.names.variants);
        }

        // e. If variantSubtags contains any duplicate elements, throw a RangeError exception.
        if (has_duplicate_variant)
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, *variants, vm.names.variants);
    }

    // 10. Let allExtensions be the suffix of tag following baseName.
    auto& extensions = locale_id->extensions;
    auto& private_use_extensions = locale_id->private_use_extensions;

    // 11. Let newTag be language.
    Unicode::LocaleID new_tag;
    new_tag.language_id.language = move(language);

    // 12. If script is not undefined, set newTag to the string-concatenation of newTag, "-", and script.
    new_tag.language_id.script = move(script);

    // 13. If region is not undefined, set newTag to the string-concatenation of newTag, "-", and region.
    new_tag.language_id.region = move(region);

    // 14. If variants is not undefined, set newTag to the string-concatenation of newTag, "-", and variants.
    new_tag.language_id.variants = move(variant_subtags);

    // 15. Set newTag to the string-concatenation of newTag and allExtensions.
    new_tag.extensions = move(extensions);
    new_tag.private_use_extensions = move(private_use_extensions);

    // 16. Return newTag.
    return new_tag.to_utf16_string();
}

// 15.1.3 MakeLocaleRecord ( tag, options, localeExtensionKeys ), https://tc39.es/ecma402/#sec-makelocalerecord
static LocaleAndKeys make_locale_record(Utf16View tag, LocaleOptionsAndKeys options, ReadonlySpan<Utf16View> locale_extension_keys)
{
    auto locale_id = Unicode::parse_unicode_locale_id(tag);
    VERIFY(locale_id.has_value());

    Vector<Utf16String> attributes;
    Vector<Unicode::Keyword> keywords;

    // 1. If tag contains a substring that is a Unicode locale extension sequence, then
    locale_id->for_each_extension_of_type<Unicode::LocaleExtension>([&](Unicode::LocaleExtension& components) {
        // a. Let extension be the String value consisting of the substring of the Unicode locale extension sequence within tag.
        // b. Let components be UnicodeExtensionComponents(extension).

        // c. Let attributes be components.[[Attributes]].
        attributes = move(components.attributes);

        // d. Let keywords be components.[[Keywords]].
        keywords = move(components.keywords);

        return IterationDecision::Break;
    });
    // 2. Else,
    //     a. Let attributes be a new empty List.
    //     b. Let keywords be a new empty List.

    auto option_field_from_key = [](LocaleOptionsAndKeys& value, Utf16View key) -> Optional<Utf16String>& {
        if (key == "ca"sv)
            return value.ca;
        if (key == "co"sv)
            return value.co;
        if (key == "fw"sv)
            return value.fw;
        if (key == "hc"sv)
            return value.hc;
        if (key == "kf"sv)
            return value.kf;
        if (key == "kn"sv)
            return value.kn;
        if (key == "nu"sv)
            return value.nu;
        VERIFY_NOT_REACHED();
    };

    auto result_field_from_key = [](LocaleAndKeys& value, Utf16View key) -> Optional<Utf16String>& {
        if (key == "ca"sv)
            return value.ca;
        if (key == "co"sv)
            return value.co;
        if (key == "fw"sv)
            return value.fw;
        if (key == "hc"sv)
            return value.hc;
        if (key == "kf"sv)
            return value.kf;
        if (key == "kn"sv)
            return value.kn;
        if (key == "nu"sv)
            return value.nu;
        VERIFY_NOT_REACHED();
    };

    // 3. Let result be a new Record.
    LocaleAndKeys result {};

    // 4. For each element key of localeExtensionKeys, do
    for (auto const& key : locale_extension_keys) {
        Unicode::Keyword* entry = nullptr;
        Optional<Utf16String> value;

        // a. If keywords contains an element whose [[Key]] is key, then
        if (auto it = keywords.find_if([&](auto const& k) { return k.key.utf16_view() == key; }); it != keywords.end()) {
            // i. Let entry be the element of keywords whose [[Key]] is key.
            entry = &(*it);

            // ii. Let value be entry.[[Value]].
            value = entry->value;
        }
        // b. Else,
        //     i. Let entry be empty.
        //     ii. Let value be undefined.

        // c. Assert: options has a field [[<key>]].
        // d. Let overrideValue be options.[[<key>]].
        auto const& override_value = option_field_from_key(options, key);

        // e. If overrideValue is not undefined, then
        if (override_value.has_value()) {
            // i. Set value to CanonicalizeUValue(key, overrideValue).
            value = Unicode::canonicalize_unicode_extension_values(key.bytes(), override_value->utf16_view());

            // ii. If entry is not empty, then
            if (entry != nullptr) {
                // 1. Set entry.[[Value]] to value.
                entry->value = *value;
            }
            // iii. Else,
            else {
                // 1. Append the Record { [[Key]]: key, [[Value]]: value } to keywords.
                keywords.empend(Utf16String::from_utf16(key), *value);
            }
        }

        // f. Set result.[[<key>]] to value.
        if (value.has_value())
            result_field_from_key(result, key) = *value;
    }

    // 5. Let locale be the String value that is tag with any Unicode locale extension sequences removed.
    locale_id->remove_extension_type<Unicode::LocaleExtension>();
    auto locale = locale_id->to_utf16_string();

    // 6. If attributes is not empty or keywords is not empty, then
    if (!attributes.is_empty() || !keywords.is_empty()) {
        // a. Set result.[[locale]] to InsertUnicodeExtensionAndCanonicalize(locale, attributes, keywords).
        result.locale = insert_unicode_extension_and_canonicalize(locale_id.release_value(), move(attributes), move(keywords));
    }
    // 7. Else,
    else {
        // a. Set result.[[locale]] to CanonicalizeUnicodeLocaleId(locale).
        result.locale = canonicalize_unicode_locale_id(locale);
    }

    // 8. Return result.
    return result;
}

// 15.1 The Intl.Locale Constructor, https://tc39.es/ecma402/#sec-intl-locale-constructor
LocaleConstructor::LocaleConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Locale.as_string(), realm.intrinsics().function_prototype())
{
}

void LocaleConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 14.2.1 Intl.Locale.prototype, https://tc39.es/ecma402/#sec-Intl.Locale.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().intl_locale_prototype(), 0);
    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 15.1.1 Intl.Locale ( tag [ , options ] ), https://tc39.es/ecma402/#sec-Intl.Locale
ThrowCompletionOr<Value> LocaleConstructor::call()
{
    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm().throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Intl.Locale");
}

// 15.1.1 Intl.Locale ( tag [ , options ] ), https://tc39.es/ecma402/#sec-Intl.Locale
ThrowCompletionOr<GC::Ref<Object>> LocaleConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto tag_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let localeExtensionKeys be %Intl.Locale%.[[LocaleExtensionKeys]].
    auto locale_extension_keys = Locale::locale_extension_keys();

    // 3. Let internalSlotsList be « [[InitializedLocale]], [[Locale]], [[Calendar]], [[Collation]], [[FirstDayOfWeek]], [[HourCycle]], [[NumberingSystem]] ».
    // 4. If localeExtensionKeys contains "kf", then
    //     a. Append [[CaseFirst]] to internalSlotsList.
    // 5. If localeExtensionKeys contains "kn", then
    //     a. Append [[Numeric]] to internalSlotsList.
    // 6. Let locale be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.Locale.prototype%", internalSlotsList).
    auto locale = TRY(ordinary_create_from_constructor<Locale>(vm, new_target, &Intrinsics::intl_locale_prototype));

    // 7. If tag is not a String and tag is not an Object, throw a TypeError exception.
    if (!tag_value.is_string() && !tag_value.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOrString, "tag"sv);

    Utf16String tag;
    bool tag_is_canonicalized = false;

    // 8. If tag is an Object and tag has an [[InitializedLocale]] internal slot, then
    //     a. Let tag be tag.[[Locale]].
    if (auto locale_tag = tag_value.as_if<Locale>()) {
        tag = locale_tag->locale();
    }
    // 9. Else,
    else {
        // a. Let tag be ? ToString(tag).
        auto tag_string = TRY(tag_value.to_utf16_string(vm));

        // 11. If IsWellFormedLanguageTag(tag) is false, throw a RangeError exception.
        if (!is_well_formed_language_tag(tag_string.utf16_view()))
            return vm.throw_completion<RangeError>(ErrorType::IntlInvalidLanguageTag, tag_string);

        // 13. Set tag to CanonicalizeUnicodeLocaleId(tag).
        tag = canonicalize_unicode_locale_id(tag_string.utf16_view());
        tag_is_canonicalized = true;
    }

    // 10. Set options to ? CoerceOptionsToObject(options).
    auto options = TRY(coerce_options_to_object(vm, options_value));

    // 11. If IsWellFormedLanguageTag(tag) is false, throw a RangeError exception.
    if (!tag_is_canonicalized && !is_well_formed_language_tag(tag.utf16_view()))
        return vm.throw_completion<RangeError>(ErrorType::IntlInvalidLanguageTag, tag);

    // 12. NOTE: Because LanguageId canonicalization can alter tag in arbitrary ways according to Alias Rules from
    //     supplementalMetadata.xml, it is necessary to perform such canonicalization before applying overrides from
    //     options.

    // 13. Set tag to CanonicalizeUnicodeLocaleId(tag).
    if (!tag_is_canonicalized) {
        tag = canonicalize_unicode_locale_id(tag.utf16_view());
    }

    // 14. Set tag to ? UpdateLanguageId(tag, options).
    tag = TRY(update_language_id(vm, tag.utf16_view(), options));

    // 15. Let opt be a new Record.
    LocaleOptionsAndKeys opt {};

    // 16. Let calendar be ? GetOption(options, "calendar", STRING, EMPTY, undefined).
    // 17. If calendar is not undefined, then
    //     a. If calendar cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
    // 18. Set opt.[[ca]] to calendar.
    opt.ca = TRY(get_string_option(vm, options, vm.names.calendar, is_type_identifier));

    // 19. Let collation be ? GetOption(options, "collation", STRING, EMPTY, undefined).
    // 20. If collation is not undefined, then
    //     a. If collation cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
    // 21. Set opt.[[co]] to collation.
    opt.co = TRY(get_string_option(vm, options, vm.names.collation, is_type_identifier));

    // 22. Let fw be ? GetOption(options, "firstDayOfWeek", STRING, EMPTY, undefined).
    auto first_day_of_week = TRY(get_string_option(vm, options, vm.names.firstDayOfWeek, nullptr));

    // 23. If fw is not undefined, then
    if (first_day_of_week.has_value()) {
        // a. Set fw to WeekdayToUValue(fw).
        first_day_of_week = Utf16String::from_utf16(weekday_to_u_value(first_day_of_week->utf16_view()));

        // b. If fw cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
        if (!Unicode::is_type_identifier(first_day_of_week->utf16_view()))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, *first_day_of_week, vm.names.firstDayOfWeek);
    }

    // 24. Set opt.[[fw]] to fw.
    opt.fw = move(first_day_of_week);

    // 25. Let hc be ? GetOption(options, "hourCycle", STRING, « "h11", "h12", "h23", "h24" », undefined).
    // 26. Set opt.[[hc]] to hc.
    opt.hc = TRY(get_string_option(vm, options, vm.names.hourCycle, nullptr, AK::Array { "h11"sv, "h12"sv, "h23"sv, "h24"sv }));

    // 27. Let kf be ? GetOption(options, "caseFirst", STRING, « "upper", "lower", "false" », undefined).
    // 28. Set opt.[[kf]] to kf.
    opt.kf = TRY(get_string_option(vm, options, vm.names.caseFirst, nullptr, AK::Array { "upper"sv, "lower"sv, "false"sv }));

    // 29. Let kn be ? GetOption(options, "numeric", BOOLEAN, EMPTY, undefined).
    auto kn = TRY(get_option(vm, options, vm.names.numeric, OptionType::Boolean, {}, Empty {}));

    // 30. If kn is not undefined, set kn to ! ToString(kn).
    // 31. Set opt.[[kn]] to kn.
    if (!kn.is_undefined())
        opt.kn = kn.as_bool() ? "true"_utf16 : "false"_utf16;

    // 32. Let numberingSystem be ? GetOption(options, "numberingSystem", STRING, EMPTY, undefined).
    // 33. If numberingSystem is not undefined, then
    //     a. If numberingSystem cannot be matched by the type Unicode locale nonterminal, throw a RangeError exception.
    // 34. Set opt.[[nu]] to numberingSystem.
    opt.nu = TRY(get_string_option(vm, options, vm.names.numberingSystem, is_type_identifier));

    // 35. Let r be MakeLocaleRecord(tag, opt, localeExtensionKeys).
    auto result = make_locale_record(tag.utf16_view(), move(opt), locale_extension_keys);

    // 36. Set locale.[[Locale]] to r.[[locale]].
    locale->set_locale(move(result.locale));

    // 37. Set locale.[[Calendar]] to r.[[ca]].
    if (result.ca.has_value())
        locale->set_calendar(result.ca.release_value());

    // 38. Set locale.[[Collation]] to r.[[co]].
    if (result.co.has_value())
        locale->set_collation(result.co.release_value());

    // 39. Set locale.[[FirstDayOfWeek]] to r.[[fw]].
    if (result.fw.has_value())
        locale->set_first_day_of_week(result.fw.release_value());

    // 40. Set locale.[[HourCycle]] to r.[[hc]].
    if (result.hc.has_value())
        locale->set_hour_cycle(result.hc.release_value());

    // 41. If localeExtensionKeys contains "kf", then
    if (locale_extension_keys.span().contains_slow(Utf16View { "kf"sv })) {
        // a. Set locale.[[CaseFirst]] to r.[[kf]].
        if (result.kf.has_value())
            locale->set_case_first(result.kf.release_value());
    }

    // 42. If localeExtensionKeys contains "kn", then
    if (locale_extension_keys.span().contains_slow(Utf16View { "kn"sv })) {
        // a. If SameValue(r.[[kn]], "true") is true or r.[[kn]] is the empty String, then
        if (result.kn.has_value() && (result.kn->utf16_view() == "true"sv || result.kn->is_empty())) {
            // i. Set locale.[[Numeric]] to true.
            locale->set_numeric(true);
        }
        // b. Else,
        else {
            // i. Set locale.[[Numeric]] to false.
            locale->set_numeric(false);
        }
    }

    // 43. Set locale.[[NumberingSystem]] to r.[[nu]].
    if (result.nu.has_value())
        locale->set_numbering_system(result.nu.release_value());

    // 44. Return locale.
    return locale;
}

}
