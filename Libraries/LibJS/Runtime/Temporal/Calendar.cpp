/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/CalendarConstructor.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(Calendar);

// 12 Temporal.Calendar Objects, https://tc39.es/proposal-temporal/#sec-temporal-calendar-objects
Calendar::Calendar(String identifier, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_identifier(move(identifier))
{
}

// 12.1.1 IsBuiltinCalendar ( id ), https://tc39.es/proposal-temporal/#sec-temporal-isbuiltincalendar
bool is_builtin_calendar(StringView identifier)
{
    // 1. Let calendars be AvailableCalendars().
    auto calendars = available_calendars();

    // 2. If calendars contains the ASCII-lowercase of id, return true.
    for (auto calendar : calendars) {
        if (calendar.equals_ignoring_ascii_case(identifier))
            return true;
    }

    // 3. Return false.
    return false;
}

// 12.1.2 AvailableCalendars ( ), https://tc39.es/proposal-temporal/#sec-temporal-availablecalendars
ReadonlySpan<StringView> available_calendars()
{
    // 1. Let calendars be the List of String values representing calendar types supported by the implementation.
    // NOTE: This can be removed in favor of using `Unicode::get_available_calendars()` once everything is updated to handle non-iso8601 calendars.
    static constexpr AK::Array calendars { "iso8601"sv };

    // 2. Assert: calendars contains "iso8601".
    // 3. Assert: calendars does not contain any element that does not identify a calendar type in the Unicode Common Locale Data Repository (CLDR).
    // 4. Sort calendars in order as if an Array of the same values had been sorted using %Array.prototype.sort% with undefined as comparefn.

    // 5. Return calendars.
    return calendars.span();
}

// 12.2.2 CreateCalendarMethodsRecord ( calendar, methods ), https://tc39.es/proposal-temporal/#sec-temporal-createcalendarmethodsrecord
ThrowCompletionOr<CalendarMethods> create_calendar_methods_record(VM& vm, Variant<String, NonnullGCPtr<Object>> calendar, ReadonlySpan<CalendarMethod> methods)
{
    // 1. Let record be the Calendar Methods Record { [[Receiver]]: calendar, [[DateAdd]]: undefined, [[DateFromFields]]: undefined, [[DateUntil]]: undefined, [[Day]]: undefined, [[Fields]]: undefined, [[MergeFields]]: undefined, [[MonthDayFromFields]]: undefined, [[YearMonthFromFields]]: undefined }.
    CalendarMethods record {
        .receiver = move(calendar),
        .date_add = nullptr,
        .date_from_fields = nullptr,
        .date_until = nullptr,
        .day = nullptr,
        .fields = nullptr,
        .merge_fields = nullptr,
        .month_day_from_fields = nullptr,
        .year_month_from_fields = nullptr,
    };

    // 2. For each element methodName in methods, do
    for (auto const& method_name : methods) {
        // a. Perform ? CalendarMethodsRecordLookup(record, methodName).
        TRY(calendar_methods_record_lookup(vm, record, method_name));
    }

    // 3. Return record.
    return record;
}

ThrowCompletionOr<Optional<CalendarMethods>> create_calendar_methods_record_from_relative_to(VM& vm, GCPtr<PlainDate> plain_relative_to, GCPtr<ZonedDateTime> zoned_relative_to, ReadonlySpan<CalendarMethod> methods)
{
    // FIXME: The casts to NonnullGCPtr<Object> should not be here, and can be fixed once PlainDate & ZonedDateTime have the updated type in the [[Calendar]] slot.

    // 1. If zonedRelativeTo is not undefined, return ? CreateCalendarMethodsRecord(zonedRelativeTo.[[Calendar]], methods).
    if (zoned_relative_to)
        return TRY(create_calendar_methods_record(vm, NonnullGCPtr<Object> { zoned_relative_to->calendar() }, methods));

    // 2. If plainRelativeTo is not undefined, return ? CreateCalendarMethodsRecord(plainRelativeTo.[[Calendar]], methods).
    if (plain_relative_to)
        return TRY(create_calendar_methods_record(vm, NonnullGCPtr<Object> { plain_relative_to->calendar() }, methods));

    // 3. Return undefined.
    return OptionalNone {};
}

// 12.2.4 CalendarMethodsRecordLookup ( calendarRec, methodName ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmethodsrecordlookup
ThrowCompletionOr<void> calendar_methods_record_lookup(VM& vm, CalendarMethods& calendar_record, CalendarMethod method_name)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: CalendarMethodsRecordHasLookedUp(calendarRec, methodName) is false.
    // 2. If methodName is DATE-ADD, then
    //     a. If calendarRec.[[Receiver]] is a String, then
    //         i. Set calendarRec.[[DateAdd]] to %Temporal.Calendar.prototype.dateAdd%.
    //     b. Else,
    //         i. Set calendarRec.[[DateAdd]] to ? GetMethod(calendarRec.[[Receiver]], "dateAdd").
    //         ii. If calendarRec.[[DateAdd]] is undefined, throw a TypeError exception.
    // 3. Else if methodName is DATE-FROM-FIELDS, then
    //     a. If calendarRec.[[Receiver]] is a String, then
    //         i. Set calendarRec.[[DateFromFields]] to %Temporal.Calendar.prototype.dateFromFields%.
    //     b. Else,
    //         i. Set calendarRec.[[DateFromFields]] to ? GetMethod(calendarRec.[[Receiver]], "dateFromFields").
    //         ii. If calendarRec.[[DateFromFields]] is undefined, throw a TypeError exception.
    // 4. Else if methodName is DATE-UNTIL, then
    //     a. If calendarRec.[[Receiver]] is a String, then
    //         i. Set calendarRec.[[DateUntil]] to %Temporal.Calendar.prototype.dateUntil%.
    //     b. Else,
    //         i. Set calendarRec.[[DateUntil]] to ? GetMethod(calendarRec.[[Receiver]], "dateUntil").
    //         ii. If calendarRec.[[DateUntil]] is undefined, throw a TypeError exception.
    // 5. Else if methodName is DAY, then
    //     a. If calendarRec.[[Receiver]] is a String, then
    //         i. Set calendarRec.[[Day]] to %Temporal.Calendar.prototype.day%.
    //     b. Else,
    //         i. Set calendarRec.[[Day]] to ? GetMethod(calendarRec.[[Receiver]], "day").
    //         ii. If calendarRec.[[Day]] is undefined, throw a TypeError exception.
    // 6. Else if methodName is FIELDS, then
    //     a. If calendarRec.[[Receiver]] is a String, then
    //         i. Set calendarRec.[[Fields]] to %Temporal.Calendar.prototype.fields%.
    //     b. Else,
    //         i. Set calendarRec.[[Fields]] to ? GetMethod(calendarRec.[[Receiver]], "fields").
    //         ii. If calendarRec.[[Fields]] is undefined, throw a TypeError exception.
    // 7. Else if methodName is MERGE-FIELDS, then
    //     a. If calendarRec.[[Receiver]] is a String, then
    //         i. Set calendarRec.[[MergeFields]] to %Temporal.Calendar.prototype.mergeFields%.
    //     b. Else,
    //         i. Set calendarRec.[[MergeFields]] to ? GetMethod(calendarRec.[[Receiver]], "mergeFields").
    //         ii. If calendarRec.[[MergeFields]] is undefined, throw a TypeError exception.
    // 8. Else if methodName is MONTH-DAY-FROM-FIELDS, then
    //     a. If calendarRec.[[Receiver]] is a String, then
    //         i. Set calendarRec.[[MonthDayFromFields]] to %Temporal.Calendar.prototype.monthDayFromFields%.
    //     b. Else,
    //         i. Set calendarRec.[[MonthDayFromFields]] to ? GetMethod(calendarRec.[[Receiver]], "monthDayFromFields").
    //         ii. If calendarRec.[[MonthDayFromFields]] is undefined, throw a TypeError exception.
    // 9. Else if methodName is YEAR-MONTH-FROM-FIELDS, then
    //     a. If calendarRec.[[Receiver]] is a String, then
    //         i. Set calendarRec.[[YearMonthFromFields]] to %Temporal.Calendar.prototype.yearMonthFromFields%.
    //     b. Else,
    //         i. Set calendarRec.[[YearMonthFromFields]] to ? GetMethod(calendarRec.[[Receiver]], "yearMonthFromFields").
    //         ii. If calendarRec.[[YearMonthFromFields]] is undefined, throw a TypeError exception.
    switch (method_name) {
#define __JS_ENUMERATE(PascalName, camelName, snake_name)                                                               \
    case CalendarMethod::PascalName: {                                                                                  \
        VERIFY(!calendar_record.snake_name);                                                                            \
        if (calendar_record.receiver.has<String>()) {                                                                   \
            const auto& calendar_prototype = *realm.intrinsics().temporal_calendar_prototype();                         \
            calendar_record.snake_name = calendar_prototype.get_without_side_effects(vm.names.camelName).as_function(); \
        } else {                                                                                                        \
            Value calendar { calendar_record.receiver.get<NonnullGCPtr<Object>>() };                                    \
            calendar_record.snake_name = TRY(calendar.get_method(vm, vm.names.camelName));                              \
            if (!calendar_record.snake_name)                                                                            \
                return vm.throw_completion<TypeError>(ErrorType::IsUndefined, #camelName##sv);                          \
        }                                                                                                               \
        break;                                                                                                          \
    }
        JS_ENUMERATE_CALENDAR_METHODS
#undef __JS_ENUMERATE
    }

    // 10. Return unused.
    return {};
}

// 12.2.5 CalendarMethodsRecordHasLookedUp ( calendarRec, methodName ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmethodsrecordhaslookedup
bool calendar_methods_record_has_looked_up(CalendarMethods const& calendar_record, CalendarMethod method_name)
{
    // 1. If methodName is DATE-ADD, then
    //     a. Let method be calendarRec.[[DateAdd]].
    // 2. Else if methodName is DATE-FROM-FIELDS, then
    //     a. Let method be calendarRec.[[DateFromFields]].
    // 3. Else if methodName is DATE-UNTIL, then
    //     a. Let method be calendarRec.[[DateUntil]].
    // 4. Else if methodName is DAY, then
    //     a. Let method be calendarRec.[[Day]].
    // 5. Else if methodName is FIELDS, then
    //     a. Let method be calendarRec.[[Fields]].
    // 6. Else if methodName is MERGE-FIELDS, then
    //     a. Let method be calendarRec.[[MergeFields]].
    // 7. Else if methodName is MONTH-DAY-FROM-FIELDS, then
    //     a. Let method be calendarRec.[[MonthDayFromFields]].
    // 8. Else if methodName is YEAR-MONTH-FROM-FIELDS, then
    //     a. Let method be calendarRec.[[YearMonthFromFields]].
    // 9. If method is undefined, return false.
    // 10. Return true.
    switch (method_name) {
#define __JS_ENUMERATE(PascalName, camelName, snake_name) \
    case CalendarMethod::PascalName: {                    \
        return calendar_record.snake_name != nullptr;     \
    }
        JS_ENUMERATE_CALENDAR_METHODS
#undef __JS_ENUMERATE
    }
    VERIFY_NOT_REACHED();
}

// 12.2.6 CalendarMethodsRecordIsBuiltin ( calendarRec ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmethodsrecordisbuiltin
bool calendar_methods_record_is_builtin(CalendarMethods const& calendar_record)
{
    // 1. If calendarRec.[[Receiver]] is a String, return true.
    if (calendar_record.receiver.has<String>())
        return true;

    // 2. Return false.
    return false;
}

// 12.2.7 CalendarMethodsRecordCall ( calendarRec, methodName, arguments ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmethodsrecordcall
ThrowCompletionOr<Value> calendar_methods_record_call(VM& vm, CalendarMethods const& calendar_record, CalendarMethod method_name, ReadonlySpan<Value> arguments)
{
    // 1. Assert: CalendarMethodsRecordHasLookedUp(calendarRec, methodName) is true.
    VERIFY(calendar_methods_record_has_looked_up(calendar_record, method_name));

    // 2. Let receiver be calendarRec.[[Receiver]].
    // 3. If CalendarMethodsRecordIsBuiltin(calendarRec) is true, then
    //     a. Set receiver to ! CreateTemporalCalendar(calendarRec.[[Receiver]]).
    GCPtr<Object> receiver;
    if (calendar_methods_record_is_builtin(calendar_record))
        receiver = MUST(create_temporal_calendar(vm, calendar_record.receiver.get<String>()));
    else
        receiver = calendar_record.receiver.get<NonnullGCPtr<Object>>();

    // 4. If methodName is DATE-ADD, then
    //     a. Return ? Call(calendarRec.[[DateAdd]], receiver, arguments).
    // 5. If methodName is DATE-FROM-FIELDS, then
    //     a. Return ? Call(calendarRec.[[DateFromFields]], receiver, arguments).
    // 6. If methodName is DATE-UNTIL, then
    //     a. Return ? Call(calendarRec.[[DateUntil]], receiver, arguments).
    // 7. If methodName is DAY, then
    //     a. Return ? Call(calendarRec.[[Day]], receiver, arguments).
    // 8. If methodName is FIELDS, then
    //     a. Return ? Call(calendarRec.[[Fields]], receiver, arguments).
    // 9. If methodName is MERGE-FIELDS, then
    //     a. Return ? Call(calendarRec.[[MergeFields]], receiver, arguments).
    // 10. If methodName is MONTH-DAY-FROM-FIELDS, then
    //     a. Return ? Call(calendarRec.[[MonthDayFromFields]], receiver, arguments).
    // 11. If methodName is YEAR-MONTH-FROM-FIELDS, then
    //     a. Return ? Call(calendarRec.[[YearMonthFromFields]], receiver, arguments).
    switch (method_name) {
#define __JS_ENUMERATE(PascalName, camelName, snake_name)                      \
    case CalendarMethod::PascalName: {                                         \
        return TRY(call(vm, calendar_record.snake_name, receiver, arguments)); \
    }
        JS_ENUMERATE_CALENDAR_METHODS
#undef __JS_ENUMERATE
    }
    VERIFY_NOT_REACHED();
}

// 12.2.1 CreateTemporalCalendar ( identifier [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalcalendar
ThrowCompletionOr<Calendar*> create_temporal_calendar(VM& vm, String const& identifier, FunctionObject const* new_target)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: IsBuiltinCalendar(identifier) is true.
    VERIFY(is_builtin_calendar(identifier));

    // 2. If newTarget is not provided, set newTarget to %Temporal.Calendar%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_calendar_constructor();

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.Calendar.prototype%", « [[InitializedTemporalCalendar]], [[Identifier]] »).
    // 4. Set object.[[Identifier]] to the ASCII-lowercase of identifier.
    auto object = TRY(ordinary_create_from_constructor<Calendar>(vm, *new_target, &Intrinsics::temporal_calendar_prototype, TRY_OR_THROW_OOM(vm, identifier.to_lowercase())));

    // 5. Return object.
    return object.ptr();
}

