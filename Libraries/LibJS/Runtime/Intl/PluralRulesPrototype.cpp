/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/PluralRulesPrototype.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibUnicode/PluralRules.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(PluralRulesPrototype);

// 17.3 Properties of the Intl.PluralRules Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-pluralrules-prototype-object
PluralRulesPrototype::PluralRulesPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void PluralRulesPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 17.3.5 Intl.PluralRules.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-intl.pluralrules.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Intl.PluralRules"_string), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.resolvedOptions, resolved_options, 0, attr);
    define_native_function(realm, vm.names.select, select, 1, attr);
    define_native_function(realm, vm.names.selectRange, select_range, 2, attr);
}

// 17.3.2 Intl.PluralRules.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-intl.pluralrules.prototype.resolvedoptions
JS_DEFINE_NATIVE_FUNCTION(PluralRulesPrototype::resolved_options)
{
    auto& realm = *vm.current_realm();

    // 1. Let pr be the this value.
    // 2. Perform ? RequireInternalSlot(pr, [[InitializedPluralRules]]).
    auto plural_rules = TRY(typed_this_object(vm));

    // 3. Let options be OrdinaryObjectCreate(%Object.prototype%).
    auto options = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. Let pluralCategories be a List of Strings containing all possible results of PluralRuleSelect for the selected
    //    locale pr.[[Locale]], sorted according to the following order: "zero", "one", "two", "few", "many", "other".
    auto available_categories = plural_rules->formatter().available_plural_categories();

    auto plural_categories = Array::create_from<Unicode::PluralCategory>(realm, available_categories, [&](auto category) {
        return PrimitiveString::create(vm, Unicode::plural_category_to_string(category));
    });

    // 5. For each row of Table 30, except the header row, in table order, do
    //     a. Let p be the Property value of the current row.
    //     b. If p is "pluralCategories", then
    //         i. Let v be CreateArrayFromList(pluralCategories).
    //     c. Else,
    //         i. Let v be the value of pr's internal slot whose name is the Internal Slot value of the current row.
    //     d. If v is not undefined, then
    //         i. Perform ! CreateDataPropertyOrThrow(options, p, v).
    MUST(options->create_data_property_or_throw(vm.names.locale, PrimitiveString::create(vm, plural_rules->locale())));
    MUST(options->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, plural_rules->type_string())));
    MUST(options->create_data_property_or_throw(vm.names.minimumIntegerDigits, Value(plural_rules->min_integer_digits())));
    if (plural_rules->has_min_fraction_digits())
        MUST(options->create_data_property_or_throw(vm.names.minimumFractionDigits, Value(plural_rules->min_fraction_digits())));
    if (plural_rules->has_max_fraction_digits())
        MUST(options->create_data_property_or_throw(vm.names.maximumFractionDigits, Value(plural_rules->max_fraction_digits())));
    if (plural_rules->has_min_significant_digits())
        MUST(options->create_data_property_or_throw(vm.names.minimumSignificantDigits, Value(plural_rules->min_significant_digits())));
    if (plural_rules->has_max_significant_digits())
        MUST(options->create_data_property_or_throw(vm.names.maximumSignificantDigits, Value(plural_rules->max_significant_digits())));
    MUST(options->create_data_property_or_throw(vm.names.pluralCategories, plural_categories));
    MUST(options->create_data_property_or_throw(vm.names.roundingIncrement, Value(plural_rules->rounding_increment())));
    MUST(options->create_data_property_or_throw(vm.names.roundingMode, PrimitiveString::create(vm, plural_rules->rounding_mode_string())));
    MUST(options->create_data_property_or_throw(vm.names.roundingPriority, PrimitiveString::create(vm, plural_rules->computed_rounding_priority_string())));
    MUST(options->create_data_property_or_throw(vm.names.trailingZeroDisplay, PrimitiveString::create(vm, plural_rules->trailing_zero_display_string())));

    // 10. Return options.
    return options;
}

// 17.3.3 Intl.PluralRules.prototype.select ( value ), https://tc39.es/ecma402/#sec-intl.pluralrules.prototype.select
JS_DEFINE_NATIVE_FUNCTION(PluralRulesPrototype::select)
{
    // 1. Let pr be the this value.
    // 2. Perform ? RequireInternalSlot(pr, [[InitializedPluralRules]]).
    auto plural_rules = TRY(typed_this_object(vm));

    // 3. Let n be ? ToNumber(value).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 4. Return ! ResolvePlural(pr, n).[[PluralCategory]].
    auto plurality = resolve_plural(plural_rules, number);
    return PrimitiveString::create(vm, Unicode::plural_category_to_string(plurality));
}

// 17.3.4 Intl.PluralRules.prototype.selectRange ( start, end ), https://tc39.es/ecma402/#sec-intl.pluralrules.prototype.selectrange
JS_DEFINE_NATIVE_FUNCTION(PluralRulesPrototype::select_range)
{
    auto start = vm.argument(0);
    auto end = vm.argument(1);

    // 1. Let pr be the this value.
    // 2. Perform ? RequireInternalSlot(pr, [[InitializedPluralRules]]).
    auto plural_rules = TRY(typed_this_object(vm));

    // 3. If start is undefined or end is undefined, throw a TypeError exception.
    if (start.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "start"sv);
    if (end.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "end"sv);

    // 4. Let x be ? ToNumber(start).
    auto x = TRY(start.to_number(vm));

    // 5. Let y be ? ToNumber(end).
    auto y = TRY(end.to_number(vm));

    // 6. Return ? ResolvePluralRange(pr, x, y).
    auto plurality = TRY(resolve_plural_range(vm, plural_rules, x, y));
    return PrimitiveString::create(vm, Unicode::plural_category_to_string(plurality));
}

}
