/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    OptionDefault, OptionType, get_option, modulo, ordinary_create_from_constructor_of,
};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::date::{parse_date_time_utc_offset_from_parse_result, system_time_zone_identifier};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intl::abstract_operations::{
    LocaleKey, LocaleOptions, ResolvedOptions, SpecialBehaviors, canonicalize_locale_list, filter_locales,
    get_available_named_time_zone_identifier, get_number_option, resolve_options, throw_option_is_not_valid_value,
};
use crate::runtime::intl::date_time_format::{
    DateTimeFormat, adjust_date_time_style_format, for_each_calendar_field, get_date_time_format,
};
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::temporal::iso8601::{SubMinutePrecision, parse_utc_offset};
use crate::unicode::date_time_format::{
    self as unicode, CalendarPattern, CalendarPatternField, CalendarPatternFieldValue, HourCycle,
};
use crate::utf16::{Utf16View, concatenate};

#[repr(C)]
#[derive(Trace)]
pub struct DateTimeFormatConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    DateTimeFormatConstructor,
    initialize: DateTimeFormatConstructor::initialize,
    call: DateTimeFormatConstructor::call,
    construct: DateTimeFormatConstructor::construct
);

impl DateTimeFormatConstructor {
    // 11.1 The Intl.DateTimeFormat Constructor, https://tc39.es/ecma402/#sec-intl-datetimeformat-constructor
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DateTimeFormatConstructor> {
        realm.create_object(
            vm,
            DateTimeFormatConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.DateTimeFormat.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 11.2.1 Intl.DateTimeFormat.prototype, https://tc39.es/ecma402/#sec-intl.datetimeformat.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().intl_date_time_format_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_native_function(
            vm,
            realm,
            &vm.names.supportedLocalesOf,
            raw_native!(DateTimeFormatConstructor::supported_locales_of),
            1,
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            None,
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 11.1.1 Intl.DateTimeFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.datetimeformat
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, let newTarget be the active function object, else let newTarget be NewTarget.
        Ok(Value::from_object(Self::construct(
            function,
            vm,
            function.as_function_object_gc(),
        )?))
    }

    // 11.1.1 Intl.DateTimeFormat ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sec-intl.datetimeformat
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 2. Let dateTimeFormat be ? CreateDateTimeFormat(newTarget, locales, options, ANY, DATE).
        let date_time_format = create_date_time_format(
            vm,
            new_target,
            locales,
            options,
            OptionRequired::Any,
            OptionDefaults::Date,
            None,
        )?;

        // 3. If the implementation supports the normative optional constructor mode of 4.3 Note 1, then
        //     a. Let this be the this value.
        //     b. Return ? ChainDateTimeFormat(dateTimeFormat, NewTarget, this).

        // 4. Return dateTimeFormat.
        Ok(date_time_format.upcast())
    }

    // 11.2.2 Intl.DateTimeFormat.supportedLocalesOf ( locales [ , options ] ), https://tc39.es/ecma402/#sec-intl.datetimeformat.supportedlocalesof
    fn supported_locales_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let availableLocales be %DateTimeFormat%.[[AvailableLocales]].

        // 2. Let requestedLocales be ? CanonicalizeLocaleList(locales).
        let requested_locales = canonicalize_locale_list(vm, locales)?;