// 12.2.2 GetBuiltinCalendar ( id ), https://tc39.es/proposal-temporal/#sec-temporal-getbuiltincalendar
ThrowCompletionOr<Calendar*> get_builtin_calendar(VM& vm, String const& identifier)
{
    // 1. If IsBuiltinCalendar(id) is false, throw a RangeError exception.
    if (!is_builtin_calendar(identifier))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarIdentifier, identifier);

    // 2. Return ! CreateTemporalCalendar(id).
    return MUST_OR_THROW_OOM(create_temporal_calendar(vm, identifier));
}

// 12.2.3 GetISO8601Calendar ( ), https://tc39.es/proposal-temporal/#sec-temporal-getiso8601calendar
Calendar* get_iso8601_calendar(VM& vm)
{
    // 1. Return ! GetBuiltinCalendar("iso8601").
    return MUST(get_builtin_calendar(vm, "iso8601"_string));
}

// 12.2.4 CalendarFields ( calendar, fieldNames ), https://tc39.es/proposal-temporal/#sec-temporal-calendarfields
ThrowCompletionOr<Vector<String>> calendar_fields(VM& vm, Object& calendar, Vector<StringView> const& field_names)
{
    auto& realm = *vm.current_realm();

    // 1. Let fields be ? GetMethod(calendar, "fields").
    auto fields = TRY(Value(&calendar).get_method(vm, vm.names.fields));

    // 2. If fields is undefined, return fieldNames.
    if (!fields) {
        Vector<String> result;
        TRY_OR_THROW_OOM(vm, result.try_ensure_capacity(field_names.size()));
        for (auto& value : field_names)
            result.unchecked_append(TRY_OR_THROW_OOM(vm, String::from_utf8(value)));
        return result;
    }

    // 3. Let fieldsArray be ? Call(fields, calendar, « CreateArrayFromList(fieldNames) »).
    auto field_names_array = Array::create_from<StringView>(realm, field_names, [&](auto value) {
        return PrimitiveString::create(vm, value);
    });
    auto fields_array = TRY(call(vm, *fields, &calendar, field_names_array));

    // 4. Return ? IterableToListOfType(fieldsArray, « String »).
    auto list = TRY(iterable_to_list_of_type(vm, fields_array, { OptionType::String }));

    Vector<String> result;
    TRY_OR_THROW_OOM(vm, result.try_ensure_capacity(list.size()));
    for (auto& value : list)
        result.unchecked_append(value.as_string().utf8_string());
    return result;
}

