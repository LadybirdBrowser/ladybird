/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/TimeZoneMethods.h>

namespace JS::Temporal {

// 11.5.2 CreateTimeZoneMethodsRecord ( timeZone, methods ), https://tc39.es/proposal-temporal/#sec-temporal-createtimezonemethodsrecord
ThrowCompletionOr<TimeZoneMethods> create_time_zone_methods_record(VM& vm, Variant<String, NonnullGCPtr<Object>> time_zone, ReadonlySpan<TimeZoneMethod> methods)
{
    // 1. Let record be the Time Zone Methods Record { [[Receiver]]: timeZone, [[GetOffsetNanosecondsFor]]: undefined, [[GetPossibleInstantsFor]]: undefined  }.
    TimeZoneMethods record {
        .receiver = move(time_zone),
        .get_offset_nanoseconds_for = nullptr,
        .get_possible_instants_for = nullptr,
    };

    // 2. For each element methodName in methods, do
    for (TimeZoneMethod method_name : methods) {
        // a. Perform ? TimeZoneMethodsRecordLookup(record, methodName).
        TRY(time_zone_methods_record_lookup(vm, record, method_name));
    }

    // 3. Return record.
    return record;
}

// 11.5.3 TimeZoneMethodsRecordLookup ( timeZoneRec, methodName ), https://tc39.es/proposal-temporal/#sec-temporal-timezonemethodsrecordlookup
ThrowCompletionOr<void> time_zone_methods_record_lookup(VM& vm, TimeZoneMethods& time_zone_record, TimeZoneMethod method_name)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: TimeZoneMethodsRecordHasLookedUp(timeZoneRec, methodName) is false.
    // 2. If methodName is GET-OFFSET-NANOSECONDS-FOR, then
    //     a. If timeZoneRec.[[Receiver]] is a String, then
    //         i. Set timeZoneRec.[[GetOffsetNanosecondsFor]] to %Temporal.TimeZone.prototype.getOffsetNanosecondsFor%.
    //     b. Else,
    //         i. Set timeZoneRec.[[GetOffsetNanosecondsFor]] to ? GetMethod(timeZoneRec.[[Receiver]], "getOffsetNanosecondsFor").
    //         ii. If timeZoneRec.[[GetOffsetNanosecondsFor]] is undefined, throw a TypeError exception.
    // 3. Else if methodName is GET-POSSIBLE-INSTANTS-FOR, then
    //     a. If timeZoneRec.[[Receiver]] is a String, then
    //         i. Set timeZoneRec.[[GetPossibleInstantsFor]] to %Temporal.TimeZone.prototype.getPossibleInstantsFor%.
    //     b. Else,
    //         i. Set timeZoneRec.[[GetPossibleInstantsFor]] to ? GetMethod(timeZoneRec.[[Receiver]], "getPossibleInstantsFor").
    //         ii. If timeZoneRec.[[GetPossibleInstantsFor]] is undefined, throw a TypeError exception.
    switch (method_name) {
#define __JS_ENUMERATE(PascalName, camelName, snake_name)                                                                 \
    case TimeZoneMethod::PascalName: {                                                                                    \
        VERIFY(!time_zone_record.snake_name);                                                                             \
        if (time_zone_record.receiver.has<String>()) {                                                                    \
            const auto& time_zone_prototype = *realm.intrinsics().temporal_time_zone_prototype();                         \
            time_zone_record.snake_name = time_zone_prototype.get_without_side_effects(vm.names.camelName).as_function(); \
        } else {                                                                                                          \
            Value time_zone { time_zone_record.receiver.get<NonnullGCPtr<Object>>() };                                    \
            time_zone_record.snake_name = TRY(time_zone.get_method(vm, vm.names.camelName));                              \
            if (!time_zone_record.snake_name)                                                                             \
                return vm.throw_completion<TypeError>(ErrorType::IsUndefined, #camelName##sv);                            \
        }                                                                                                                 \
        break;                                                                                                            \
    }
        JS_ENUMERATE_TIME_ZONE_METHODS
#undef __JS_ENUMERATE
    }
    // 4. Return UNUSED.
    return {};
}

// 11.5.4 TimeZoneMethodsRecordHasLookedUp ( timeZoneRec, methodName ), https://tc39.es/proposal-temporal/#sec-temporal-timezonemethodsrecordhaslookedup
bool time_zone_methods_record_has_looked_up(TimeZoneMethods const& time_zone_record, TimeZoneMethod method_name)
{
    // 1. If methodName is GET-OFFSET-NANOSECONDS-FOR, then
    //     a. Let method be timeZoneRec.[[GetOffsetNanosecondsFor]].
    // 2. Else if methodName is GET-POSSIBLE-INSTANTS-FOR, then
    //     a. Let method be timeZoneRec.[[GetPossibleInstantsFor]].
    // 3. If method is undefined, return false.
    // 4. Return true.
    switch (method_name) {
#define __JS_ENUMERATE(PascalName, camelName, snake_name) \
    case TimeZoneMethod::PascalName: {                    \
        return time_zone_record.snake_name != nullptr;    \
    }
        JS_ENUMERATE_TIME_ZONE_METHODS
#undef __JS_ENUMERATE
    }
    VERIFY_NOT_REACHED();
}

// 11.5.5 TimeZoneMethodsRecordIsBuiltin ( timeZoneRec ), https://tc39.es/proposal-temporal/#sec-temporal-timezonemethodsrecordisbuiltin
bool time_zone_methods_record_is_builtin(TimeZoneMethods const& time_zone_record)
{
    // 1. If timeZoneRec.[[Receiver]] is a String, return true.
    if (time_zone_record.receiver.has<String>())
        return true;

    // 2. Return false.
    return false;
}

// 11.5.6 TimeZoneMethodsRecordCall ( timeZoneRec, methodName, arguments ), https://tc39.es/proposal-temporal/#sec-temporal-timezonemethodsrecordcall
ThrowCompletionOr<Value> time_zone_methods_record_call(VM& vm, TimeZoneMethods const& time_zone_record, TimeZoneMethod method_name, ReadonlySpan<Value> arguments)
{
    // 1. Assert: TimeZoneMethodsRecordHasLookedUp(timeZoneRec, methodName) is true.
    VERIFY(time_zone_methods_record_has_looked_up(time_zone_record, method_name));

    // 2. Let receiver be timeZoneRec.[[Receiver]].
    // 3. If TimeZoneMethodsRecordIsBuiltin(timeZoneRec) is true, then
    //     a. Set receiver to ! CreateTemporalTimeZone(timeZoneRec.[[Receiver]]).
    GCPtr<Object> receiver;
    if (time_zone_methods_record_is_builtin(time_zone_record))
        receiver = MUST(create_temporal_time_zone(vm, time_zone_record.receiver.get<String>()));
    else
        receiver = time_zone_record.receiver.get<NonnullGCPtr<Object>>();

    // 4. If methodName is GET-OFFSET-NANOSECONDS-FOR, then
    //     a. Return ? Call(timeZoneRec.[[GetOffsetNanosecondsFor]], receiver, arguments).
    // 5. If methodName is GET-POSSIBLE-INSTANTS-FOR, then
    //     a. Return ? Call(timeZoneRec.[[GetPossibleInstantsFor]], receiver, arguments).
    switch (method_name) {
#define __JS_ENUMERATE(PascalName, camelName, snake_name)                       \
    case TimeZoneMethod::PascalName: {                                          \
        return TRY(call(vm, time_zone_record.snake_name, receiver, arguments)); \
    }
        JS_ENUMERATE_TIME_ZONE_METHODS
#undef __JS_ENUMERATE
    }
    VERIFY_NOT_REACHED();
}

}
