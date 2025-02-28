/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/Locale.h>
#include <LibJS/Runtime/Intl/LocaleConstructor.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(LocaleConstructor);

struct LocaleAndKeys {
    String locale;
    Optional<String> ca;
    Optional<String> co;
    Optional<String> fw;
    Optional<String> hc;
    Optional<String> kf;
    Optional<String> kn;
    Optional<String> nu;
};

// Note: This is not an AO in the spec. This just serves to abstract very similar steps in ApplyOptionsToTag and the Intl.Locale constructor.
static ThrowCompletionOr<Optional<String>> get_string_option(VM& vm, Object const& options, PropertyKey const& property, Function<bool(StringView)> validator, ReadonlySpan<StringView> values = {})
{
    auto option = TRY(get_option(vm, options, property, OptionType::String, values, Empty {}));
    if (option.is_undefined())
        return OptionalNone {};

    if (validator && !validator(option.as_string().utf8_string_view()))
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, option, property);

    return option.as_string().utf8_string();
}

// 15.1.2 ApplyOptionsToTag ( tag, options ), https://tc39.es/ecma402/#sec-apply-options-to-tag
static ThrowCompletionOr<String> apply_options_to_tag(VM& vm, StringView tag, Object const& options)
{
    // 1. Assert: Type(tag) is String.
    // 2. Assert: Type(options) is Object.

    // 3. If ! IsStructurallyValidLanguageTag(tag) is false, throw a RangeError exception.
    if (!is_structurally_valid_language_tag(tag))
        return vm.throw_completion<RangeError>(ErrorType::IntlInvalidLanguageTag, tag);

    // 4. Let language be ? GetOption(options, "language", string, empty, undefined).
    // 5. If language is not undefined, then
    //     a. If language does not match the unicode_language_subtag production, throw a RangeError exception.
    auto language = TRY(get_string_option(vm, options, vm.names.language, Unicode::is_unicode_language_subtag));

    // 6. Let script be ? GetOption(options, "script", string, empty, undefined).
    // 7. If script is not undefined, then
    //     a. If script does not match the unicode_script_subtag production, throw a RangeError exception.
    auto script = TRY(get_string_option(vm, options, vm.names.script, Unicode::is_unicode_script_subtag));

    // 8. Let region be ? GetOption(options, "region", string, empty, undefined).
    // 9. If region is not undefined, then
    //     a. If region does not match the unicode_region_subtag production, throw a RangeError exception.
    auto region = TRY(get_string_option(vm, options, vm.names.region, Unicode::is_unicode_region_subtag));

    // 10. Set tag to ! CanonicalizeUnicodeLocaleId(tag).
    auto canonicalized_tag = JS::Intl::canonicalize_unicode_locale_id(tag);

    // 11. Assert: tag matches the unicode_locale_id production.
    auto locale_id = Unicode::parse_unicode_locale_id(canonicalized_tag);
    VERIFY(locale_id.has_value());

    // 12. Let languageId be the substring of tag corresponding to the unicode_language_id production.
    auto& language_id = locale_id->language_id;

    // 13. If language is not undefined, then
    if (language.has_value()) {
        // a. Set languageId to languageId with the substring corresponding to the unicode_language_subtag production replaced by the string language.
        language_id.language = language.release_value();
    }

    // 14. If script is not undefined, then
    if (script.has_value()) {
        // a. If languageId does not contain a unicode_script_subtag production, then
        //     i. Set languageId to the string-concatenation of the unicode_language_subtag production of languageId, "-", script, and the rest of languageId.
        // b. Else,
        //     i. Set languageId to languageId with the substring corresponding to the unicode_script_subtag production replaced by the string script.
        language_id.script = script.release_value();
    }

    // 15. If region is not undefined, then
    if (region.has_value()) {
        // a. If languageId does not contain a unicode_region_subtag production, then
        //     i. Set languageId to the string-concatenation of the unicode_language_subtag production of languageId, the substring corresponding to "-"` and the `unicode_script_subtag` production if present, `"-", region, and the rest of languageId.
        // b. Else,
        //     i. Set languageId to languageId with the substring corresponding to the unicode_region_subtag production replaced by the string region.
        language_id.region = region.release_value();
    }

    // 16. Set tag to tag with the substring corresponding to the unicode_language_id production replaced by the string languageId.
    canonicalized_tag = locale_id->to_string();

    // 17. Return ! CanonicalizeUnicodeLocaleId(tag).
    return JS::Intl::canonicalize_unicode_locale_id(canonicalized_tag);
}