// 12.2.5 CalendarMergeFields ( calendar, fields, additionalFields ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmergefields
ThrowCompletionOr<Object*> calendar_merge_fields(VM& vm, Object& calendar, Object& fields, Object& additional_fields)
{
    // 1. Let mergeFields be ? GetMethod(calendar, "mergeFields").
    auto merge_fields = TRY(Value(&calendar).get_method(vm, vm.names.mergeFields));

    // 2. If mergeFields is undefined, then
    if (!merge_fields) {
        // a. Return ? DefaultMergeCalendarFields(fields, additionalFields).
        return TRY(default_merge_calendar_fields(vm, fields, additional_fields));
    }

    // 3. Let result be ? Call(mergeFields, calendar, « fields, additionalFields »).
    auto result = TRY(call(vm, merge_fields, &calendar, &fields, &additional_fields));

    // 4. If Type(result) is not Object, throw a TypeError exception.
    if (!result.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, result.to_string_without_side_effects());

    // 5. Return result.
    return &result.as_object();
}

// 12.2.6 CalendarDateAdd ( calendar, date, duration [ , options [ , dateAdd ] ] ), https://tc39.es/proposal-temporal/#sec-temporal-calendardateadd
ThrowCompletionOr<PlainDate*> calendar_date_add(VM& vm, Object& calendar, Value date, Duration& duration, Object* options, FunctionObject* date_add)
{
    // NOTE: `date` is a `Value` because we sometimes need to pass a PlainDate, sometimes a PlainDateTime, and sometimes undefined.

    // 1. Assert: Type(calendar) is Object.
    // 2. If options is not present, set options to undefined.
    // 3. Assert: Type(options) is Object or Undefined.

    // 4. If dateAdd is not present, set dateAdd to ? GetMethod(calendar, "dateAdd").
    if (!date_add)
        date_add = TRY(Value(&calendar).get_method(vm, vm.names.dateAdd));

    // 5. Let addedDate be ? Call(dateAdd, calendar, « date, duration, options »).
    auto added_date = TRY(call(vm, date_add ?: js_undefined(), &calendar, date, &duration, options ?: js_undefined()));

    // 6. Perform ? RequireInternalSlot(addedDate, [[InitializedTemporalDate]]).
    auto added_date_object = TRY(added_date.to_object(vm));
    if (!is<PlainDate>(*added_date_object))
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Temporal.PlainDate");

    // 7. Return addedDate.
    return static_cast<PlainDate*>(added_date_object.ptr());
}

// 12.2.7 CalendarDateUntil ( calendar, one, two, options [ , dateUntil ] ), https://tc39.es/proposal-temporal/#sec-temporal-calendardateuntil
ThrowCompletionOr<NonnullGCPtr<Duration>> calendar_date_until(VM& vm, CalendarMethods const& calendar_record, Value one, Value two, Object const& options)
{
    // 1. Let duration be ? CalendarMethodsRecordCall(calendarRec, DATE-UNTIL, « one, two, options »).
    auto duration = TRY(calendar_methods_record_call(vm, calendar_record, CalendarMethod::DateUntil, Vector<Value> { one, two, &options }));

    // 2. If CalendarMethodsRecordIsBuiltin(calendarRec) is true, return duration.
    if (calendar_methods_record_is_builtin(calendar_record))
        return verify_cast<Duration>(duration.as_object());

    // 3. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
    auto duration_object = TRY(duration.to_object(vm));
    if (!is<Duration>(*duration_object))
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Temporal.Duration");

    // 4. Return duration.
    return static_cast<Duration&>(duration.as_object());
}

// 12.2.8 CalendarYear ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendaryear
ThrowCompletionOr<double> calendar_year(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "year", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.year, &date_like));

    // 2. If result is undefined, throw a RangeError exception.
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.year.as_string(), vm.names.undefined.as_string());

    // 3. Return ? ToIntegerWithTruncation(result).
    return TRY(to_integer_with_truncation(vm, result, ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.year.as_string(), vm.names.Infinity.as_string()));
}

// 12.2.9 CalendarMonth ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmonth
ThrowCompletionOr<double> calendar_month(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "month", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.month, &date_like));

    // NOTE: Explicitly handled for a better error message similar to the other calendar property AOs
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.month.as_string(), vm.names.undefined.as_string());

    // 2. Return ? ToPositiveIntegerWithTruncation(result).
    return TRY(to_positive_integer_with_truncation(vm, result));
}

