/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/PlainTimeConstructor.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(PlainTimeConstructor);

// 4.1 The Temporal.PlainTime Constructor, https://tc39.es/proposal-temporal/#sec-temporal-plaintime-constructor
PlainTimeConstructor::PlainTimeConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.PlainTime.as_string(), realm.intrinsics().function_prototype())
{
}

void PlainTimeConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 4.2.1 Temporal.PlainTime.prototype, https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().temporal_plain_time_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.from, from, 1, attr);
    define_native_function(realm, vm.names.compare, compare, 2, attr);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 4.1.1 Temporal.PlainTime ( [ hour [ , minute [ , second [ , millisecond [ , microsecond [ , nanosecond ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime
ThrowCompletionOr<Value> PlainTimeConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.PlainTime");
}

// 4.1.1 Temporal.PlainTime ( [ hour [ , minute [ , second [ , millisecond [ , microsecond [ , nanosecond ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime
ThrowCompletionOr<NonnullGCPtr<Object>> PlainTimeConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 2. Let hour be ? ToIntegerWithTruncation(hour).
    auto hour = TRY(to_integer_with_truncation(vm, vm.argument(0), ErrorType::TemporalInvalidPlainTime));

    // 3. Let minute be ? ToIntegerWithTruncation(hour).
    auto minute = TRY(to_integer_with_truncation(vm, vm.argument(1), ErrorType::TemporalInvalidPlainTime));

    // 4. Let second be ? ToIntegerWithTruncation(hour).
    auto second = TRY(to_integer_with_truncation(vm, vm.argument(2), ErrorType::TemporalInvalidPlainTime));

    // 5. Let millisecond be ? ToIntegerWithTruncation(hour).
    auto millisecond = TRY(to_integer_with_truncation(vm, vm.argument(3), ErrorType::TemporalInvalidPlainTime));

    // 6. Let microsecond be ? ToIntegerWithTruncation(hour).
    auto microsecond = TRY(to_integer_with_truncation(vm, vm.argument(4), ErrorType::TemporalInvalidPlainTime));

    // 7. Let nanosecond be ? ToIntegerWithTruncation(hour).
    auto nanosecond = TRY(to_integer_with_truncation(vm, vm.argument(5), ErrorType::TemporalInvalidPlainTime));

    // IMPLEMENTATION DEFINED: This is an optimization that allows us to treat these doubles as normal integers from this point onwards.
    // This does not change the exposed behavior as the call to CreateTemporalTime will immediately check that these values are valid
    // ISO values (for hours: 0 - 23, for minutes and seconds: 0 - 59, milliseconds, microseconds, and nanoseconds: 0 - 999) all of which
    // are subsets of this check.
    if (!AK::is_within_range<u8>(hour) || !AK::is_within_range<u8>(minute) || !AK::is_within_range<u8>(second) || !AK::is_within_range<u16>(millisecond) || !AK::is_within_range<u16>(microsecond) || !AK::is_within_range<u16>(nanosecond))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainTime);

    // 8. Return ? CreateTemporalTime(hour, minute, second, millisecond, microsecond, nanosecond, NewTarget).
    return *TRY(create_temporal_time(vm, hour, minute, second, millisecond, microsecond, nanosecond, &new_target));
}

// 4.2.2 Temporal.PlainTime.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.from
JS_DEFINE_NATIVE_FUNCTION(PlainTimeConstructor::from)
{
    // 1. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(1)));

    // 2. Let overflow be ? ToTemporalOverflow(options).
    auto overflow = TRY(to_temporal_overflow(vm, options));

    auto item = vm.argument(0);

    // 3. If Type(item) is Object and item has an [[InitializedTemporalTime]] internal slot, then
    if (item.is_object() && is<PlainTime>(item.as_object())) {
        auto& plain_time = static_cast<PlainTime&>(item.as_object());
        // a. Return ! CreateTemporalTime(item.[[ISOHour]], item.[[ISOMinute]], item.[[ISOSecond]], item.[[ISOMillisecond]], item.[[ISOMicrosecond]], item.[[ISONanosecond]]).
        return MUST(create_temporal_time(vm, plain_time.iso_hour(), plain_time.iso_minute(), plain_time.iso_second(), plain_time.iso_millisecond(), plain_time.iso_microsecond(), plain_time.iso_nanosecond()));
    }

    // 4. Return ? ToTemporalTime(item, overflow).
    return TRY(to_temporal_time(vm, item, overflow));
}

// 4.2.3 Temporal.PlainTime.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.compare
JS_DEFINE_NATIVE_FUNCTION(PlainTimeConstructor::compare)
{
    // 1. Set one to ? ToTemporalTime(one).
    auto* one = TRY(to_temporal_time(vm, vm.argument(0)));

    // 2. Set two to ? ToTemporalTime(two).
    auto* two = TRY(to_temporal_time(vm, vm.argument(1)));

    // 3. Return 𝔽(! CompareTemporalTime(one.[[ISOHour]], one.[[ISOMinute]], one.[[ISOSecond]], one.[[ISOMillisecond]], one.[[ISOMicrosecond]], one.[[ISONanosecond]], two.[[ISOHour]], two.[[ISOMinute]], two.[[ISOSecond]], two.[[ISOMillisecond]], two.[[ISOMicrosecond]], two.[[ISONanosecond]])).
    return Value(compare_temporal_time(one->iso_hour(), one->iso_minute(), one->iso_second(), one->iso_millisecond(), one->iso_microsecond(), one->iso_nanosecond(), two->iso_hour(), two->iso_minute(), two->iso_second(), two->iso_millisecond(), two->iso_microsecond(), two->iso_nanosecond()));
}

}
