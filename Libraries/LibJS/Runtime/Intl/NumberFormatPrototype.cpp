/*
 * Copyright (c) 2021-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/NumberFormat.h>
#include <LibJS/Runtime/Intl/NumberFormatFunction.h>
#include <LibJS/Runtime/Intl/NumberFormatPrototype.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(NumberFormatPrototype);

// 15.3 Properties of the Intl.NumberFormat Prototype Object, https://tc39.es/ecma402/#sec-properties-of-intl-numberformat-prototype-object
NumberFormatPrototype::NumberFormatPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void NumberFormatPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 15.3.2 Intl.NumberFormat.prototype [ @@toStringTag ], https://tc39.es/ecma402/#sec-intl.numberformat.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Intl.NumberFormat"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.format, format, nullptr, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.formatToParts, format_to_parts, 1, attr);
    define_native_function(realm, vm.names.formatRange, format_range, 2, attr);
    define_native_function(realm, vm.names.formatRangeToParts, format_range_to_parts, 2, attr);
    define_native_function(realm, vm.names.resolvedOptions, resolved_options, 0, attr);
}

// 15.3.3 get Intl.NumberFormat.prototype.format, https://tc39.es/ecma402/#sec-intl.numberformat.prototype.format
JS_DEFINE_NATIVE_FUNCTION(NumberFormatPrototype::format)
{
    auto& realm = *vm.current_realm();

    // 1. Let nf be the this value.
    // 2. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
    //     a. Set nf to ? UnwrapNumberFormat(nf).
    // 3. Perform ? RequireInternalSlot(nf, [[InitializedNumberFormat]]).
    auto number_format = TRY(typed_this_object(vm));

    // 4. If nf.[[BoundFormat]] is undefined, then
    if (!number_format->bound_format()) {
        // a. Let F be a new built-in function object as defined in Number Format Functions (15.1.4).
        // b. Set F.[[NumberFormat]] to nf.
        auto bound_format = NumberFormatFunction::create(realm, number_format);

        // c. Set nf.[[BoundFormat]] to F.
        number_format->set_bound_format(bound_format);
    }

    // 5. Return nf.[[BoundFormat]].
    return number_format->bound_format();
}

// 15.3.4 Intl.NumberFormat.prototype.formatToParts ( value ), https://tc39.es/ecma402/#sec-intl.numberformat.prototype.formattoparts
JS_DEFINE_NATIVE_FUNCTION(NumberFormatPrototype::format_to_parts)
{
    auto value = vm.argument(0);

    // 1. Let nf be the this value.
    // 2. Perform ? RequireInternalSlot(nf, [[InitializedNumberFormat]]).
    auto number_format = TRY(typed_this_object(vm));

    // 3. Let x be ? ToIntlMathematicalValue(value).
    auto mathematical_value = TRY(to_intl_mathematical_value(vm, value));

    // 4. Return ? FormatNumericToParts(nf, x).
    return format_numeric_to_parts(vm, number_format, move(mathematical_value));
}

// 15.3.5 Intl.NumberFormat.prototype.formatRange ( start, end ), https://tc39.es/ecma402/#sec-intl.numberformat.prototype.formatrange
JS_DEFINE_NATIVE_FUNCTION(NumberFormatPrototype::format_range)
{
    auto start = vm.argument(0);
    auto end = vm.argument(1);

    // 1. Let nf be the this value.
    // 2. Perform ? RequireInternalSlot(nf, [[InitializedNumberFormat]]).
    auto number_format = TRY(typed_this_object(vm));

    // 3. If start is undefined or end is undefined, throw a TypeError exception.
    if (start.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "start"sv);
    if (end.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "end"sv);

    // 4. Let x be ? ToIntlMathematicalValue(start).
    auto x = TRY(to_intl_mathematical_value(vm, start));

    // 5. Let y be ? ToIntlMathematicalValue(end).
    auto y = TRY(to_intl_mathematical_value(vm, end));

    // 6. Return ? FormatNumericRange(nf, x, y).
    auto formatted = TRY(format_numeric_range(vm, number_format, move(x), move(y)));
    return PrimitiveString::create(vm, move(formatted));
}

// 15.3.6 Intl.NumberFormat.prototype.formatRangeToParts ( start, end ), https://tc39.es/ecma402/#sec-intl.numberformat.prototype.formatrangetoparts
JS_DEFINE_NATIVE_FUNCTION(NumberFormatPrototype::format_range_to_parts)
{
    auto start = vm.argument(0);
    auto end = vm.argument(1);

    // 1. Let nf be the this value.
    // 2. Perform ? RequireInternalSlot(nf, [[InitializedNumberFormat]]).
    auto number_format = TRY(typed_this_object(vm));

    // 3. If start is undefined or end is undefined, throw a TypeError exception.
    if (start.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "start"sv);
    if (end.is_undefined())
        return vm.throw_completion<TypeError>(ErrorType::IsUndefined, "end"sv);

    // 4. Let x be ? ToIntlMathematicalValue(start).
    auto x = TRY(to_intl_mathematical_value(vm, start));

    // 5. Let y be ? ToIntlMathematicalValue(end).
    auto y = TRY(to_intl_mathematical_value(vm, end));

    // 6. Return ? FormatNumericRangeToParts(nf, x, y).
    return TRY(format_numeric_range_to_parts(vm, number_format, move(x), move(y)));
}

// 15.3.7 Intl.NumberFormat.prototype.resolvedOptions ( ), https://tc39.es/ecma402/#sec-intl.numberformat.prototype.resolvedoptions
JS_DEFINE_NATIVE_FUNCTION(NumberFormatPrototype::resolved_options)
{
    auto& realm = *vm.current_realm();

    // 1. Let nf be the this value.
    // 2. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
    //     a. Set nf to ? UnwrapNumberFormat(nf).
    // 3. Perform ? RequireInternalSlot(nf, [[InitializedNumberFormat]]).
    auto number_format = TRY(typed_this_object(vm));

    // 4. Let options be OrdinaryObjectCreate(%Object.prototype%).
    auto options = Object::create(realm, realm.intrinsics().object_prototype());

    // 5. For each row of Table 11, except the header row, in table order, do
    //     a. Let p be the Property value of the current row.
    //     b. Let v be the value of nf's internal slot whose name is the Internal Slot value of the current row.
    //     c. If v is not undefined, then
    //         i. Perform ! CreateDataPropertyOrThrow(options, p, v).
    MUST(options->create_data_property_or_throw(vm.names.locale, PrimitiveString::create(vm, number_format->locale())));
    MUST(options->create_data_property_or_throw(vm.names.numberingSystem, PrimitiveString::create(vm, number_format->numbering_system())));
    MUST(options->create_data_property_or_throw(vm.names.style, PrimitiveString::create(vm, number_format->style_string())));
    if (number_format->has_currency())
        MUST(options->create_data_property_or_throw(vm.names.currency, PrimitiveString::create(vm, number_format->currency())));
    if (number_format->has_currency_display())
        MUST(options->create_data_property_or_throw(vm.names.currencyDisplay, PrimitiveString::create(vm, number_format->currency_display_string())));
    if (number_format->has_currency_sign())
        MUST(options->create_data_property_or_throw(vm.names.currencySign, PrimitiveString::create(vm, number_format->currency_sign_string())));
    if (number_format->has_unit())
        MUST(options->create_data_property_or_throw(vm.names.unit, PrimitiveString::create(vm, number_format->unit())));
    if (number_format->has_unit_display())
        MUST(options->create_data_property_or_throw(vm.names.unitDisplay, PrimitiveString::create(vm, number_format->unit_display_string())));
    MUST(options->create_data_property_or_throw(vm.names.minimumIntegerDigits, Value(number_format->min_integer_digits())));
    if (number_format->has_min_fraction_digits())
        MUST(options->create_data_property_or_throw(vm.names.minimumFractionDigits, Value(number_format->min_fraction_digits())));
    if (number_format->has_max_fraction_digits())
        MUST(options->create_data_property_or_throw(vm.names.maximumFractionDigits, Value(number_format->max_fraction_digits())));
    if (number_format->has_min_significant_digits())
        MUST(options->create_data_property_or_throw(vm.names.minimumSignificantDigits, Value(number_format->min_significant_digits())));
    if (number_format->has_max_significant_digits())
        MUST(options->create_data_property_or_throw(vm.names.maximumSignificantDigits, Value(number_format->max_significant_digits())));
    MUST(options->create_data_property_or_throw(vm.names.useGrouping, number_format->use_grouping_to_value(vm)));
    MUST(options->create_data_property_or_throw(vm.names.notation, PrimitiveString::create(vm, number_format->notation_string())));
    if (number_format->has_compact_display())
        MUST(options->create_data_property_or_throw(vm.names.compactDisplay, PrimitiveString::create(vm, number_format->compact_display_string())));
    MUST(options->create_data_property_or_throw(vm.names.signDisplay, PrimitiveString::create(vm, number_format->sign_display_string())));
    MUST(options->create_data_property_or_throw(vm.names.roundingIncrement, Value(number_format->rounding_increment())));
    MUST(options->create_data_property_or_throw(vm.names.roundingMode, PrimitiveString::create(vm, number_format->rounding_mode_string())));
    MUST(options->create_data_property_or_throw(vm.names.roundingPriority, PrimitiveString::create(vm, number_format->computed_rounding_priority_string())));
    MUST(options->create_data_property_or_throw(vm.names.trailingZeroDisplay, PrimitiveString::create(vm, number_format->trailing_zero_display_string())));

    // 6. Return options.
    return options;
}

}