// 12.2.10 CalendarMonthCode ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmonthcode
ThrowCompletionOr<String> calendar_month_code(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "monthCode", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.monthCode, &date_like));

    // 2. If result is undefined, throw a RangeError exception.
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.monthCode.as_string(), vm.names.undefined.as_string());

    // 3. Return ? ToString(result).
    return result.to_string(vm);
}

// 12.2.11 CalendarDay ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendarday
ThrowCompletionOr<double> calendar_day(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "day", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.day, &date_like));

    // NOTE: Explicitly handled for a better error message similar to the other calendar property AOs
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.day.as_string(), vm.names.undefined.as_string());

    // 2. Return ? ToPositiveIntegerWithTruncation(result).
    return TRY(to_positive_integer_with_truncation(vm, result));
}

// 12.2.12 CalendarDayOfWeek ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendardayofweek
ThrowCompletionOr<double> calendar_day_of_week(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "dayOfWeek", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.dayOfWeek, &date_like));

    // NOTE: Explicitly handled for a better error message similar to the other calendar property AOs
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.dayOfWeek.as_string(), vm.names.undefined.as_string());

    // 2. Return ? ToPositiveIntegerWithTruncation(result).
    return TRY(to_positive_integer_with_truncation(vm, result));
}

// 12.2.13 CalendarDayOfYear ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendardayofyear
ThrowCompletionOr<double> calendar_day_of_year(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "dayOfYear", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.dayOfYear, &date_like));

    // NOTE: Explicitly handled for a better error message similar to the other calendar property AOs
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.dayOfYear.as_string(), vm.names.undefined.as_string());

    // 2. Return ? ToPositiveIntegerWithTruncation(result).
    return TRY(to_positive_integer_with_truncation(vm, result));
}

// 12.2.14 CalendarWeekOfYear ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendarweekofyear
ThrowCompletionOr<double> calendar_week_of_year(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "weekOfYear", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.weekOfYear, &date_like));

    // NOTE: Explicitly handled for a better error message similar to the other calendar property AOs
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.weekOfYear.as_string(), vm.names.undefined.as_string());

    // 2. Return ? ToPositiveIntegerWithTruncation(result).
    return TRY(to_positive_integer_with_truncation(vm, result));
}

// 12.2.15 CalendarYearOfWeek ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendaryearofweek
ThrowCompletionOr<double> calendar_year_of_week(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "yearOfWeek", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.yearOfWeek, &date_like));

    // 2. If result is undefined, throw a RangeError exception.
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.yearOfWeek.as_string(), vm.names.undefined.as_string());

    // 3. Return ? ToIntegerWithTruncation(result).
    return TRY(to_integer_with_truncation(vm, result, ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.yearOfWeek.as_string(), vm.names.Infinity.to_string()));
}

// 12.2.16 CalendarDaysInWeek ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendardaysinweek
ThrowCompletionOr<double> calendar_days_in_week(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "daysInWeek", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.daysInWeek, &date_like));

    // NOTE: Explicitly handled for a better error message similar to the other calendar property AOs
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.daysInWeek.as_string(), vm.names.undefined.as_string());

    // 2. Return ? ToPositiveIntegerWithTruncation(result).
    return TRY(to_positive_integer_with_truncation(vm, result));
}

// 12.2.17 CalendarDaysInMonth ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendardaysinmonth
ThrowCompletionOr<double> calendar_days_in_month(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "daysInMonth", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.daysInMonth, &date_like));

    // NOTE: Explicitly handled for a better error message similar to the other calendar property AOs
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.daysInMonth.as_string(), vm.names.undefined.as_string());

    // 2. Return ? ToPositiveIntegerWithTruncation(result).
    return TRY(to_positive_integer_with_truncation(vm, result));
}

// 12.2.18 CalendarDaysInYear ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendardaysinyear
ThrowCompletionOr<double> calendar_days_in_year(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "daysInYear", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.daysInYear, &date_like));

    // NOTE: Explicitly handled for a better error message similar to the other calendar property AOs
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.daysInYear.as_string(), vm.names.undefined.as_string());

    // 2. Return ? ToPositiveIntegerWithTruncation(result).
    return TRY(to_positive_integer_with_truncation(vm, result));
}

// 12.2.19 CalendarMonthsInYear ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmonthsinyear
ThrowCompletionOr<double> calendar_months_in_year(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "monthsInYear", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.monthsInYear, &date_like));

    // NOTE: Explicitly handled for a better error message similar to the other calendar property AOs
    if (result.is_undefined())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.monthsInYear.as_string(), vm.names.undefined.as_string());

    // 2. Return ? ToPositiveIntegerWithTruncation(result).
    return TRY(to_positive_integer_with_truncation(vm, result));
}

// 12.2.20 CalendarInLeapYear ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendarinleapyear
ThrowCompletionOr<Value> calendar_in_leap_year(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Let result be ? Invoke(calendar, "inLeapYear", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.inLeapYear, &date_like));

    // 2. Return ToBoolean(result).
    return result.to_boolean();
}

// 15.6.1.1 CalendarEra ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendarera
ThrowCompletionOr<Value> calendar_era(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Assert: Type(calendar) is Object.

    // 2. Let result be ? Invoke(calendar, "era", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.era, &date_like));

    // 3. If result is not undefined, set result to ? ToString(result).
    if (!result.is_undefined())
        result = PrimitiveString::create(vm, TRY(result.to_string(vm)));

    // 4. Return result.
    return result;
}

// 15.6.1.2 CalendarEraYear ( calendar, dateLike ), https://tc39.es/proposal-temporal/#sec-temporal-calendarerayear
ThrowCompletionOr<Value> calendar_era_year(VM& vm, Object& calendar, Object& date_like)
{
    // 1. Assert: Type(calendar) is Object.

    // 2. Let result be ? Invoke(calendar, "eraYear", « dateLike »).
    auto result = TRY(Value(&calendar).invoke(vm, vm.names.eraYear, &date_like));

    // 3. If result is not undefined, set result to ? ToIntegerWithTruncation(result).
    if (!result.is_undefined())
        result = Value(TRY(to_integer_with_truncation(vm, result, ErrorType::TemporalInvalidCalendarFunctionResult, vm.names.eraYear.as_string(), "Infinity"sv)));

    // 4. Return result.
    return result;
}

