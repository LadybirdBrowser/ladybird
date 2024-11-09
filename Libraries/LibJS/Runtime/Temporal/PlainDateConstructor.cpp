/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateConstructor.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(PlainDateConstructor);

// 3.1 The Temporal.PlainDate Constructor, https://tc39.es/proposal-temporal/#sec-temporal-plaindate-constructor
PlainDateConstructor::PlainDateConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.PlainDate.as_string(), realm.intrinsics().function_prototype())
{
}

void PlainDateConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 3.2.1 Temporal.PlainDate.prototype, https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().temporal_plain_date_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.from, from, 1, attr);
    define_native_function(realm, vm.names.compare, compare, 2, attr);

    define_direct_property(vm.names.length, Value(3), Attribute::Configurable);
}

// 3.1.1 Temporal.PlainDate ( isoYear, isoMonth, isoDay [ , calendarLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate
ThrowCompletionOr<Value> PlainDateConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.PlainDate");
}

// 3.1.1 Temporal.PlainDate ( isoYear, isoMonth, isoDay [ , calendarLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate
ThrowCompletionOr<NonnullGCPtr<Object>> PlainDateConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 2. Let y be ? ToIntegerWithTruncation(isoYear).
    auto y = TRY(to_integer_with_truncation(vm, vm.argument(0), ErrorType::TemporalInvalidPlainDate));

    // 3. Let m be ? ToIntegerWithTruncation(isoMonth).
    auto m = TRY(to_integer_with_truncation(vm, vm.argument(1), ErrorType::TemporalInvalidPlainDate));

    // 4. Let d be ? ToIntegerWithTruncation(isoDay).
    auto d = TRY(to_integer_with_truncation(vm, vm.argument(2), ErrorType::TemporalInvalidPlainDate));

    // 5. Let calendar be ? ToTemporalCalendarWithISODefault(calendarLike).
    auto* calendar = TRY(to_temporal_calendar_with_iso_default(vm, vm.argument(3)));

    // IMPLEMENTATION DEFINED: This is an optimization that allows us to treat these doubles as normal integers from this point onwards.
    // This does not change the exposed behavior as the call to CreateTemporalDate will immediately check that these values are valid
    // ISO values (for years: -273975 - 273975, for months: 1 - 12, for days: 1 - 31) all of which are subsets of this check.
    if (!AK::is_within_range<i32>(y) || !AK::is_within_range<u8>(m) || !AK::is_within_range<u8>(d))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDate);

    // 6. Return ? CreateTemporalDate(y, m, d, calendar, NewTarget).
    return *TRY(create_temporal_date(vm, y, m, d, *calendar, &new_target));
}

// 3.2.2 Temporal.PlainDate.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.from
JS_DEFINE_NATIVE_FUNCTION(PlainDateConstructor::from)
{
    // 1. Set options to ? GetOptionsObject(options).
    auto const* options = TRY(get_options_object(vm, vm.argument(1)));

    auto item = vm.argument(0);
    // 2. If Type(item) is Object and item has an [[InitializedTemporalDate]] internal slot, then
    if (item.is_object() && is<PlainDate>(item.as_object())) {
        auto& plain_date_item = static_cast<PlainDate&>(item.as_object());
        // a. Perform ? ToTemporalOverflow(options).
        (void)TRY(to_temporal_overflow(vm, options));

        // b. Return ! CreateTemporalDate(item.[[ISOYear]], item.[[ISOMonth]], item.[[ISODay]], item.[[Calendar]]).
        return MUST(create_temporal_date(vm, plain_date_item.iso_year(), plain_date_item.iso_month(), plain_date_item.iso_day(), plain_date_item.calendar()));
    }

    // 3. Return ? ToTemporalDate(item, options).
    return TRY(to_temporal_date(vm, item, options));
}

// 3.2.3 Temporal.PlainDate.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.compare
JS_DEFINE_NATIVE_FUNCTION(PlainDateConstructor::compare)
{
    // 1. Set one to ? ToTemporalDate(one).
    auto* one = TRY(to_temporal_date(vm, vm.argument(0)));

    // 2. Set two to ? ToTemporalDate(two).
    auto* two = TRY(to_temporal_date(vm, vm.argument(1)));

    // 3. Return 𝔽(! CompareISODate(one.[[ISOYear]], one.[[ISOMonth]], one.[[ISODay]], two.[[ISOYear]], two.[[ISOMonth]], two.[[ISODay]])).
    return Value(compare_iso_date(one->iso_year(), one->iso_month(), one->iso_day(), two->iso_year(), two->iso_month(), two->iso_day()));
}

}
