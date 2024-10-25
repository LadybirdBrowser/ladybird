/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainMonthDayPrototype.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(PlainMonthDayPrototype);

// 10.3 Properties of the Temporal.PlainMonthDay Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plainmonthday-prototype-object
PlainMonthDayPrototype::PlainMonthDayPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void PlainMonthDayPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 10.3.2 Temporal.PlainMonthDay.prototype[ @@toStringTag ], https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.PlainMonthDay"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.calendar, calendar_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.calendarId, calendar_id_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.monthCode, month_code_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.day, day_getter, {}, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.with, with, 1, attr);
    define_native_function(realm, vm.names.equals, equals, 1, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
    define_native_function(realm, vm.names.valueOf, value_of, 0, attr);
    define_native_function(realm, vm.names.toPlainDate, to_plain_date, 1, attr);
    define_native_function(realm, vm.names.getISOFields, get_iso_fields, 0, attr);
}

// 10.3.3 get Temporal.PlainMonthDay.prototype.calendar, https://tc39.es/proposal-temporal/#sec-get-temporal.plainmonthday.prototype.calendar
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::calendar_getter)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Return monthDay.[[Calendar]].
    return Value(&month_day->calendar());
}

// 10.3.4 get Temporal.PlainMonthDay.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.plainmonthday.prototype.monthcode
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::month_code_getter)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Let calendar be monthDay.[[Calendar]].
    auto& calendar = month_day->calendar();

    // 4. Return ? CalendarMonthCode(calendar, monthDay).
    return PrimitiveString::create(vm, TRY(calendar_month_code(vm, calendar, month_day)));
}

// 10.3.5 get Temporal.PlainMonthDay.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.plainmonthday.prototype.day
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::day_getter)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Let calendar be monthDay.[[Calendar]].
    auto& calendar = month_day->calendar();

    // 4. Return 𝔽(? CalendarDay(calendar, monthDay)).
    return Value(TRY(calendar_day(vm, calendar, month_day)));
}

// 10.3.6 Temporal.PlainMonthDay.prototype.with ( temporalMonthDayLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.with
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::with)
{
    auto temporal_month_day_like = vm.argument(0);

    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. If Type(temporalMonthDayLike) is not Object, then
    if (!temporal_month_day_like.is_object()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, temporal_month_day_like.to_string_without_side_effects());
    }

    // 4. Perform ? RejectObjectWithCalendarOrTimeZone(temporalMonthDayLike).
    TRY(reject_object_with_calendar_or_time_zone(vm, temporal_month_day_like.as_object()));

    // 5. Let calendar be monthDay.[[Calendar]].
    auto& calendar = month_day->calendar();

    // 6. Let fieldNames be ? CalendarFields(calendar, « "day", "month", "monthCode", "year" »).
    auto field_names = TRY(calendar_fields(vm, calendar, { "day"sv, "month"sv, "monthCode"sv, "year"sv }));

    // 7. Let partialMonthDay be ? PrepareTemporalFields(temporalMonthDayLike, fieldNames, partial).
    auto* partial_month_day = TRY(prepare_temporal_fields(vm, temporal_month_day_like.as_object(), field_names, PrepareTemporalFieldsPartial {}));

    // 8. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(1)));

    // 9. Let fields be ? PrepareTemporalFields(monthDay, fieldNames, «»).
    auto* fields = TRY(prepare_temporal_fields(vm, month_day, field_names, Vector<StringView> {}));

    // 10. Set fields to ? CalendarMergeFields(calendar, fields, partialMonthDay).
    fields = TRY(calendar_merge_fields(vm, calendar, *fields, *partial_month_day));

    // 11. Set fields to ? PrepareTemporalFields(fields, fieldNames, «»).
    fields = TRY(prepare_temporal_fields(vm, *fields, field_names, Vector<StringView> {}));

    // 12. Return ? CalendarMonthDayFromFields(calendar, fields, options).
    return TRY(calendar_month_day_from_fields(vm, calendar, *fields, options));
}

// 10.3.7 Temporal.PlainMonthDay.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::equals)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalMonthDay(other).
    auto* other = TRY(to_temporal_month_day(vm, vm.argument(0)));

    // 4. If monthDay.[[ISOMonth]] ≠ other.[[ISOMonth]], return false.
    if (month_day->iso_month() != other->iso_month())
        return Value(false);

    // 5. If monthDay.[[ISODay]] ≠ other.[[ISODay]], return false.
    if (month_day->iso_day() != other->iso_day())
        return Value(false);

    // 6. If monthDay.[[ISOYear]] ≠ other.[[ISOYear]], return false.
    if (month_day->iso_year() != other->iso_year())
        return Value(false);

    // 7. Return ? CalendarEquals(monthDay.[[Calendar]], other.[[Calendar]]).
    return Value(TRY(calendar_equals(vm, month_day->calendar(), other->calendar())));
}

// 10.3.8 Temporal.PlainMonthDay.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::to_string)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. Let showCalendar be ? ToCalendarNameOption(options).
    auto show_calendar = TRY(to_calendar_name_option(vm, *options));

    // 5. Return ? TemporalMonthDayToString(monthDay, showCalendar).
    return PrimitiveString::create(vm, TRY(temporal_month_day_to_string(vm, month_day, show_calendar)));
}