// 12.2.21 ToTemporalCalendar ( temporalCalendarLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalcalendar
ThrowCompletionOr<Object*> to_temporal_calendar(VM& vm, Value temporal_calendar_like)
{
    // 1. If Type(temporalCalendarLike) is Object, then
    if (temporal_calendar_like.is_object()) {
        auto& temporal_calendar_like_object = temporal_calendar_like.as_object();

        // a. If temporalCalendarLike has an [[InitializedTemporalCalendar]] internal slot, then
        if (is<Calendar>(temporal_calendar_like_object)) {
            // i. Return temporalCalendarLike.
            return &temporal_calendar_like_object;
        }

        // b. If temporalCalendarLike has an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]], [[InitializedTemporalTime]], [[InitializedTemporalYearMonth]], or [[InitializedTemporalZonedDateTime]] internal slot, then
        // i. Return temporalCalendarLike.[[Calendar]].
        if (is<PlainDate>(temporal_calendar_like_object))
            return &static_cast<PlainDate&>(temporal_calendar_like_object).calendar();
        if (is<PlainDateTime>(temporal_calendar_like_object))
            return &static_cast<PlainDateTime&>(temporal_calendar_like_object).calendar();
        if (is<PlainMonthDay>(temporal_calendar_like_object))
            return &static_cast<PlainMonthDay&>(temporal_calendar_like_object).calendar();
        if (is<PlainTime>(temporal_calendar_like_object))
            return &static_cast<PlainTime&>(temporal_calendar_like_object).calendar();
        if (is<PlainYearMonth>(temporal_calendar_like_object))
            return &static_cast<PlainYearMonth&>(temporal_calendar_like_object).calendar();
        if (is<ZonedDateTime>(temporal_calendar_like_object))
            return &static_cast<ZonedDateTime&>(temporal_calendar_like_object).calendar();

        // c. If temporalCalendarLike has an [[InitializedTemporalTimeZone]] internal slot, throw a RangeError exception.
        if (is<TimeZone>(temporal_calendar_like_object))
            return vm.throw_completion<RangeError>(ErrorType::TemporalUnexpectedTimeZoneObject);

        // d. If ? HasProperty(temporalCalendarLike, "calendar") is false, return temporalCalendarLike.
        if (!TRY(temporal_calendar_like_object.has_property(vm.names.calendar)))
            return &temporal_calendar_like_object;

        // e. Set temporalCalendarLike to ? Get(temporalCalendarLike, "calendar").
        temporal_calendar_like = TRY(temporal_calendar_like_object.get(vm.names.calendar));

        // f. If Type(temporalCalendarLike) is Object, then
        if (temporal_calendar_like.is_object()) {
            // i. If temporalCalendarLike has an [[InitializedTemporalTimeZone]] internal slot, throw a RangeError exception.
            if (is<TimeZone>(temporal_calendar_like.as_object()))
                return vm.throw_completion<RangeError>(ErrorType::TemporalUnexpectedTimeZoneObject);

            // ii. If ? HasProperty(temporalCalendarLike, "calendar") is false, return temporalCalendarLike.
            if (!TRY(temporal_calendar_like.as_object().has_property(vm.names.calendar)))
                return &temporal_calendar_like.as_object();
        }
    }

    // 2. Let identifier be ? ToString(temporalCalendarLike).
    auto identifier = TRY(temporal_calendar_like.to_string(vm));

    // 3. Set identifier to ? ParseTemporalCalendarString(identifier).
    identifier = TRY(parse_temporal_calendar_string(vm, identifier));

    // 4. If IsBuiltinCalendar(identifier) is false, throw a RangeError exception.
    if (!is_builtin_calendar(identifier))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarIdentifier, identifier);

    // 5. Return ! CreateTemporalCalendar(identifier).
    return MUST_OR_THROW_OOM(create_temporal_calendar(vm, identifier));
}

// 12.2.22 ToTemporalCalendarWithISODefault ( temporalCalendarLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalcalendarwithisodefault
ThrowCompletionOr<Object*> to_temporal_calendar_with_iso_default(VM& vm, Value temporal_calendar_like)
{
    // 1. If temporalCalendarLike is undefined, then
    if (temporal_calendar_like.is_undefined()) {
        // a. Return ! GetISO8601Calendar().
        return get_iso8601_calendar(vm);
    }
    // 2. Return ? ToTemporalCalendar(temporalCalendarLike).
    return to_temporal_calendar(vm, temporal_calendar_like);
}

// 12.2.23 GetTemporalCalendarWithISODefault ( item ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalcalendarwithisodefault
ThrowCompletionOr<Object*> get_temporal_calendar_with_iso_default(VM& vm, Object& item)
{
    // 1. If item has an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]], [[InitializedTemporalTime]], [[InitializedTemporalYearMonth]], or [[InitializedTemporalZonedDateTime]] internal slot, then
    // a. Return item.[[Calendar]].
    if (is<PlainDate>(item))
        return &static_cast<PlainDate&>(item).calendar();
    if (is<PlainDateTime>(item))
        return &static_cast<PlainDateTime&>(item).calendar();
    if (is<PlainMonthDay>(item))
        return &static_cast<PlainMonthDay&>(item).calendar();
    if (is<PlainTime>(item))
        return &static_cast<PlainTime&>(item).calendar();
    if (is<PlainYearMonth>(item))
        return &static_cast<PlainYearMonth&>(item).calendar();
    if (is<ZonedDateTime>(item))
        return &static_cast<ZonedDateTime&>(item).calendar();

    // 2. Let calendarLike be ? Get(item, "calendar").
    auto calendar_like = TRY(item.get(vm.names.calendar));

    // 3. Return ? ToTemporalCalendarWithISODefault(calendarLike).
    return to_temporal_calendar_with_iso_default(vm, calendar_like);
}

// 12.2.24 CalendarDateFromFields ( calendar, fields [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-calendardatefromfields
ThrowCompletionOr<PlainDate*> calendar_date_from_fields(VM& vm, Object& calendar, Object const& fields, Object const* options)
{
    // 1. If options is not present, set options to undefined.

    // 2. Let date be ? Invoke(calendar, "dateFromFields", « fields, options »).
    auto date = TRY(Value(&calendar).invoke(vm, vm.names.dateFromFields, &fields, options ?: js_undefined()));

    // 3. Perform ? RequireInternalSlot(date, [[InitializedTemporalDate]]).
    auto date_object = TRY(date.to_object(vm));
    if (!is<PlainDate>(*date_object))
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Temporal.PlainDate");

    // 4. Return date.
    return static_cast<PlainDate*>(date_object.ptr());
}

// 12.2.25 CalendarYearMonthFromFields ( calendar, fields [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-calendaryearmonthfromfields
ThrowCompletionOr<PlainYearMonth*> calendar_year_month_from_fields(VM& vm, Object& calendar, Object const& fields, Object const* options)
{
    // 1. If options is not present, set options to undefined.

    // 2. Let yearMonth be ? Invoke(calendar, "yearMonthFromFields", « fields, options »).
    auto year_month = TRY(Value(&calendar).invoke(vm, vm.names.yearMonthFromFields, &fields, options ?: js_undefined()));

    // 3. Perform ? RequireInternalSlot(yearMonth, [[InitializedTemporalYearMonth]]).
    auto year_month_object = TRY(year_month.to_object(vm));
    if (!is<PlainYearMonth>(*year_month_object))
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Temporal.PlainYearMonth");

    // 4. Return yearMonth.
    return static_cast<PlainYearMonth*>(year_month_object.ptr());
}

// 12.2.26 CalendarMonthDayFromFields ( calendar, fields [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmonthdayfromfields
ThrowCompletionOr<PlainMonthDay*> calendar_month_day_from_fields(VM& vm, Object& calendar, Object const& fields, Object const* options)
{
    // 1. If options is not present, set options to undefined.

    // 2. Let monthDay be ? Invoke(calendar, "monthDayFromFields", « fields, options »).
    auto month_day = TRY(Value(&calendar).invoke(vm, vm.names.monthDayFromFields, &fields, options ?: js_undefined()));

    // 3. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day_object = TRY(month_day.to_object(vm));
    if (!is<PlainMonthDay>(*month_day_object))
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Temporal.PlainMonthDay");

    // 4. Return monthDay.
    return static_cast<PlainMonthDay*>(month_day_object.ptr());
}