// 15.1.3 ApplyUnicodeExtensionToTag ( tag, options, relevantExtensionKeys ), https://tc39.es/ecma402/#sec-apply-unicode-extension-to-tag
static LocaleAndKeys apply_unicode_extension_to_tag(StringView tag, LocaleAndKeys options, ReadonlySpan<StringView> relevant_extension_keys)
{
    auto locale_id = Unicode::parse_unicode_locale_id(tag);
    VERIFY(locale_id.has_value());

    Vector<String> attributes;
    Vector<Unicode::Keyword> keywords;

    // 1. If tag contains a substring that is a Unicode locale extension sequence, then
    for (auto& extension : locale_id->extensions) {
        if (!extension.has<Unicode::LocaleExtension>())
            continue;

        // a. Let extension be the String value consisting of the substring of the Unicode locale extension sequence within tag.
        // b. Let components be ! UnicodeExtensionComponents(extension).
        auto& components = extension.get<Unicode::LocaleExtension>();
        // c. Let attributes be components.[[Attributes]].
        attributes = move(components.attributes);
        // d. Let keywords be components.[[Keywords]].
        keywords = move(components.keywords);

        break;
    }
    // 2. Else,
    //     a. Let attributes be a new empty List.
    //     b. Let keywords be a new empty List.

    auto field_from_key = [](LocaleAndKeys& value, StringView key) -> Optional<String>& {
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

    // 4. For each element key of relevantExtensionKeys, do
    for (auto const& key : relevant_extension_keys) {
        Unicode::Keyword* entry = nullptr;
        Optional<String> value;

        // a. If keywords contains an element whose [[Key]] is key, then
        if (auto it = keywords.find_if([&](auto const& k) { return key == k.key; }); it != keywords.end()) {
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
        auto override_value = field_from_key(options, key);

        // e. If overrideValue is not undefined, then
        if (override_value.has_value()) {
            // i. Set value to CanonicalizeUValue(key, overrideValue).
            value = Unicode::canonicalize_unicode_extension_values(key, *override_value);

            // ii. If entry is not empty, then
            if (entry != nullptr) {
                // 1. Set entry.[[Value]] to value.
                entry->value = *value;
            }
            // iii. Else,
            else {
                // 1. Append the Record { [[Key]]: key, [[Value]]: value } to keywords.
                keywords.empend(MUST(String::from_utf8(key)), *value);
            }
        }

        // f. Set result.[[<key>]] to value.
        field_from_key(result, key) = move(value);
    }

    // 5. Let locale be the String value that is tag with any Unicode locale extension sequences removed.
    locale_id->remove_extension_type<Unicode::LocaleExtension>();
    auto locale = locale_id->to_string();

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
// 1.2.3 Intl.Locale ( tag [ , options ] ), https://tc39.es/proposal-intl-locale-info/#sec-Intl.Locale
ThrowCompletionOr<GC::Ref<Object>> LocaleConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto tag_value = vm.argument(0);
    auto options_value = vm.argument(1);

    // 2. Let relevantExtensionKeys be %Locale%.[[RelevantExtensionKeys]].
    auto relevant_extension_keys = Locale::relevant_extension_keys();

    // 3. Let internalSlotsList be « [[InitializedLocale]], [[Locale]], [[Calendar]], [[Collation]], [[FirstDayOfWeek]], [[HourCycle]], [[NumberingSystem]] ».
    // 4. If relevantExtensionKeys contains "kf", then
    //     a. Append [[CaseFirst]] as the last element of internalSlotsList.
    // 5. If relevantExtensionKeys contains "kn", then
    //     a. Append [[Numeric]] as the last element of internalSlotsList.

    // 6. Let locale be ? OrdinaryCreateFromConstructor(NewTarget, "%Intl.Locale.prototype%", internalSlotsList).
    auto locale = TRY(ordinary_create_from_constructor<Locale>(vm, new_target, &Intrinsics::intl_locale_prototype));

    String tag;

    // 7. If Type(tag) is not String or Object, throw a TypeError exception.
    if (!tag_value.is_string() && !tag_value.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOrString, "tag"sv);

    // 8. If Type(tag) is Object and tag has an [[InitializedLocale]] internal slot, then
    if (tag_value.is_object() && is<Locale>(tag_value.as_object())) {
        // a. Let tag be tag.[[Locale]].
        auto const& tag_object = static_cast<Locale const&>(tag_value.as_object());
        tag = tag_object.locale();
    }
    // 9. Else,
    else {
        // a. Let tag be ? ToString(tag).
        tag = TRY(tag_value.to_string(vm));
    }

    // 10. Set options to ? CoerceOptionsToObject(options).
    auto* options = TRY(coerce_options_to_object(vm, options_value));

    // 11. Set tag to ? ApplyOptionsToTag(tag, options).
    tag = TRY(apply_options_to_tag(vm, tag, *options));

    // 12. Let opt be a new Record.
    LocaleAndKeys opt {};

    // 13. Let calendar be ? GetOption(options, "calendar", string, empty, undefined).
    // 14. If calendar is not undefined, then
    //     a. If calendar does not match the Unicode Locale Identifier type nonterminal, throw a RangeError exception.
    // 15. Set opt.[[ca]] to calendar.
    opt.ca = TRY(get_string_option(vm, *options, vm.names.calendar, Unicode::is_type_identifier));

    // 16. Let collation be ? GetOption(options, "collation", string, empty, undefined).
    // 17. If collation is not undefined, then
    //     a. If collation does not match the Unicode Locale Identifier type nonterminal, throw a RangeError exception.
    // 18. Set opt.[[co]] to collation.
    opt.co = TRY(get_string_option(vm, *options, vm.names.collation, Unicode::is_type_identifier));

    // 19. Let fw be ? Let fw be ? GetOption(options, "firstDayOfWeek", "string", undefined, undefined).
    auto first_day_of_week = TRY(get_string_option(vm, *options, vm.names.firstDayOfWeek, nullptr));

    // 20. If fw is not undefined, then
    if (first_day_of_week.has_value()) {
        // a. Set fw to !WeekdayToString(fw).
        first_day_of_week = MUST(String::from_utf8(weekday_to_string(*first_day_of_week)));

        // b. If fw does not match the type sequence (from UTS 35 Unicode Locale Identifier, section 3.2), throw a RangeError exception.
        if (!Unicode::is_type_identifier(*first_day_of_week))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, *first_day_of_week, vm.names.firstDayOfWeek);
    }

    // 21. Set opt.[[fw]] to firstDay.
    opt.fw = move(first_day_of_week);

    // 22. Let hc be ? GetOption(options, "hourCycle", string, « "h11", "h12", "h23", "h24" », undefined).
    // 23. Set opt.[[hc]] to hc.
    opt.hc = TRY(get_string_option(vm, *options, vm.names.hourCycle, nullptr, AK::Array { "h11"sv, "h12"sv, "h23"sv, "h24"sv }));

    // 24. Let kf be ? GetOption(options, "caseFirst", string, « "upper", "lower", "false" », undefined).
    // 25. Set opt.[[kf]] to kf.
    opt.kf = TRY(get_string_option(vm, *options, vm.names.caseFirst, nullptr, AK::Array { "upper"sv, "lower"sv, "false"sv }));

    // 26. Let kn be ? GetOption(options, "numeric", boolean, empty, undefined).
    auto kn = TRY(get_option(vm, *options, vm.names.numeric, OptionType::Boolean, {}, Empty {}));

    // 27. If kn is not undefined, set kn to ! ToString(kn).
    // 28. Set opt.[[kn]] to kn.
    if (!kn.is_undefined())
        opt.kn = TRY(kn.to_string(vm));

    // 29. Let numberingSystem be ? GetOption(options, "numberingSystem", string, empty, undefined).
    // 30. If numberingSystem is not undefined, then
    //     a. If numberingSystem does not match the Unicode Locale Identifier type nonterminal, throw a RangeError exception.
    // 31. Set opt.[[nu]] to numberingSystem.
    opt.nu = TRY(get_string_option(vm, *options, vm.names.numberingSystem, Unicode::is_type_identifier));

    // 32. Let r be ! ApplyUnicodeExtensionToTag(tag, opt, relevantExtensionKeys).
    auto result = apply_unicode_extension_to_tag(tag, move(opt), relevant_extension_keys);

    // 33. Set locale.[[Locale]] to r.[[locale]].
    locale->set_locale(move(result.locale));

    // 34. Set locale.[[Calendar]] to r.[[ca]].
    if (result.ca.has_value())
        locale->set_calendar(result.ca.release_value());

    // 35. Set locale.[[Collation]] to r.[[co]].
    if (result.co.has_value())
        locale->set_collation(result.co.release_value());

    // 36. Set locale.[[FirstDayOfWeek]] to r.[[fw]].
    if (result.fw.has_value())
        locale->set_first_day_of_week(result.fw.release_value());

    // 37. Set locale.[[HourCycle]] to r.[[hc]].
    if (result.hc.has_value())
        locale->set_hour_cycle(result.hc.release_value());

    // 38. If relevantExtensionKeys contains "kf", then
    if (relevant_extension_keys.span().contains_slow("kf"sv)) {
        // a. Set locale.[[CaseFirst]] to r.[[kf]].
        if (result.kf.has_value())
            locale->set_case_first(result.kf.release_value());
    }

    // 39. If relevantExtensionKeys contains "kn", then
    if (relevant_extension_keys.span().contains_slow("kn"sv)) {
        // a. If SameValue(r.[[kn]], "true") is true or r.[[kn]] is the empty String, then
        if (result.kn.has_value() && (result.kn == "true"sv || result.kn->is_empty())) {
            // i. Set locale.[[Numeric]] to true.
            locale->set_numeric(true);
        }
        // b. Else,
        else {
            // i. Set locale.[[Numeric]] to false.
            locale->set_numeric(false);
        }
    }

    // 40. Set locale.[[NumberingSystem]] to r.[[nu]].
    if (result.nu.has_value())
        locale->set_numbering_system(result.nu.release_value());

    // 41. Return locale.
    return locale;
}

}