// 10.3.9 Temporal.PlainMonthDay.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.tolocalestring
// NOTE: This is the minimum toLocaleString implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::to_locale_string)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Return ? TemporalMonthDayToString(monthDay, "auto").
    return PrimitiveString::create(vm, TRY(temporal_month_day_to_string(vm, month_day, "auto"sv)));
}

// 10.3.10 Temporal.PlainMonthDay.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::to_json)
{
    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Return ? TemporalMonthDayToString(monthDay, "auto").
    return PrimitiveString::create(vm, TRY(temporal_month_day_to_string(vm, month_day, "auto"sv)));
}

// 10.3.11 Temporal.PlainMonthDay.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.PlainMonthDay", "a primitive value");
}

// 10.3.12 Temporal.PlainMonthDay.prototype.toPlainDate ( item ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.toplaindate
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::to_plain_date)
{
    auto& realm = *vm.current_realm();

    auto item = vm.argument(0);

    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. If Type(item) is not Object, then
    if (!item.is_object()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, item);
    }

    // 4. Let calendar be monthDay.[[Calendar]].
    auto& calendar = month_day->calendar();

    // 5. Let receiverFieldNames be ? CalendarFields(calendar, « "day", "monthCode" »).
    auto receiver_field_names = TRY(calendar_fields(vm, calendar, { "day"sv, "monthCode"sv }));

    // 6. Let fields be ? PrepareTemporalFields(monthDay, receiverFieldNames, «»).
    auto* fields = TRY(prepare_temporal_fields(vm, month_day, receiver_field_names, Vector<StringView> {}));

    // 7. Let inputFieldNames be ? CalendarFields(calendar, « "year" »).
    auto input_field_names = TRY(calendar_fields(vm, calendar, { "year"sv }));

    // 8. Let inputFields be ? PrepareTemporalFields(item, inputFieldNames, «»).
    auto* input_fields = TRY(prepare_temporal_fields(vm, item.as_object(), input_field_names, Vector<StringView> {}));

    // 9. Let mergedFields be ? CalendarMergeFields(calendar, fields, inputFields).
    auto* merged_fields = TRY(calendar_merge_fields(vm, calendar, *fields, *input_fields));

    // 10. Let mergedFieldNames be MergeLists(receiverFieldNames, inputFieldNames).
    auto merged_field_names = merge_lists(receiver_field_names, input_field_names);

    // 11. Set mergedFields to ? PrepareTemporalFields(mergedFields, mergedFieldNames, «»).
    merged_fields = TRY(prepare_temporal_fields(vm, *merged_fields, merged_field_names, Vector<StringView> {}));

    // 12. Let options be OrdinaryObjectCreate(null).
    auto options = Object::create(realm, nullptr);

    // 13. Perform ! CreateDataPropertyOrThrow(options, "overflow", "reject").
    MUST(options->create_data_property_or_throw(vm.names.overflow, PrimitiveString::create(vm, vm.names.reject.as_string())));

    // 14. Return ? CalendarDateFromFields(calendar, mergedFields, options).
    return TRY(calendar_date_from_fields(vm, calendar, *merged_fields, options));
}

// 10.3.13 Temporal.PlainMonthDay.prototype.getISOFields ( ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype.getisofields
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::get_iso_fields)
{
    auto& realm = *vm.current_realm();

    // 1. Let monthDay be the this value.
    // 2. Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // 3. Let fields be OrdinaryObjectCreate(%Object.prototype%).
    auto fields = Object::create(realm, realm.intrinsics().object_prototype());

    // 4. Perform ! CreateDataPropertyOrThrow(fields, "calendar", monthDay.[[Calendar]]).
    MUST(fields->create_data_property_or_throw(vm.names.calendar, Value(&month_day->calendar())));

    // 5. Perform ! CreateDataPropertyOrThrow(fields, "isoDay", 𝔽(monthDay.[[ISODay]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoDay, Value(month_day->iso_day())));

    // 6. Perform ! CreateDataPropertyOrThrow(fields, "isoMonth", 𝔽(monthDay.[[ISOMonth]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoMonth, Value(month_day->iso_month())));

    // 7. Perform ! CreateDataPropertyOrThrow(fields, "isoYear", 𝔽(monthDay.[[ISOYear]])).
    MUST(fields->create_data_property_or_throw(vm.names.isoYear, Value(month_day->iso_year())));

    // 8. Return fields.
    return fields;
}

// 10.3.3 get Temporal.PlainMonthDay.prototype.calendarId
JS_DEFINE_NATIVE_FUNCTION(PlainMonthDayPrototype::calendar_id_getter)
{
    // Step 1: Let monthDay be the this value
    // Step 2: Perform ? RequireInternalSlot(monthDay, [[InitializedTemporalMonthDay]]).
    auto month_day = TRY(typed_this_object(vm));

    // Step 3: Return monthDay.[[Calendar]].
    auto& calendar = static_cast<Calendar&>(month_day->calendar());
    return PrimitiveString::create(vm, calendar.identifier());
}

}