// 12.2.27 MaybeFormatCalendarAnnotation ( calendarObject, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-maybeformatcalendarannotation
ThrowCompletionOr<String> maybe_format_calendar_annotation(VM& vm, Object const* calendar_object, StringView show_calendar)
{
    // 1. If showCalendar is "never", return the empty String.
    if (show_calendar == "never"sv)
        return String {};

    // 2. Assert: Type(calendarObject) is Object.
    VERIFY(calendar_object);

    // 3. Let calendarID be ? ToString(calendarObject).
    auto calendar_id = TRY(Value(calendar_object).to_string(vm));

    // 4. Return FormatCalendarAnnotation(calendarID, showCalendar).
    return format_calendar_annotation(vm, calendar_id, show_calendar);
}

// 12.2.28 FormatCalendarAnnotation ( id, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-formatcalendarannotation
ThrowCompletionOr<String> format_calendar_annotation(VM& vm, StringView id, StringView show_calendar)
{
    VERIFY(show_calendar == "auto"sv || show_calendar == "always"sv || show_calendar == "never"sv || show_calendar == "critical"sv);

    // 1. If showCalendar is "never", return the empty String.
    if (show_calendar == "never"sv)
        return String {};

    // 2. If showCalendar is "auto" and id is "iso8601", return the empty String.
    if (show_calendar == "auto"sv && id == "iso8601"sv)
        return String {};

    // 3. If showCalendar is "critical", let flag be "!"; else, let flag be the empty String.
    auto flag = show_calendar == "critical"sv ? "!"sv : ""sv;

    // 4. Return the string-concatenation of "[", flag, "u-ca=", id, and "]".
    return TRY_OR_THROW_OOM(vm, String::formatted("[{}u-ca={}]", flag, id));
}

// 12.2.29 CalendarEquals ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-calendarequals
ThrowCompletionOr<bool> calendar_equals(VM& vm, Object& one, Object& two)
{
    // 1. If one and two are the same Object value, return true.
    if (&one == &two)
        return true;

    // 2. Let calendarOne be ? ToString(one).
    auto calendar_one = TRY(Value(&one).to_string(vm));

    // 3. Let calendarTwo be ? ToString(two).
    auto calendar_two = TRY(Value(&two).to_string(vm));

    // 4. If calendarOne is calendarTwo, return true.
    if (calendar_one == calendar_two)
        return true;

    // 5. Return false.
    return false;
}

// 12.2.30 ConsolidateCalendars ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-consolidatecalendars
ThrowCompletionOr<Object*> consolidate_calendars(VM& vm, Object& one, Object& two)
{
    // 1. If one and two are the same Object value, return two.
    if (&one == &two)
        return &two;

    // 2. Let calendarOne be ? ToString(one).
    auto calendar_one = TRY(Value(&one).to_string(vm));

    // 3. Let calendarTwo be ? ToString(two).
    auto calendar_two = TRY(Value(&two).to_string(vm));

    // 4. If calendarOne is calendarTwo, return two.
    if (calendar_one == calendar_two)
        return &two;

    // 5. If calendarOne is "iso8601", return two.
    if (calendar_one == "iso8601"sv)
        return &two;

    // 6. If calendarTwo is "iso8601", return one.
    if (calendar_two == "iso8601"sv)
        return &one;

    // 7. Throw a RangeError exception.
    return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendar);
}

// 12.2.31 ISODaysInMonth ( year, month ), https://tc39.es/proposal-temporal/#sec-temporal-isodaysinmonth
u8 iso_days_in_month(i32 year, u8 month)
{
    // 1. If month is 1, 3, 5, 7, 8, 10, or 12, return 31.
    if (month == 1 || month == 3 || month == 5 || month == 7 || month == 8 || month == 10 || month == 12)
        return 31;

    // 2. If month is 4, 6, 9, or 11, return 30.
    if (month == 4 || month == 6 || month == 9 || month == 11)
        return 30;

    // 3. Assert: month is 2.
    VERIFY(month == 2);

    // 4. Return 28 + ℝ(InLeapYear(TimeFromYear(𝔽(year)))).
    return 28 + JS::in_leap_year(time_from_year(year));
}

// 12.2.32 ToISOWeekOfYear ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-toisoweekofyear
YearWeekRecord to_iso_week_of_year(i32 year, u8 month, u8 day)
{
    // 1. Assert: IsValidISODate(year, month, day) is true.
    VERIFY(is_valid_iso_date(year, month, day));

    // 2. Let wednesday be 3.
    constexpr auto wednesday = 3;

    // 3. Let thursday be 4.
    constexpr auto thursday = 4;

    // 4. Let friday be 5.
    constexpr auto friday = 5;

    // 5. Let saturday be 6.
    constexpr auto saturday = 6;

    // 6. Let daysInWeek be 7.
    constexpr auto days_in_week = 7;

    // 7. Let maxWeekNumber be 53.
    constexpr auto max_week_number = 53;

    // 8. Let dayOfYear be ToISODayOfYear(year, month, day).
    auto day_of_year = to_iso_day_of_year(year, month, day);

    // 9. Let dayOfWeek be ToISODayOfWeek(year, month, day).
    auto day_of_week = to_iso_day_of_week(year, month, day);

    // 10. Let week be floor((dayOfYear + daysInWeek - dayOfWeek + wednesday ) / daysInWeek).
    auto week = static_cast<i32>(floor(static_cast<double>(day_of_year + days_in_week - day_of_week + wednesday) / days_in_week));

    // 11. If week < 1, then
    if (week < 1) {
        // a. NOTE: This is the last week of the previous year.

        // b. Let dayOfJan1st be ToISODayOfWeek(year, 1, 1).
        auto day_of_jan_1st = to_iso_day_of_week(year, 1, 1);

        // c. If dayOfJan1st is friday, then
        if (day_of_jan_1st == friday) {
            // i. Return the Year-Week Record { [[Week]]: maxWeekNumber, [[Year]]: year - 1 }.
            return YearWeekRecord { .week = max_week_number, .year = year - 1 };
        }

        // d. If dayOfJan1st is saturday, and InLeapYear(TimeFromYear(𝔽(year - 1))) is 1𝔽, then
        if (day_of_jan_1st == saturday && in_leap_year(time_from_year(year - 1))) {
            // i. Return the Year-Week Record { [[Week]]: maxWeekNumber. [[Year]]: year - 1 }.
            return YearWeekRecord { .week = max_week_number, .year = year - 1 };
        }

        // e. Return the Year-Week Record { [[Week]]: maxWeekNumber - 1, [[Year]]: year - 1 }.
        return YearWeekRecord { .week = max_week_number - 1, .year = year - 1 };
    }

    // 12. If week is maxWeekNumber, then
    if (week == max_week_number) {
        // a. Let daysInYear be DaysInYear(𝔽(year)).
        auto days_in_year = JS::days_in_year(year);

        // b. Let daysLaterInYear be daysInYear - dayOfYear.
        auto days_later_in_year = days_in_year - day_of_year;

        // c. Let daysAfterThursday be thursday - dayOfWeek.
        auto days_after_thursday = thursday - day_of_week;

        // d. If daysLaterInYear < daysAfterThursday, then
        if (days_later_in_year < days_after_thursday) {
            // i. Return the Year-Week Record { [[Week]]: 1, [[Year]]: year + 1 }.
            return YearWeekRecord { .week = 1, .year = year + 1 };
        }
    }

    // 13. Return the Year-Week Record { [[Week]]: week, [[Year]]: year }.
    return YearWeekRecord { .week = static_cast<u8>(week), .year = year };
}