        // 3. Return ? FilterLocales(availableLocales, requestedLocales, options).
        Ok(Value::from_object(filter_locales(vm, &requested_locales, options)?))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OptionRequired {
    Any,
    Date,
    Time,
    YearMonth,
    MonthDay,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OptionDefaults {
    All,
    Date,
    Time,
    YearMonth,
    MonthDay,
    ZonedDateTime,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OptionInherit {
    All,
    Relevant,
}

fn throw_invalid_date_time_format_option<T>(vm: &Vm, option: &str, other_option: &str) -> ThrowCompletionOr<T> {
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::IntlInvalidDateTimeFormatOption,
        &[&option, &other_option],
    )
}

// 11.1.2 CreateDateTimeFormat ( newTarget, locales, options, required, defaults ), https://tc39.es/ecma402/#sec-createdatetimeformat
// 15.4.1 CreateDateTimeFormat ( newTarget, locales, options, required, defaults [ , toLocaleStringTimeZone ] ), https://tc39.es/proposal-temporal/#sec-createdatetimeformat
// 3.1.1 CreateDateTimeFormat ( newTarget, locales, options, required, defaults ), https://tc39.es/proposal-intl-era-monthcode/#sec-ecma402-intl-datetimeformat-constructor
pub fn create_date_time_format(
    vm: &Vm,
    new_target: Gc<FunctionObject>,
    locales_value: Value,
    options_value: Value,
    required: OptionRequired,
    defaults: OptionDefaults,
    to_locale_string_time_zone: Option<Utf16View<'_>>,
) -> ThrowCompletionOr<Gc<DateTimeFormat>> {
    let realm = vm.current_realm().expect("CreateDateTimeFormat runs in a realm");
    let names = &vm.names;

    // 1. Let dateTimeFormat be ? OrdinaryCreateFromConstructor(newTarget, "%Intl.DateTimeFormat.prototype%", « [[InitializedDateTimeFormat]], [[Locale]], [[Calendar]], [[NumberingSystem]], [[TimeZone]], [[HourCycle]], [[DateStyle]], [[TimeStyle]], [[DateTimeFormat]], [[BoundFormat]] »).
    let date_time_format = ordinary_create_from_constructor_of(
        vm,
        realm,
        new_target,
        Intrinsics::intl_date_time_format_prototype,
        |prototype| DateTimeFormat::new(vm, prototype),
    )?;

    // 2. Let hour12 be undefined.
    let hour12 = Cell::new(Value::UNDEFINED);

    // 3. Let modifyResolutionOptions be a new Abstract Closure with parameters (options) that captures hour12 and performs the following steps when called:
    let modify_resolution_options = |options: &mut LocaleOptions| {
        // a. Set hour12 to options.[[hour12]].
        hour12.set(options.hour12);

        // b. Remove field [[hour12]] from options.
        options.hour12 = Value::UNDEFINED;

        // c. If hour12 is not undefined, set options.[[hc]] to null.
        if !hour12.get().is_undefined() {
            options.hc = Some(LocaleKey::Empty);
        }
    };

    // 4. Let optionsResolution be ? ResolveOptions(%Intl.DateTimeFormat%, %Intl.DateTimeFormat%.[[LocaleData]], locales, options, « COERCE-OPTIONS », modifyResolutionOptions).
    // 5. Set options to optionsResolution.[[Options]].
    // 6. Let r be optionsResolution.[[ResolvedLocale]].
    let ResolvedOptions {
        options,
        resolved_locale: result,
        ..
    } = resolve_options(
        vm,
        &*date_time_format,
        locales_value,
        options_value,
        SpecialBehaviors::COERCE_OPTIONS,
        Some(&modify_resolution_options),
    )?;
    let hour12 = hour12.get();

    // 7. Set dateTimeFormat.[[Locale]] to r.[[Locale]].
    date_time_format.set_locale(result.locale);

    // 8. Let resolvedCalendar be r.[[ca]].
    if let LocaleKey::String(resolved_calendar) = result.ca {
        // 9. If resolvedCalendar is "islamic", then
        // NB: We also make "islamic-rgsa" fall back to "islamic-tbla", as test262 relies on this behavior. This falls
        //     within implementation-defined behavior. See:
        //     https://github.com/tc39/ecma402/pull/1044#discussion_r2926804980
        let resolved_calendar_view = Utf16View::of_string(&resolved_calendar);
        if resolved_calendar_view == "islamic" || resolved_calendar_view == "islamic-rgsa" {
            // a. Set resolvedCalendar to "islamic-tbla".
            date_time_format.set_calendar(Utf16String::from_utf8("islamic-tbla"));

            // b. If the ECMAScript implementation has a mechanism for reporting diagnostic warning messages, a warning
            //    should be issued.
        } else {
            // 10. Set dateTimeFormat.[[Calendar]] to resolvedCalendar.
            date_time_format.set_calendar(resolved_calendar);
        }
    }

    date_time_format.set_icu_locale(result.icu_locale);

    // 11. Set dateTimeFormat.[[NumberingSystem]] to r.[[nu]].
    if let LocaleKey::String(resolved_numbering_system) = result.nu {
        date_time_format.set_numbering_system(resolved_numbering_system);
    }

    // 12. Let resolvedLocaleData be r.[[LocaleData]].

    let mut hour_cycle_value: Option<HourCycle> = None;
    let mut hour12_value: Option<bool> = None;

    // 13. If hour12 is true, then
    //     a. Let hc be resolvedLocaleData.[[hourCycle12]].
    // 14. Else if hour12 is false, then
    //     a. Let hc be resolvedLocaleData.[[hourCycle24]].
    if hour12.is_boolean() {
        // NOTE: We let LibUnicode figure out the appropriate hour cycle.
        hour12_value = Some(hour12.as_bool());
    }
    // 15. Else,
    else {
        // a. Assert: hour12 is undefined.
        assert!(hour12.is_undefined());

        // b. Let hc be r.[[hc]].
        if let LocaleKey::String(resolved_hour_cycle) = &result.hc {
            hour_cycle_value = Some(unicode::hour_cycle_from_string(Utf16View::of_string(
                resolved_hour_cycle,
            )));
        }

        // c. If hc is null, set hc to resolvedLocaleData.[[hourCycle]].
        if hour_cycle_value.is_none() {
            hour_cycle_value = unicode::default_hour_cycle(Utf16View::of_string(&date_time_format.icu_locale()));
        }
    }

    // 16. Set dateTimeFormat.[[HourCycle]] to hc.
    // NOTE: The [[HourCycle]] is stored and accessed from [[DateTimeFormat]].

    // 17. Let timeZone be ? Get(options, "timeZone").
    let time_zone_value = options.get(vm, &names.timeZone)?;
    let mut icu_time_zone;
    let mut time_zone;

    // 18. If timeZone is undefined, then
    if time_zone_value.is_undefined() {
        // a. If toLocaleStringTimeZone is present, then
        if let Some(to_locale_string_time_zone) = to_locale_string_time_zone {
            // i. Set timeZone to toLocaleStringTimeZone.
            time_zone = to_locale_string_time_zone.to_utf16_string();
        }
        // b. Else,
        else {
            // i. Set timeZone to SystemTimeZoneIdentifier().
            time_zone = system_time_zone_identifier();
        }
    }
    // 19. Else,
    else {
        // a. If toLocaleStringTimeZone is present, throw a TypeError exception.
        if to_locale_string_time_zone.is_some() {
            return throw_invalid_date_time_format_option(vm, "timeZone", "a toLocaleString time zone");
        }

        // b. Set timeZone to ? ToString(timeZone).
        time_zone = time_zone_value.to_utf16_string(vm)?;
    }

    // 20. If IsTimeZoneOffsetString(timeZone) is true, then
    let offset_minutes =
        parse_utc_offset(Utf16View::of_string(&time_zone), SubMinutePrecision::No).map(|parse_result| {
            // a. Let parseResult be ParseText(StringToCodePoints(timeZone), UTCOffset[~SubMinutePrecision]).

            // b. Assert: parseResult is a Parse Node.

            // c. Let offsetNanoseconds be ? ParseDateTimeUTCOffset(timeZone).
            let offset_nanoseconds = parse_date_time_utc_offset_from_parse_result(&parse_result);

            // d. Let offsetMinutes be offsetNanoseconds / (6 × 10**10).
            offset_nanoseconds / 60_000_000_000.0
        });
    let is_time_zone_offset_string = offset_minutes.is_some();

    if let Some(offset_minutes) = offset_minutes {
        // e. Set timeZone to FormatOffsetTimeZoneIdentifier(offsetMinutes).
        time_zone = format_offset_time_zone_identifier(offset_minutes);
        icu_time_zone = time_zone.clone();
    }
    // 21. Else,
    else {
        // a. Let timeZoneIdentifierRecord be GetAvailableNamedTimeZoneIdentifier(timeZone).
        let time_zone_view = Utf16View::of_string(&time_zone);
        let Some(time_zone_identifier_record) = get_available_named_time_zone_identifier(time_zone_view) else {
            // b. If timeZoneIdentifierRecord is EMPTY, throw a RangeError exception.
            return throw_option_is_not_valid_value(vm, time_zone_view, &names.timeZone);
        };

        // c. Set timeZone to timeZoneIdentifierRecord.[[Identifier]].
        time_zone = time_zone_identifier_record.identifier.clone();
        icu_time_zone = time_zone.clone();
    }

    // 22. Set dateTimeFormat.[[TimeZone]] to timeZone.
    date_time_format.set_time_zone(time_zone);

    // NOTE: ICU requires time zone offset strings to be of the form "GMT+00:00"
    if is_time_zone_offset_string {
        icu_time_zone = concatenate(&[Utf16View::Ascii(b"GMT"), Utf16View::of_string(&icu_time_zone)]);
    }

    // AD-HOC: We must store the massaged time zone for creating ICU formatters for Temporal objects.
    date_time_format.set_temporal_time_zone(icu_time_zone.clone());

    // 23. Let formatOptions be a new Record.
    // 24. Set formatOptions.[[hourCycle]] to hc.
    let mut format_options = CalendarPattern {
        hour_cycle: hour_cycle_value,
        hour12: hour12_value,
        ..CalendarPattern::default()
    };

    // 25. Let hasExplicitFormatComponents be false.
    // NOTE: Instead of using a boolean, we track any explicitly provided component name for nicer exception messages.
    let mut explicit_format_component: Option<&PropertyKey> = None;

    // 26. For each row of Table 16, except the header row, in table order, do
    for_each_calendar_field(vm, |row| {
        // a. Let prop be the name given in the Property column of the current row.

        // b. If prop is "fractionalSecondDigits", then
        if row.field == CalendarPatternField::FractionalSecondDigits {
            // i. Let value be ? GetNumberOption(options, "fractionalSecondDigits", 1, 3, undefined).
            let value = get_number_option(vm, &options, row.property, 1, 3, None)?;

            // d. Set formatOptions.[[<prop>]] to value.
            if let Some(value) = value {
                format_options.set_field(
                    row.field,
                    Some(CalendarPatternFieldValue::FractionalSecondDigits(value as u8)),
                );

                // e. If value is not undefined, then
                //     i. Set hasExplicitFormatComponents to true.
                explicit_format_component = Some(row.property);
            }
        }
        // c. Else,
        else {
            // i. Let values be a List whose elements are the strings given in the Values column of the current row.
            // ii. Let value be ? GetOption(options, prop, string, values, undefined).
            let value = get_option(
                vm,
                &options,
                row.property,
                OptionType::String,
                row.values,
                OptionDefault::Empty,
            )?;

            // d. Set formatOptions.[[<prop>]] to value.
            if !value.is_undefined() {
                format_options.set_field(
                    row.field,
                    Some(CalendarPatternFieldValue::Style(
                        unicode::calendar_pattern_style_from_string(value.as_string().utf16_string_view()),
                    )),
                );

                // e. If value is not undefined, then
                //     i. Set hasExplicitFormatComponents to true.
                explicit_format_component = Some(row.property);
            }
        }

        Ok(())
    })?;

    // 27. Let formatMatcher be ? GetOption(options, "formatMatcher", string, « "basic", "best fit" », "best fit").
    get_option(
        vm,
        &options,
        &names.formatMatcher,
        OptionType::String,
        &["basic", "best fit"],
        OptionDefault::string("best fit"),
    )?;

    // 28. Let dateStyle be ? GetOption(options, "dateStyle", string, « "full", "long", "medium", "short" », undefined).
    let date_style = get_option(
        vm,
        &options,
        &names.dateStyle,
        OptionType::String,
        &["full", "long", "medium", "short"],
        OptionDefault::Empty,
    )?;

    // 29. Set dateTimeFormat.[[DateStyle]] to dateStyle.
    if !date_style.is_undefined() {
        date_time_format.set_date_style(date_style.as_string().utf16_string_view());
    }

    // 30. Let timeStyle be ? GetOption(options, "timeStyle", string, « "full", "long", "medium", "short" », undefined).
    let time_style = get_option(
        vm,
        &options,
        &names.timeStyle,
        OptionType::String,
        &["full", "long", "medium", "short"],
        OptionDefault::Empty,
    )?;

    // 31. Set dateTimeFormat.[[TimeStyle]] to timeStyle.
    if !time_style.is_undefined() {
        date_time_format.set_time_style(time_style.as_string().utf16_string_view());
    }

    // 32. Let formats be resolvedLocaleData.[[formats]].[[<resolvedCalendar>]].

    let icu_locale = date_time_format.icu_locale();
    let icu_locale_view = Utf16View::of_string(&icu_locale);
    let icu_time_zone_view = Utf16View::of_string(&icu_time_zone);

    // 33. If dateStyle is not undefined or timeStyle is not undefined, then
    let formatter = if date_time_format.has_date_style() || date_time_format.has_time_style() {
        // a. If hasExplicitFormatComponents is true, then
        if let Some(explicit_format_component) = explicit_format_component {
            // i. Throw a TypeError exception.
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IntlInvalidDateTimeFormatOption,
                &[explicit_format_component, &"dateStyle or timeStyle"],
            );
        }

        // b. If required is date and timeStyle is not undefined, then
        if required == OptionRequired::Date && !time_style.is_undefined() {
            // i. Throw a TypeError exception.
            return throw_invalid_date_time_format_option(vm, "timeStyle", "date");
        }

        // c. If required is time and dateStyle is not undefined, then
        if required == OptionRequired::Time && !date_style.is_undefined() {
            // i. Throw a TypeError exception.
            return throw_invalid_date_time_format_option(vm, "dateStyle", "time");
        }

        // d. Let styles be resolvedLocaleData.[[styles]].[[<resolvedCalendar>]].
        // e. Let bestFormat be DateTimeStyleFormat(dateStyle, timeStyle, styles).
        let formatter = unicode::DateTimeFormat::create_for_date_and_time_style(
            icu_locale_view,
            icu_time_zone_view,
            format_options.hour_cycle,
            format_options.hour12,
            date_time_format.date_style(),
            date_time_format.time_style(),
        );

        let best_format = formatter.chosen_pattern();
        use CalendarPatternField::{
            Day, DayPeriod, Era, FractionalSecondDigits, Hour, Minute, Month, Second, Weekday, Year,
        };

        // f. If dateStyle is not undefined, then
        if !date_style.is_undefined() {
            // i. Set dateTimeFormat.[[TemporalPlainDateFormat]] to AdjustDateTimeStyleFormat(formats, bestFormat, formatMatcher, « "weekday", "era", "year", "month", "day" »).
            let temporal_plain_date_format =
                adjust_date_time_style_format(vm, &best_format, &[Weekday, Era, Year, Month, Day]);
            date_time_format.set_temporal_plain_date_format(Some(temporal_plain_date_format));

            // ii. Set dateTimeFormat.[[TemporalPlainYearMonthFormat]] to AdjustDateTimeStyleFormat(formats, bestFormat, formatMatcher, « "era", "year", "month" »).
            let temporal_plain_year_month_format = adjust_date_time_style_format(vm, &best_format, &[Era, Year, Month]);
            date_time_format.set_temporal_plain_year_month_format(Some(temporal_plain_year_month_format));

            // iii. Set dateTimeFormat.[[TemporalPlainMonthDayFormat]] to AdjustDateTimeStyleFormat(formats, bestFormat, formatMatcher, « "month", "day" »).
            let temporal_plain_month_day_format = adjust_date_time_style_format(vm, &best_format, &[Month, Day]);
            date_time_format.set_temporal_plain_month_day_format(Some(temporal_plain_month_day_format));
        }
        // g. Else,
        else {
            // i. Set dateTimeFormat.[[TemporalPlainDateFormat]] to null.
            // ii. Set dateTimeFormat.[[TemporalPlainYearMonthFormat]] to null.
            // iii. Set dateTimeFormat.[[TemporalPlainMonthDayFormat]] to null.
        }

        // h. If timeStyle is not undefined, then
        if !time_style.is_undefined() {
            // i. Set dateTimeFormat.[[TemporalPlainTimeFormat]] to AdjustDateTimeStyleFormat(formats, bestFormat, formatMatcher, « "dayPeriod", "hour", "minute", "second", "fractionalSecondDigits" »).
            let temporal_plain_time_format = adjust_date_time_style_format(
                vm,
                &best_format,
                &[DayPeriod, Hour, Minute, Second, FractionalSecondDigits],
            );
            date_time_format.set_temporal_plain_time_format(Some(temporal_plain_time_format));
        }
        // i. Else,
        else {
            // i. Set dateTimeFormat.[[TemporalPlainTimeFormat]] to null.
        }

        // j. Set dateTimeFormat.[[TemporalPlainDateTimeFormat]] to AdjustDateTimeStyleFormat(formats, bestFormat, formatMatcher, « "weekday", "era", "year", "month", "day", "dayPeriod", "hour", "minute", "second", "fractionalSecondDigits" »).
        let temporal_plain_date_time_format = adjust_date_time_style_format(
            vm,
            &best_format,
            &[
                Weekday,
                Era,
                Year,
                Month,
                Day,
                DayPeriod,
                Hour,
                Minute,
                Second,
                FractionalSecondDigits,
            ],
        );
        date_time_format.set_temporal_plain_date_time_format(Some(temporal_plain_date_time_format));

        // k. Set dateTimeFormat.[[TemporalInstantFormat]] to bestFormat.
        date_time_format.set_temporal_instant_format(Some(best_format));

        formatter
    }
    // 34. Else,
    else {
        // a. Let bestFormat be GetDateTimeFormat(formats, formatMatcher, formatOptions, required, defaults, ALL).
        let best_format = get_date_time_format(&format_options, required, defaults, OptionInherit::All)
            .expect("GetDateTimeFormat with inherit ALL returns a format");

        // b. Set dateTimeFormat.[[TemporalPlainDateFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, DATE, DATE, RELEVANT).
        let temporal_plain_date_format = get_date_time_format(
            &format_options,
            OptionRequired::Date,
            OptionDefaults::Date,
            OptionInherit::Relevant,
        );
        date_time_format.set_temporal_plain_date_format(temporal_plain_date_format);

        // c. Set dateTimeFormat.[[TemporalPlainYearMonthFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, YEAR-MONTH, YEAR-MONTH, RELEVANT).
        let temporal_plain_year_month_format = get_date_time_format(
            &format_options,
            OptionRequired::YearMonth,
            OptionDefaults::YearMonth,
            OptionInherit::Relevant,
        );
        date_time_format.set_temporal_plain_year_month_format(temporal_plain_year_month_format);

        // d. Set dateTimeFormat.[[TemporalPlainMonthDayFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, MONTH-DAY, MONTH-DAY, RELEVANT).
        let temporal_plain_month_day_format = get_date_time_format(
            &format_options,
            OptionRequired::MonthDay,
            OptionDefaults::MonthDay,
            OptionInherit::Relevant,
        );
        date_time_format.set_temporal_plain_month_day_format(temporal_plain_month_day_format);

        // e. Set dateTimeFormat.[[TemporalPlainTimeFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, TIME, TIME, RELEVANT).
        let temporal_plain_time_format = get_date_time_format(
            &format_options,
            OptionRequired::Time,
            OptionDefaults::Time,
            OptionInherit::Relevant,
        );
        date_time_format.set_temporal_plain_time_format(temporal_plain_time_format);

        // f. Set dateTimeFormat.[[TemporalPlainDateTimeFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, ANY, ALL, RELEVANT).
        let temporal_plain_date_time_format = get_date_time_format(
            &format_options,
            OptionRequired::Any,
            OptionDefaults::All,
            OptionInherit::Relevant,
        );
        date_time_format.set_temporal_plain_date_time_format(temporal_plain_date_time_format);

        // g. If toLocaleStringTimeZone is present, then
        if to_locale_string_time_zone.is_some() {
            // i. Set dateTimeFormat.[[TemporalInstantFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, ANY, ZONED-DATE-TIME, ALL).
            let temporal_instant_format = get_date_time_format(
                &format_options,
                OptionRequired::Any,
                OptionDefaults::ZonedDateTime,
                OptionInherit::All,
            );
            date_time_format.set_temporal_instant_format(temporal_instant_format);
        }
        // h. Else,
        else {
            // i. Set dateTimeFormat.[[TemporalInstantFormat]] to GetDateTimeFormat(formats, formatMatcher, formatOptions, ANY, ALL, ALL).
            let temporal_instant_format = get_date_time_format(
                &format_options,
                OptionRequired::Any,
                OptionDefaults::All,
                OptionInherit::All,
            );
            date_time_format.set_temporal_instant_format(temporal_instant_format);
        }

        unicode::DateTimeFormat::create_for_pattern_options(icu_locale_view, icu_time_zone_view, &best_format)
    };

    // 35. Set dateTimeFormat.[[DateTimeFormat]] to bestFormat.
    date_time_format.set_date_time_format(formatter.chosen_pattern());

    // Non-standard, create an ICU number formatter for this Intl object.
    date_time_format.set_formatter(formatter);

    // 36. Return dateTimeFormat.
    Ok(date_time_format)
}

// 11.1.3 FormatOffsetTimeZoneIdentifier ( offsetMinutes ), https://tc39.es/ecma402/#sec-formatoffsettimezoneidentifier
pub fn format_offset_time_zone_identifier(offset_minutes: f64) -> Utf16String {
    // 1. If offsetMinutes ≥ 0, let sign be the code unit 0x002B (PLUS SIGN); otherwise, let sign be the code unit 0x002D (HYPHEN-MINUS).
    let sign = if offset_minutes >= 0.0 { '+' } else { '-' };

    // 2. Let absoluteMinutes be abs(offsetMinutes).
    let absolute_minutes = offset_minutes.abs();

    // 3. Let hours be floor(absoluteMinutes / 60).
    let hours = (absolute_minutes / 60.0).floor() as i64;

    // 4. Let minutes be absoluteMinutes modulo 60.
    let minutes = modulo(absolute_minutes, 60.0) as i64;

    // 5. Return the string-concatenation of sign, ToZeroPaddedDecimalString(hours, 2), the code unit 0x003A (COLON), and ToZeroPaddedDecimalString(minutes, 2).
    Utf16String::from_utf8(&format!("{sign}{hours:02}:{minutes:02}"))
}
