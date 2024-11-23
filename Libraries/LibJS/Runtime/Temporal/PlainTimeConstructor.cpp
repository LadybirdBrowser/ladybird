/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/PlainTimeConstructor.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainTimeConstructor);

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

    // 1. If NewTarget is undefined, then
    //     a. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.PlainTime");
}

// 4.1.1 Temporal.PlainTime ( [ hour [ , minute [ , second [ , millisecond [ , microsecond [ , nanosecond ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime
ThrowCompletionOr<GC::Ref<Object>> PlainTimeConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto next_integer_argument = [&, index = 0]() mutable -> ThrowCompletionOr<double> {
        if (auto value = vm.argument(index++); !value.is_undefined())
            return to_integer_with_truncation(vm, value, ErrorType::TemporalInvalidPlainTime);
        return 0;
    };

    // 2. If hour is undefined, set hour to 0; else set hour to ? ToIntegerWithTruncation(hour).
    auto hour = TRY(next_integer_argument());

    // 3. If minute is undefined, set minute to 0; else set minute to ? ToIntegerWithTruncation(minute).
    auto minute = TRY(next_integer_argument());

    // 4. If second is undefined, set second to 0; else set second to ? ToIntegerWithTruncation(second).
    auto second = TRY(next_integer_argument());

    // 5. If millisecond is undefined, set millisecond to 0; else set millisecond to ? ToIntegerWithTruncation(millisecond).
    auto millisecond = TRY(next_integer_argument());

    // 6. If microsecond is undefined, set microsecond to 0; else set microsecond to ? ToIntegerWithTruncation(microsecond).
    auto microsecond = TRY(next_integer_argument());

    // 7. If nanosecond is undefined, set nanosecond to 0; else set nanosecond to ? ToIntegerWithTruncation(nanosecond).
    auto nanosecond = TRY(next_integer_argument());

    // 8. If IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond) is false, throw a RangeError exception.
    if (!is_valid_time(hour, minute, second, millisecond, microsecond, nanosecond))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainTime);

    // 9. Let time be CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond).
    auto time = create_time_record(hour, minute, second, millisecond, microsecond, nanosecond);

    // 10. Return ? CreateTemporalTime(time, NewTarget).
    return TRY(create_temporal_time(vm, time, new_target));
}

// 4.2.2 Temporal.PlainTime.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.from
JS_DEFINE_NATIVE_FUNCTION(PlainTimeConstructor::from)
{
    // 4. Return ? ToTemporalTime(item, overflow).
    return TRY(to_temporal_time(vm, vm.argument(0), vm.argument(1)));
}

// 4.2.3 Temporal.PlainTime.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.compare
JS_DEFINE_NATIVE_FUNCTION(PlainTimeConstructor::compare)
{
    // 1. Set one to ? ToTemporalTime(one).
    auto one = TRY(to_temporal_time(vm, vm.argument(0)));

    // 2. Set two to ? ToTemporalTime(two).
    auto two = TRY(to_temporal_time(vm, vm.argument(1)));

    // 3. Return 𝔽(CompareTimeRecord(one.[[Time]], two.[[Time]])).
    return compare_time_record(one->time(), two->time());
}

}