// 12.2.33 ISOMonthCode ( month ), https://tc39.es/proposal-temporal/#sec-temporal-isomonthcode
ThrowCompletionOr<String> iso_month_code(VM& vm, u8 month)
{
    // 1. Let numberPart be ToZeroPaddedDecimalString(month, 2).
    // 2. Return the string-concatenation of "M" and numberPart.
    return TRY_OR_THROW_OOM(vm, String::formatted("M{:02}", month));
}

// 12.2.34 ResolveISOMonth ( fields ), https://tc39.es/proposal-temporal/#sec-temporal-resolveisomonth
ThrowCompletionOr<double> resolve_iso_month(VM& vm, Object const& fields)
{
    // 1. Assert: fields is an ordinary object with no more and no less than the own data properties listed in Table 13.

    // 2. Let month be ! Get(fields, "month").
    auto month = MUST(fields.get(vm.names.month));

    // 3. Assert: month is undefined or month is a Number.
    VERIFY(month.is_undefined() || month.is_number());

    // 4. Let monthCode be ! Get(fields, "monthCode").
    auto month_code = MUST(fields.get(vm.names.monthCode));

    // 5. If monthCode is undefined, then
    if (month_code.is_undefined()) {
        // a. If month is undefined, throw a TypeError exception.
        if (month.is_undefined())
            return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, vm.names.month.as_string());

        // b. Return ℝ(month).
        return month.as_double();
    }

    // 6. Assert: Type(monthCode) is String.
    VERIFY(month_code.is_string());
    auto month_code_string = month_code.as_string().byte_string();

    // 7. If the length of monthCode is not 3, throw a RangeError exception.
    auto month_length = month_code_string.length();
    if (month_length != 3)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    // 8. If the first code unit of monthCode is not 0x004D (LATIN CAPITAL LETTER M), throw a RangeError exception.
    if (month_code_string[0] != 0x4D)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    // 9. Let monthCodeDigits be the substring of monthCode from 1.
    auto month_code_digits = month_code_string.substring(1);

    // 10. If ParseText(StringToCodePoints(monthCodeDigits), DateMonth) is a List of errors, throw a RangeError exception.
    auto parse_result = parse_iso8601(Production::DateMonth, month_code_digits);
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    // 11. Let monthCodeNumber be ! ToIntegerOrInfinity(monthCodeDigits).
    auto month_code_number = MUST(Value(PrimitiveString::create(vm, move(month_code_digits))).to_integer_or_infinity(vm));

    // 12. Assert: SameValue(monthCode, ISOMonthCode(monthCodeNumber)) is true.
    VERIFY(month_code_string.view() == TRY(iso_month_code(vm, month_code_number)));

    // 13. If month is not undefined and SameValue(month, monthCodeNumber) is false, throw a RangeError exception.
    if (!month.is_undefined() && month.as_double() != month_code_number)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    // 14. Return monthCodeNumber.
    return month_code_number;
}

// 12.2.35 ISODateFromFields ( fields, options ), https://tc39.es/proposal-temporal/#sec-temporal-isodatefromfields
ThrowCompletionOr<ISODateRecord> iso_date_from_fields(VM& vm, Object const& fields, Object const& options)
{
    // 1. Assert: Type(fields) is Object.

    // 2. Set fields to ? PrepareTemporalFields(fields, « "day", "month", "monthCode", "year" », « "year", "day" »).
    auto* prepared_fields = TRY(prepare_temporal_fields(vm, fields,
        { "day"_string,
            "month"_string,
            "monthCode"_string,
            "year"_string },
        Vector<StringView> { "year"sv, "day"sv }));

    // 3. Let overflow be ? ToTemporalOverflow(options).
    auto overflow = TRY(to_temporal_overflow(vm, &options));

    // 4. Let year be ! Get(fields, "year").
    auto year = MUST(prepared_fields->get(vm.names.year));

    // 5. Assert: Type(year) is Number.
    VERIFY(year.is_number());

    // 6. Let month be ? ResolveISOMonth(fields).
    auto month = TRY(resolve_iso_month(vm, *prepared_fields));

    // 7. Let day be ! Get(fields, "day").
    auto day = MUST(prepared_fields->get(vm.names.day));

    // 8. Assert: Type(day) is Number.
    VERIFY(day.is_number());

    // 9. Return ? RegulateISODate(ℝ(year), month, ℝ(day), overflow).
    return regulate_iso_date(vm, year.as_double(), month, day.as_double(), overflow);
}

// 12.2.36 ISOYearMonthFromFields ( fields, options ), https://tc39.es/proposal-temporal/#sec-temporal-isoyearmonthfromfields
ThrowCompletionOr<ISOYearMonth> iso_year_month_from_fields(VM& vm, Object const& fields, Object const& options)
{
    // 1. Assert: Type(fields) is Object.

    // 2. Set fields to ? PrepareTemporalFields(fields, « "month", "monthCode", "year" », « "year" »).
    auto* prepared_fields = TRY(prepare_temporal_fields(vm, fields,
        { "month"_string,
            "monthCode"_string,
            "year"_string },
        Vector<StringView> { "year"sv }));

    // 3. Let overflow be ? ToTemporalOverflow(options).
    auto overflow = TRY(to_temporal_overflow(vm, &options));

    // 4. Let year be ! Get(fields, "year").
    auto year = MUST(prepared_fields->get(vm.names.year));

    // 5. Assert: Type(year) is Number.
    VERIFY(year.is_number());

    // 6. Let month be ? ResolveISOMonth(fields).
    auto month = TRY(resolve_iso_month(vm, *prepared_fields));

    // 7. Let result be ? RegulateISOYearMonth(ℝ(year), month, overflow).
    auto result = TRY(regulate_iso_year_month(vm, year.as_double(), month, overflow));

    // 8. Return the Record { [[Year]]: result.[[Year]], [[Month]]: result.[[Month]], [[ReferenceISODay]]: 1 }.
    return ISOYearMonth { .year = result.year, .month = result.month, .reference_iso_day = 1 };
}

// 12.2.37 ISOMonthDayFromFields ( fields, options ), https://tc39.es/proposal-temporal/#sec-temporal-isomonthdayfromfields
ThrowCompletionOr<ISOMonthDay> iso_month_day_from_fields(VM& vm, Object const& fields, Object const& options)
{
    // 1. Assert: Type(fields) is Object.

    // 2. Set fields to ? PrepareTemporalFields(fields, « "day", "month", "monthCode", "year" », « "day" »).
    auto* prepared_fields = TRY(prepare_temporal_fields(vm, fields,
        { "day"_string,
            "month"_string,
            "monthCode"_string,
            "year"_string },
        Vector<StringView> { "day"sv }));

    // 3. Let overflow be ? ToTemporalOverflow(options).
    auto overflow = TRY(to_temporal_overflow(vm, &options));

    // 4. Let month be ! Get(fields, "month").
    auto month_value = MUST(prepared_fields->get(vm.names.month));

    // 5. Let monthCode be ! Get(fields, "monthCode").
    auto month_code = MUST(prepared_fields->get(vm.names.monthCode));

    // 6. Let year be ! Get(fields, "year").
    auto year = MUST(prepared_fields->get(vm.names.year));

    // 7. If month is not undefined, and monthCode and year are both undefined, then
    if (!month_value.is_undefined() && month_code.is_undefined() && year.is_undefined()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "monthCode or year");
    }

    // 8. Set month to ? ResolveISOMonth(fields).
    auto month = TRY(resolve_iso_month(vm, *prepared_fields));

    // 9. Let day be ! Get(fields, "day").
    auto day = MUST(prepared_fields->get(vm.names.day));

    // 10. Assert: Type(day) is Number.
    VERIFY(day.is_number());

    // 11. Let referenceISOYear be 1972 (the first leap year after the Unix epoch).
    i32 reference_iso_year = 1972;

    Optional<ISODateRecord> result;

    // 12. If monthCode is undefined, then
    if (month_code.is_undefined()) {
        // a. Assert: Type(year) is Number.
        VERIFY(year.is_number());

        // b. Let result be ? RegulateISODate(ℝ(year), month, ℝ(day), overflow).
        result = TRY(regulate_iso_date(vm, year.as_double(), month, day.as_double(), overflow));
    }
    // 13. Else,
    else {
        // a. Let result be ? RegulateISODate(referenceISOYear, month, ℝ(day), overflow).
        result = TRY(regulate_iso_date(vm, reference_iso_year, month, day.as_double(), overflow));
    }

    // 14. Return the Record { [[Month]]: result.[[Month]], [[Day]]: result.[[Day]], [[ReferenceISOYear]]: referenceISOYear }.
    return ISOMonthDay { .month = result->month, .day = result->day, .reference_iso_year = reference_iso_year };
}

// 12.2.38 DefaultMergeCalendarFields ( fields, additionalFields ), https://tc39.es/proposal-temporal/#sec-temporal-defaultmergecalendarfields
ThrowCompletionOr<Object*> default_merge_calendar_fields(VM& vm, Object const& fields, Object const& additional_fields)
{
    auto& realm = *vm.current_realm();

    // 1. Let merged be OrdinaryObjectCreate(%Object.prototype%).
    auto merged = Object::create(realm, realm.intrinsics().object_prototype());

    // 2. Let fieldsKeys be ? EnumerableOwnPropertyNames(fields, key).
    auto fields_keys = TRY(fields.enumerable_own_property_names(Object::PropertyKind::Key));

    // 3. For each element key of fieldsKeys, do
    for (auto& key : fields_keys) {
        // a. If key is not "month" or "monthCode", then
        if (!key.as_string().byte_string().is_one_of(vm.names.month.as_string(), vm.names.monthCode.as_string())) {
            auto property_key = MUST(PropertyKey::from_value(vm, key));

            // i. Let propValue be ? Get(fields, key).
            auto prop_value = TRY(fields.get(property_key));

            // ii. If propValue is not undefined, then
            if (!prop_value.is_undefined()) {
                // 1. Perform ! CreateDataPropertyOrThrow(merged, key, propValue).
                MUST(merged->create_data_property_or_throw(property_key, prop_value));
            }
        }
    }

    // 4. Let additionalFieldsKeys be ? EnumerableOwnPropertyNames(additionalFields, key).
    auto additional_fields_keys = TRY(additional_fields.enumerable_own_property_names(Object::PropertyKind::Key));

    // IMPLEMENTATION DEFINED: This is an optimization, so we don't have to iterate new_keys three times (worst case), but only once.
    bool additional_fields_keys_contains_month_or_month_code_property = false;

    // 5. For each element key of additionalFieldsKeys, do
    for (auto& key : additional_fields_keys) {
        auto property_key = MUST(PropertyKey::from_value(vm, key));

        // a. Let propValue be ? Get(additionalFields, key).
        auto prop_value = TRY(additional_fields.get(property_key));

        // b. If propValue is not undefined, then
        if (!prop_value.is_undefined()) {
            // i. Perform ! CreateDataPropertyOrThrow(merged, key, propValue).
            MUST(merged->create_data_property_or_throw(property_key, prop_value));
        }

        // See comment above.
        additional_fields_keys_contains_month_or_month_code_property |= key.as_string().byte_string() == vm.names.month.as_string() || key.as_string().byte_string() == vm.names.monthCode.as_string();
    }

    // 6. If additionalFieldsKeys does not contain either "month" or "monthCode", then
    if (!additional_fields_keys_contains_month_or_month_code_property) {
        // a. Let month be ? Get(fields, "month").
        auto month = TRY(fields.get(vm.names.month));

        // b. If month is not undefined, then
        if (!month.is_undefined()) {
            // i. Perform ! CreateDataPropertyOrThrow(merged, "month", month).
            MUST(merged->create_data_property_or_throw(vm.names.month, month));
        }

        // c. Let monthCode be ? Get(fields, "monthCode").
        auto month_code = TRY(fields.get(vm.names.monthCode));

        // d. If monthCode is not undefined, then
        if (!month_code.is_undefined()) {
            // i. Perform ! CreateDataPropertyOrThrow(merged, "monthCode", monthCode).
            MUST(merged->create_data_property_or_throw(vm.names.monthCode, month_code));
        }
    }

    // 7. Return merged.
    return merged.ptr();
}

// 12.2.39 ToISODayOfYear ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-toisodayofyear
u16 to_iso_day_of_year(i32 year, u8 month, u8 day)
{
    // 1. Assert: IsValidISODate(year, month, day) is true.
    VERIFY(is_valid_iso_date(year, month, day));

    // 2. Let epochDays be MakeDay(𝔽(year), 𝔽(month - 1), 𝔽(day)).
    auto epoch_days = make_day(year, month - 1, day);

    // 3. Assert: epochDays is finite.
    VERIFY(isfinite(epoch_days));

    // 4. Return ℝ(DayWithinYear(MakeDate(epochDays, +0𝔽))) + 1.
    return day_within_year(make_date(epoch_days, 0)) + 1;
}

// 12.2.40 ToISODayOfWeek ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-toisodayofweek
u8 to_iso_day_of_week(i32 year, u8 month, u8 day)
{
    // 1. Assert: IsValidISODate(year, month, day) is true.
    VERIFY(is_valid_iso_date(year, month, day));

    // 2. Let epochDays be MakeDay(𝔽(year), 𝔽(month - 1), 𝔽(day)).
    auto epoch_days = make_day(year, month - 1, day);

    // 3. Assert: epochDays is finite.
    VERIFY(isfinite(epoch_days));

    // 4. Let dayOfWeek be WeekDay(MakeDate(epochDays, +0𝔽)).
    auto day_of_week = week_day(make_date(epoch_days, 0));

    // 5. If dayOfWeek = +0𝔽, return 7.
    if (day_of_week == 0)
        return 7;

    // 6. Return ℝ(dayOfWeek).
    return day_of_week;
}

}
