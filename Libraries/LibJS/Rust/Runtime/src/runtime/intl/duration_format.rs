/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;
use num_traits::FromPrimitive;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{OptionDefault, OptionType, construct, get_option};
use crate::runtime::big_fraction::BigFraction;
use crate::runtime::big_int::SignedBigInteger;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::intl_object::{IntlObject, ResolutionOptionDescriptor};
use crate::runtime::intl::list_format::{ListFormat, create_parts_from_list};
use crate::runtime::intl::mathematical_value::{MathematicalValue, MathematicalValueSymbol};
use crate::runtime::intl::number_format::{NumberFormat, partition_number_pattern};
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::temporal::duration::{Duration, duration_sign};
use crate::unicode::intl::{self as unicode, Style as UnicodeStyle};
use crate::utf16::{Utf16StringBuilder, Utf16View};

/// DurationFormat::Style.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Style {
    Long,
    Short,
    Narrow,
    Digital,
}

/// DurationFormat::ValueStyle, whose first three values are those of Unicode::Style.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ValueStyle {
    Long,
    Short,
    Narrow,
    Numeric,
    TwoDigit,
    Fractional,
}

/// DurationFormat::Display.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Display {
    Auto,
    Always,
}

/// DurationFormat::Unit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Unit {
    Years,
    Months,
    Weeks,
    Days,
    Hours,
    Minutes,
    Seconds,
    Milliseconds,
    Microseconds,
    Nanoseconds,
}

// 13.5.6.1 Duration Unit Options Records, https://tc39.es/ecma402/#sec-durationformat-unit-options-record
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DurationUnitOptions {
    pub style: ValueStyle,
    pub display: Display,
}

impl Default for DurationUnitOptions {
    fn default() -> Self {
        Self {
            style: ValueStyle::Long,
            display: Display::Auto,
        }
    }
}

/// 13 DurationFormat Objects, https://tc39.es/ecma402/#durationformat-objects
#[repr(C)]
#[derive(Trace)]
pub struct DurationFormat {
    base: Object,
    #[gc(untraced)]
    locale: GcRefCell<Utf16String>, // [[Locale]]
    #[gc(untraced)]
    numbering_system: GcRefCell<Utf16String>, // [[NumberingSystem]]
    #[gc(untraced)]
    hour_minute_separator: GcRefCell<Utf16String>, // [[HourMinutesSeparator]]
    #[gc(untraced)]
    minute_second_separator: GcRefCell<Utf16String>, // [[MinutesSecondsSeparator]]

    #[gc(untraced)]
    style: Cell<Style>, // [[Style]]
    #[gc(untraced)]
    years_options: Cell<DurationUnitOptions>, // [[YearsOptions]]
    #[gc(untraced)]
    months_options: Cell<DurationUnitOptions>, // [[MonthsOptions]]
    #[gc(untraced)]
    weeks_options: Cell<DurationUnitOptions>, // [[WeeksOptions]]
    #[gc(untraced)]
    days_options: Cell<DurationUnitOptions>, // [[DaysOptions]]
    #[gc(untraced)]
    hours_options: Cell<DurationUnitOptions>, // [[HoursOptions]]
    #[gc(untraced)]
    minutes_options: Cell<DurationUnitOptions>, // [[MinutesOptions]]
    #[gc(untraced)]
    seconds_options: Cell<DurationUnitOptions>, // [[SecondsOptions]]
    #[gc(untraced)]
    milliseconds_options: Cell<DurationUnitOptions>, // [[MillisecondsOptions]]
    #[gc(untraced)]
    microseconds_options: Cell<DurationUnitOptions>, // [[MicrosecondsOptions]]
    #[gc(untraced)]
    nanoseconds_options: Cell<DurationUnitOptions>, // [[NanosecondsOptions]]
    #[gc(untraced)]
    fractional_digits: Cell<Option<u8>>, // [[FractionalDigits]]
}

define_cell!(DurationFormat, Object, extends: [Object]);

impl Deref for DurationFormat {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

/// Defines the getter and setter of each Duration Unit Options Record slot.
macro_rules! duration_unit_options_accessors {
    ($($getter:ident, $setter:ident;)*) => {
        $(
            pub fn $setter(&self, options: DurationUnitOptions) {
                self.$getter.set(options);
            }

            pub fn $getter(&self) -> DurationUnitOptions {
                self.$getter.get()
            }
        )*
    };
}

impl DurationFormat {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> DurationFormat {
        DurationFormat {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            locale: GcRefCell::default(),
            numbering_system: GcRefCell::default(),
            hour_minute_separator: GcRefCell::default(),
            minute_second_separator: GcRefCell::default(),
            style: Cell::new(Style::Long),
            years_options: Cell::new(DurationUnitOptions::default()),
            months_options: Cell::new(DurationUnitOptions::default()),
            weeks_options: Cell::new(DurationUnitOptions::default()),
            days_options: Cell::new(DurationUnitOptions::default()),
            hours_options: Cell::new(DurationUnitOptions::default()),
            minutes_options: Cell::new(DurationUnitOptions::default()),
            seconds_options: Cell::new(DurationUnitOptions::default()),
            milliseconds_options: Cell::new(DurationUnitOptions::default()),
            microseconds_options: Cell::new(DurationUnitOptions::default()),
            nanoseconds_options: Cell::new(DurationUnitOptions::default()),
            fractional_digits: Cell::new(None),
        }
    }

    pub fn style_from_string(style: Utf16View<'_>) -> Style {
        if style == "long" {
            return Style::Long;
        }
        if style == "short" {
            return Style::Short;
        }
        if style == "narrow" {
            return Style::Narrow;
        }
        if style == "digital" {
            return Style::Digital;
        }
        unreachable!("the style is one of the values GetOption allows")
    }

    pub fn style_to_string(style: Style) -> &'static str {
        match style {
            Style::Long => "long",
            Style::Short => "short",
            Style::Narrow => "narrow",
            Style::Digital => "digital",
        }
    }

    pub fn value_style_from_string(value_style: Utf16View<'_>) -> ValueStyle {
        if value_style == "long" {
            return ValueStyle::Long;
        }
        if value_style == "short" {
            return ValueStyle::Short;
        }
        if value_style == "narrow" {
            return ValueStyle::Narrow;
        }
        if value_style == "numeric" {
            return ValueStyle::Numeric;
        }
        if value_style == "2-digit" {
            return ValueStyle::TwoDigit;
        }
        if value_style == "fractional" {
            return ValueStyle::Fractional;
        }
        unreachable!("the style is one of the values GetOption allows")
    }

    pub fn value_style_to_string(value_style: ValueStyle) -> &'static str {
        match value_style {
            ValueStyle::Long => "long",
            ValueStyle::Short => "short",
            ValueStyle::Narrow => "narrow",
            ValueStyle::Numeric => "numeric",
            ValueStyle::TwoDigit => "2-digit",
            ValueStyle::Fractional => "fractional",
        }
    }

    pub fn display_from_string(display: Utf16View<'_>) -> Display {
        if display == "auto" {
            return Display::Auto;
        }
        if display == "always" {
            return Display::Always;
        }
        unreachable!("the display is one of the values GetOption allows")
    }

    pub fn display_to_string(display: Display) -> &'static str {
        match display {
            Display::Auto => "auto",
            Display::Always => "always",
        }
    }

    pub fn set_locale(&self, locale: Utf16String) {
        self.locale.replace(locale);
    }

    pub fn locale(&self) -> Utf16String {
        self.locale.borrow().clone()
    }

    pub fn set_numbering_system(&self, numbering_system: Utf16String) {
        self.numbering_system.replace(numbering_system);
    }

    pub fn numbering_system(&self) -> Utf16String {
        self.numbering_system.borrow().clone()
    }

    pub fn set_hour_minute_separator(&self, hour_minute_separator: Utf16String) {
        self.hour_minute_separator.replace(hour_minute_separator);
    }

    pub fn hour_minute_separator(&self) -> Utf16String {
        self.hour_minute_separator.borrow().clone()
    }

    pub fn set_minute_second_separator(&self, minute_second_separator: Utf16String) {
        self.minute_second_separator.replace(minute_second_separator);
    }

    pub fn minute_second_separator(&self) -> Utf16String {
        self.minute_second_separator.borrow().clone()
    }

    pub fn set_style(&self, style: Utf16View<'_>) {
        self.style.set(Self::style_from_string(style));
    }

    pub fn style(&self) -> Style {
        self.style.get()
    }

    pub fn style_string(&self) -> &'static str {
        Self::style_to_string(self.style.get())
    }

    duration_unit_options_accessors! {
        years_options, set_years_options;
        months_options, set_months_options;
        weeks_options, set_weeks_options;
        days_options, set_days_options;
        hours_options, set_hours_options;
        minutes_options, set_minutes_options;
        seconds_options, set_seconds_options;
        milliseconds_options, set_milliseconds_options;
        microseconds_options, set_microseconds_options;
        nanoseconds_options, set_nanoseconds_options;
    }

    pub fn set_fractional_digits(&self, fractional_digits: Option<u8>) {
        self.fractional_digits.set(fractional_digits);
    }

    pub fn has_fractional_digits(&self) -> bool {
        self.fractional_digits.get().is_some()
    }

    pub fn fractional_digits(&self) -> u8 {
        self.fractional_digits.get().expect("the fractional digits are set")
    }
}

impl IntlObject for DurationFormat {
    // 13.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.DurationFormat-internal-slots
    fn relevant_extension_keys(&self) -> &'static [&'static str] {
        // The value of the [[RelevantExtensionKeys]] internal slot is « "nu" ».
        &["nu"]
    }

    // 13.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.DurationFormat-internal-slots
    fn resolution_option_descriptors<'vm>(&self, vm: &'vm Vm) -> Vec<ResolutionOptionDescriptor<'vm>> {
        // The value of the [[ResolutionOptionDescriptors]] internal slot is « { [[Key]]: "nu", [[Property]]: "numberingSystem" } ».
        vec![ResolutionOptionDescriptor::string("nu", &vm.names.numberingSystem)]
    }
}

pub struct DurationInstanceComponent {
    pub value_slot: fn(&Duration) -> f64,
    pub get_internal_slot: fn(&DurationFormat) -> DurationUnitOptions,
    pub set_internal_slot: fn(&DurationFormat, DurationUnitOptions),
    pub unit: Unit,
    pub styles: &'static [&'static str],
    pub digital_default: ValueStyle,
}

// Table 20: Internal slots and property names of DurationFormat instances relevant to Intl.DurationFormat constructor, https://tc39.es/ecma402/#table-durationformat
// Table 24: DurationFormat instance internal slots and properties relevant to PartitionDurationFormatPattern, https://tc39.es/ecma402/#table-partition-duration-format-pattern
const DATE_STYLES: &[&str] = &["long", "short", "narrow"];
const TIME_STYLES: &[&str] = &["long", "short", "narrow", "numeric", "2-digit"];
const SUB_SECOND_STYLES: &[&str] = &["long", "short", "narrow", "numeric"];

pub static DURATION_INSTANCES_COMPONENTS: [DurationInstanceComponent; 10] = [
    DurationInstanceComponent {
        value_slot: Duration::years,
        get_internal_slot: DurationFormat::years_options,
        set_internal_slot: DurationFormat::set_years_options,
        unit: Unit::Years,
        styles: DATE_STYLES,
        digital_default: ValueStyle::Short,
    },
    DurationInstanceComponent {
        value_slot: Duration::months,
        get_internal_slot: DurationFormat::months_options,
        set_internal_slot: DurationFormat::set_months_options,
        unit: Unit::Months,
        styles: DATE_STYLES,
        digital_default: ValueStyle::Short,
    },
    DurationInstanceComponent {
        value_slot: Duration::weeks,
        get_internal_slot: DurationFormat::weeks_options,
        set_internal_slot: DurationFormat::set_weeks_options,
        unit: Unit::Weeks,
        styles: DATE_STYLES,
        digital_default: ValueStyle::Short,
    },
    DurationInstanceComponent {
        value_slot: Duration::days,
        get_internal_slot: DurationFormat::days_options,
        set_internal_slot: DurationFormat::set_days_options,
        unit: Unit::Days,
        styles: DATE_STYLES,
        digital_default: ValueStyle::Short,
    },
    DurationInstanceComponent {
        value_slot: Duration::hours,
        get_internal_slot: DurationFormat::hours_options,
        set_internal_slot: DurationFormat::set_hours_options,
        unit: Unit::Hours,
        styles: TIME_STYLES,
        digital_default: ValueStyle::Numeric,
    },
    DurationInstanceComponent {
        value_slot: Duration::minutes,
        get_internal_slot: DurationFormat::minutes_options,
        set_internal_slot: DurationFormat::set_minutes_options,
        unit: Unit::Minutes,
        styles: TIME_STYLES,
        digital_default: ValueStyle::Numeric,
    },
    DurationInstanceComponent {
        value_slot: Duration::seconds,
        get_internal_slot: DurationFormat::seconds_options,
        set_internal_slot: DurationFormat::set_seconds_options,
        unit: Unit::Seconds,
        styles: TIME_STYLES,
        digital_default: ValueStyle::Numeric,
    },
    DurationInstanceComponent {
        value_slot: Duration::milliseconds,
        get_internal_slot: DurationFormat::milliseconds_options,
        set_internal_slot: DurationFormat::set_milliseconds_options,
        unit: Unit::Milliseconds,
        styles: SUB_SECOND_STYLES,
        digital_default: ValueStyle::Numeric,
    },
    DurationInstanceComponent {
        value_slot: Duration::microseconds,
        get_internal_slot: DurationFormat::microseconds_options,
        set_internal_slot: DurationFormat::set_microseconds_options,
        unit: Unit::Microseconds,
        styles: SUB_SECOND_STYLES,
        digital_default: ValueStyle::Numeric,
    },
    DurationInstanceComponent {
        value_slot: Duration::nanoseconds,
        get_internal_slot: DurationFormat::nanoseconds_options,
        set_internal_slot: DurationFormat::set_nanoseconds_options,
        unit: Unit::Nanoseconds,
        styles: SUB_SECOND_STYLES,
        digital_default: ValueStyle::Numeric,
    },
];

pub struct DurationFormatPart {
    pub type_: Utf16String,
    pub value: Utf16String,
    pub unit: Utf16String,
}

fn unit_to_property_key(vm: &Vm, unit: Unit) -> &PropertyKey {
    let names = &vm.names;
    match unit {
        Unit::Years => &names.years,
        Unit::Months => &names.months,
        Unit::Weeks => &names.weeks,
        Unit::Days => &names.days,
        Unit::Hours => &names.hours,
        Unit::Minutes => &names.minutes,
        Unit::Seconds => &names.seconds,
        Unit::Milliseconds => &names.milliseconds,
        Unit::Microseconds => &names.microseconds,
        Unit::Nanoseconds => &names.nanoseconds,
    }
}

/// The string-concatenation of the unit's property key and "Display".
fn unit_to_display_property_key(vm: &Vm, unit: Unit) -> &PropertyKey {
    let names = &vm.names;
    match unit {
        Unit::Years => &names.yearsDisplay,
        Unit::Months => &names.monthsDisplay,
        Unit::Weeks => &names.weeksDisplay,
        Unit::Days => &names.daysDisplay,
        Unit::Hours => &names.hoursDisplay,
        Unit::Minutes => &names.minutesDisplay,
        Unit::Seconds => &names.secondsDisplay,
        Unit::Milliseconds => &names.millisecondsDisplay,
        Unit::Microseconds => &names.microsecondsDisplay,
        Unit::Nanoseconds => &names.nanosecondsDisplay,
    }
}

fn unit_to_number_format_property_key(vm: &Vm, unit: Unit) -> &PropertyKey {
    let names = &vm.names;
    match unit {
        Unit::Years => &names.year,
        Unit::Months => &names.month,
        Unit::Weeks => &names.week,
        Unit::Days => &names.day,
        Unit::Hours => &names.hour,
        Unit::Minutes => &names.minute,
        Unit::Seconds => &names.second,
        Unit::Milliseconds => &names.millisecond,
        Unit::Microseconds => &names.microsecond,
        Unit::Nanoseconds => &names.nanosecond,
    }
}

/// static_cast<Unicode::Style>(style) of a style that is "long", "short" or "narrow".
fn unicode_style_of(value_style: ValueStyle) -> UnicodeStyle {
    match value_style {
        ValueStyle::Long => UnicodeStyle::Long,
        ValueStyle::Short => UnicodeStyle::Short,
        ValueStyle::Narrow => UnicodeStyle::Narrow,
        ValueStyle::Numeric | ValueStyle::TwoDigit | ValueStyle::Fractional => {
            unreachable!("the style is one of the Unicode::Style values")
        }
    }
}

fn string_value(vm: &Vm, string: &str) -> Value {
    Value::from_string(PrimitiveString::create_from_utf8(vm, string))
}

fn construct_number_format(vm: &Vm, duration_format: &DurationFormat, options: Gc<Object>) -> Gc<NumberFormat> {
    let realm = vm.current_realm().expect("DurationFormat runs in a realm");

    let number_format = construct(
        vm,
        realm.intrinsics().intl_number_format_constructor(vm),
        &[
            Value::from_string(PrimitiveString::create(vm, duration_format.locale())),
            Value::from_object(options),
        ],
        None,
    )
    .must();
    number_format
        .downcast::<NumberFormat>()
        .expect("the Intl.NumberFormat constructor creates an Intl.NumberFormat")
}

fn construct_list_format(vm: &Vm, duration_format: &DurationFormat, options: Gc<Object>) -> Gc<ListFormat> {
    let realm = vm.current_realm().expect("DurationFormat runs in a realm");

    let list_format = construct(
        vm,
        realm.intrinsics().intl_list_format_constructor(vm),
        &[
            Value::from_string(PrimitiveString::create(vm, duration_format.locale())),
            Value::from_object(options),
        ],
        None,
    )
    .must();
    list_format
        .downcast::<ListFormat>()
        .expect("the Intl.ListFormat constructor creates an Intl.ListFormat")
}

// 13.5.6.1 ValidateDurationUnitStyle ( unit, style, display, prevStyle ), https://tc39.es/ecma402/#sec-validatedurationunitstyle
// AD-HOC: Our implementation takes extra parameters for better exception messages.
fn validate_duration_unit_style(
    vm: &Vm,
    unit: &PropertyKey,
    style: ValueStyle,
    display: Display,
    previous_style: Option<ValueStyle>,
    display_field: &PropertyKey,
) -> ThrowCompletionOr<()> {
    let unit_name = unit.to_utf16_string();
    let unit_name = Utf16View::of_string(&unit_name).to_utf8();

    // 1. If display is "always" and style is "fractional", throw a RangeError exception.
    if display == Display::Always && style == ValueStyle::Fractional {
        let display_field = display_field.to_utf16_string();
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::IntlFractionalUnitsMixedWithAlwaysDisplay,
            &[&unit_name, &Utf16View::of_string(&display_field).to_utf8()],
        );
    }

    // 2. If prevStyle is "fractional" and style is not "fractional", throw a RangeError exception.
    if previous_style == Some(ValueStyle::Fractional) && style != ValueStyle::Fractional {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::IntlFractionalUnitFollowedByNonFractionalUnit,
            &[&unit_name],
        );
    }

    // 3. If prevStyle is "numeric" or "2-digit" and style is not one of "fractional", "numeric" or "2-digit", throw a RangeError exception.
    if matches!(previous_style, Some(ValueStyle::Numeric | ValueStyle::TwoDigit))
        && !matches!(
            style,
            ValueStyle::Fractional | ValueStyle::Numeric | ValueStyle::TwoDigit
        )
    {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::IntlNonNumericOr2DigitAfterNumericOr2Digit,
            &[],
        );
    }

    // 4. Return unused.
    Ok(())
}

// 13.5.6 GetDurationUnitOptions ( unit, options, baseStyle, stylesList, digitalBase, prevStyle, twoDigitHours ), https://tc39.es/ecma402/#sec-getdurationunitoptions
#[allow(clippy::too_many_arguments, reason = "the abstract operation takes these arguments")]
pub fn get_duration_unit_options(
    vm: &Vm,
    unit: Unit,
    options: &Object,
    base_style: Style,
    styles_list: &[&str],
    digital_base: ValueStyle,
    previous_style: Option<ValueStyle>,
    two_digit_hours: bool,
) -> ThrowCompletionOr<DurationUnitOptions> {
    let unit_property_key = unit_to_property_key(vm, unit);

    // 1. Let style be ? GetOption(options, unit, STRING, stylesList, undefined).
    let style_value = get_option(
        vm,
        options,
        unit_property_key,
        OptionType::String,
        styles_list,
        OptionDefault::Empty,
    )?;
    let mut style;

    // 2. Let displayDefault be "always".
    let mut display_default = "always";

    // 3. If style is undefined, then
    if style_value.is_undefined() {
        // a. If baseStyle is "digital", then
        if base_style == Style::Digital {
            // i. Set style to digitalBase.
            style = digital_base;

            // ii. If unit is not one of "hours", "minutes", or "seconds", set displayDefault to "auto".
            if !matches!(unit, Unit::Hours | Unit::Minutes | Unit::Seconds) {
                display_default = "auto";
            }
        }
        // b. Else if prevStyle is one of "fractional", "numeric" or "2-digit", then
        else if matches!(
            previous_style,
            Some(ValueStyle::Fractional | ValueStyle::Numeric | ValueStyle::TwoDigit)
        ) {
            // i. Set style to "numeric".
            style = ValueStyle::Numeric;

            // ii. If unit is not "minutes" or "seconds", set displayDefault to "auto".
            if !matches!(unit, Unit::Minutes | Unit::Seconds) {
                display_default = "auto";
            }
        }
        // c. Else,
        else {
            // i. Set style to baseStyle.
            style = match base_style {
                Style::Long => ValueStyle::Long,
                Style::Short => ValueStyle::Short,
                Style::Narrow => ValueStyle::Narrow,
                Style::Digital => ValueStyle::Numeric,
            };

            // ii. Set displayDefault to "auto".
            display_default = "auto";
        }
    } else {
        style = DurationFormat::value_style_from_string(style_value.as_string().utf16_string_view());
    }

    // 4. If style is "numeric" and IsFractionalSecondUnitName(unit) is true, then
    if style == ValueStyle::Numeric && is_fractional_second_unit_name(unit) {
        // a. Set style to "fractional".
        style = ValueStyle::Fractional;

        // b. Set displayDefault to "auto".
        display_default = "auto";
    }

    // 5. Let displayField be the string-concatenation of unit and "Display".
    let display_field = unit_to_display_property_key(vm, unit);

    // 6. Let display be ? GetOption(options, displayField, STRING, « "auto", "always" », displayDefault).
    let display_value = get_option(
        vm,
        options,
        display_field,
        OptionType::String,
        &["auto", "always"],
        OptionDefault::string(display_default),
    )?;
    let display = DurationFormat::display_from_string(display_value.as_string().utf16_string_view());

    // 7. Perform ? ValidateDurationUnitStyle(unit, style, display, prevStyle).
    validate_duration_unit_style(vm, unit_property_key, style, display, previous_style, display_field)?;

    // 8. If unit is "hours" and twoDigitHours is true, set style to "2-digit".
    if unit == Unit::Hours && two_digit_hours {
        style = ValueStyle::TwoDigit;
    }

    // 9. If unit is "minutes" or "seconds" and prevStyle is "numeric" or "2-digit", set style to "2-digit".
    if matches!(unit, Unit::Minutes | Unit::Seconds)
        && matches!(previous_style, Some(ValueStyle::Numeric | ValueStyle::TwoDigit))
    {
        style = ValueStyle::TwoDigit;
    }

    // 10. Return the Duration Unit Options Record { [[Style]]: style, [[Display]]: display }.
    Ok(DurationUnitOptions { style, display })
}

/// Crypto::SignedBigInteger(double) of a duration field, which is integral.
fn big_integer_of_field(value: f64) -> SignedBigInteger {
    SignedBigInteger::from_f64(value).expect("a duration field is finite")
}

// 13.5.7 ComputeFractionalDigits ( durationFormat, duration ), https://tc39.es/ecma402/#sec-computefractionaldigits
// 15.9.6 ComputeFractionalDigits ( durationFormat, duration ), https://tc39.es/proposal-temporal/#sec-computefractionaldigits
pub fn compute_fractional_digits(duration_format: &DurationFormat, duration: &Duration) -> BigFraction {
    // 1. Let result be 0.
    let mut result = BigFraction::default();

    // 2. Let exponent be 3.
    let mut exponent: f64 = 3.0;

    // 3. For each row of Table 24, except the header row, in table order, do
    for duration_instances_component in &DURATION_INSTANCES_COMPONENTS {
        // a. Let unitOptions be the value of durationFormat's internal slot whose name is the Internal Slot value of the current row.
        let unit_options = (duration_instances_component.get_internal_slot)(duration_format);

        // b. If unitOptions.[[Style]] is "fractional", then
        if unit_options.style == ValueStyle::Fractional {
            // i. Let unit be the Unit value of the current row.
            let unit = duration_instances_component.unit;

            // ii. Assert: IsFractionalSecondUnitName(unit) is true.
            assert!(is_fractional_second_unit_name(unit));

            // iii. Let value be the value of duration's field whose name is the Value Field value of the current row.
            let value = (duration_instances_component.value_slot)(duration);

            // iv. Set result to result + (value / 10**exponent).
            result =
                &result + &BigFraction::new(big_integer_of_field(value), big_integer_of_field(10f64.powf(exponent)));

            // v. Set exponent to exponent + 3.
            exponent += 3.0;
        }
    }

    // 4. Return result.
    result
}

// 13.5.8 NextUnitFractional ( durationFormat, unit ), https://tc39.es/ecma402/#sec-nextunitfractional
pub fn next_unit_fractional(duration_format: &DurationFormat, unit: Unit) -> bool {
    // 1. If unit is "seconds" and durationFormat.[[MillisecondsOptions]].[[Style]] is "fractional", return true.
    if unit == Unit::Seconds && duration_format.milliseconds_options().style == ValueStyle::Fractional {
        return true;
    }

    // 2. If unit is "milliseconds" and durationFormat.[[MicrosecondsOptions]].[[Style]] is "fractional", return true.
    if unit == Unit::Milliseconds && duration_format.microseconds_options().style == ValueStyle::Fractional {
        return true;
    }

    // 3. If unit is "microseconds" and durationFormat.[[NanosecondsOptions]].[[Style]] is "fractional", return true.
    if unit == Unit::Microseconds && duration_format.nanoseconds_options().style == ValueStyle::Fractional {
        return true;
    }

    // 4. Return false.
    false
}

// 13.5.9 FormatNumericHours ( durationFormat, hoursValue, signDisplayed ), https://tc39.es/ecma402/#sec-formatnumerichours
pub fn format_numeric_hours(
    vm: &Vm,
    duration_format: Gc<DurationFormat>,
    hours_value: &MathematicalValue,
    sign_displayed: bool,
) -> Vec<DurationFormatPart> {
    let realm = vm.current_realm().expect("FormatNumericHours runs in a realm");
    let names = &vm.names;

    // 1. Let result be a new empty List.
    let mut result: Vec<DurationFormatPart> = Vec::new();

    // 2. Let hoursStyle be durationFormat.[[HoursOptions]].[[Style]].
    let hours_style = duration_format.hours_options().style;

    // 3. Assert: hoursStyle is "numeric" or hoursStyle is "2-digit".
    assert!(hours_style == ValueStyle::Numeric || hours_style == ValueStyle::TwoDigit);

    // 4. Let nfOpts be OrdinaryObjectCreate(null).
    let number_format_options = Object::create(vm, realm, None);

    // 5. Let numberingSystem be durationFormat.[[NumberingSystem]].
    let numbering_system = duration_format.numbering_system();

    // 6. Perform ! CreateDataPropertyOrThrow(nfOpts, "numberingSystem", numberingSystem).
    number_format_options
        .create_data_property_or_throw(
            vm,
            &names.numberingSystem,
            Value::from_string(PrimitiveString::create(vm, numbering_system)),
        )
        .must();

    // 7. If hoursStyle is "2-digit", then
    if hours_style == ValueStyle::TwoDigit {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumIntegerDigits", 2𝔽).
        number_format_options
            .create_data_property_or_throw(vm, &names.minimumIntegerDigits, Value::from_i32(2))
            .must();
    }

    // 8. If signDisplayed is false, then
    if !sign_displayed {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "signDisplay", "never").
        number_format_options
            .create_data_property_or_throw(vm, &names.signDisplay, string_value(vm, "never"))
            .must();
    }

    // 9. Perform ! CreateDataPropertyOrThrow(nfOpts, "useGrouping", false).
    number_format_options
        .create_data_property_or_throw(vm, &names.useGrouping, Value::from_bool(false))
        .must();

    // 10. Let nf be ! Construct(%Intl.NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
    let number_format = construct_number_format(vm, &duration_format, number_format_options);

    // 11. Let hoursParts be PartitionNumberPattern(nf, hoursValue).
    let hours_parts = partition_number_pattern(&number_format, hours_value);

    // 12. For each Record { [[Type]], [[Value]] } part of hoursParts, do
    result.reserve(hours_parts.len());

    for part in hours_parts {
        // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: "hour" } to result.
        result.push(DurationFormatPart {
            type_: part.type_,
            value: part.value,
            unit: Utf16String::from_utf8("hour"),
        });
    }

    // 13. Return result.
    result
}

// 13.5.10 FormatNumericMinutes ( durationFormat, minutesValue, hoursDisplayed, signDisplayed ), https://tc39.es/ecma402/#sec-formatnumericminutes
pub fn format_numeric_minutes(
    vm: &Vm,
    duration_format: Gc<DurationFormat>,
    minutes_value: &MathematicalValue,
    hours_displayed: bool,
    sign_displayed: bool,
) -> Vec<DurationFormatPart> {
    let realm = vm.current_realm().expect("FormatNumericMinutes runs in a realm");
    let names = &vm.names;

    // 1. Let result be a new empty List.
    let mut result: Vec<DurationFormatPart> = Vec::new();

    // 2. If hoursDisplayed is true, then
    if hours_displayed {
        // a. Let separator be durationFormat.[[HourMinuteSeparator]].
        let separator = duration_format.hour_minute_separator();

        // b. Append the Record { [[Type]]: "literal", [[Value]]: separator, [[Unit]]: EMPTY } to result.
        result.push(DurationFormatPart {
            type_: Utf16String::from_utf8("literal"),
            value: separator,
            unit: Utf16String::default(),
        });
    }

    // 3. Let minutesStyle be durationFormat.[[MinutesOptions]].[[Style]].
    let minutes_style = duration_format.minutes_options().style;

    // 4. Assert: minutesStyle is "numeric" or minutesStyle is "2-digit".
    assert!(minutes_style == ValueStyle::Numeric || minutes_style == ValueStyle::TwoDigit);

    // 5. Let nfOpts be OrdinaryObjectCreate(null).
    let number_format_options = Object::create(vm, realm, None);

    // 6. Let numberingSystem be durationFormat.[[NumberingSystem]].
    let numbering_system = duration_format.numbering_system();

    // 7. Perform ! CreateDataPropertyOrThrow(nfOpts, "numberingSystem", numberingSystem).
    number_format_options
        .create_data_property_or_throw(
            vm,
            &names.numberingSystem,
            Value::from_string(PrimitiveString::create(vm, numbering_system)),
        )
        .must();

    // 8. If minutesStyle is "2-digit", then
    if minutes_style == ValueStyle::TwoDigit {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumIntegerDigits", 2𝔽).
        number_format_options
            .create_data_property_or_throw(vm, &names.minimumIntegerDigits, Value::from_i32(2))
            .must();
    }

    // 9. If signDisplayed is false, then
    if !sign_displayed {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "signDisplay", "never").
        number_format_options
            .create_data_property_or_throw(vm, &names.signDisplay, string_value(vm, "never"))
            .must();
    }

    // 10. Perform ! CreateDataPropertyOrThrow(nfOpts, "useGrouping", false).
    number_format_options
        .create_data_property_or_throw(vm, &names.useGrouping, Value::from_bool(false))
        .must();

    // 11. Let nf be ! Construct(%Intl.NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
    let number_format = construct_number_format(vm, &duration_format, number_format_options);

    // 12. Let minutesParts be PartitionNumberPattern(nf, minutesValue).
    let minutes_parts = partition_number_pattern(&number_format, minutes_value);

    // 13. For each Record { [[Type]], [[Value]] } part of minutesParts, do
    result.reserve(minutes_parts.len());

    for part in minutes_parts {
        // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: "minute" } to result.
        result.push(DurationFormatPart {
            type_: part.type_,
            value: part.value,
            unit: Utf16String::from_utf8("minute"),
        });
    }

    // 14. Return result.
    result
}

// 13.5.11 FormatNumericSeconds ( durationFormat, secondsValue, minutesDisplayed, signDisplayed ), https://tc39.es/ecma402/#sec-formatnumericseconds
pub fn format_numeric_seconds(
    vm: &Vm,
    duration_format: Gc<DurationFormat>,
    seconds_value: &MathematicalValue,
    minutes_displayed: bool,
    sign_displayed: bool,
) -> Vec<DurationFormatPart> {
    let realm = vm.current_realm().expect("FormatNumericSeconds runs in a realm");
    let names = &vm.names;

    // 1. Let result be a new empty List.
    let mut result: Vec<DurationFormatPart> = Vec::new();

    // 2. If minutesDisplayed is true, then
    if minutes_displayed {
        // a. Let separator be durationFormat.[[MinuteSecondSeparator]].
        let separator = duration_format.minute_second_separator();

        // b. Append the Record { [[Type]]: "literal", [[Value]]: separator, [[Unit]]: EMPTY } to result.
        result.push(DurationFormatPart {
            type_: Utf16String::from_utf8("literal"),
            value: separator,
            unit: Utf16String::default(),
        });
    }

    // 3. Let secondsStyle be durationFormat.[[SecondsOptions]].[[Style]].
    let seconds_style = duration_format.seconds_options().style;

    // 4. Assert: secondsStyle is "numeric" or secondsStyle is "2-digit".
    assert!(seconds_style == ValueStyle::Numeric || seconds_style == ValueStyle::TwoDigit);

    // 5. Let nfOpts be OrdinaryObjectCreate(null).
    let number_format_options = Object::create(vm, realm, None);

    // 6. Let numberingSystem be durationFormat.[[NumberingSystem]].
    let numbering_system = duration_format.numbering_system();

    // 7. Perform ! CreateDataPropertyOrThrow(nfOpts, "numberingSystem", numberingSystem).
    number_format_options
        .create_data_property_or_throw(
            vm,
            &names.numberingSystem,
            Value::from_string(PrimitiveString::create(vm, numbering_system)),
        )
        .must();

    // 8. If secondsStyle is "2-digit", then
    if seconds_style == ValueStyle::TwoDigit {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumIntegerDigits", 2𝔽).
        number_format_options
            .create_data_property_or_throw(vm, &names.minimumIntegerDigits, Value::from_i32(2))
            .must();
    }

    // 9. If signDisplayed is false, then
    if !sign_displayed {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "signDisplay", "never").
        number_format_options
            .create_data_property_or_throw(vm, &names.signDisplay, string_value(vm, "never"))
            .must();
    }

    // 10. Perform ! CreateDataPropertyOrThrow(nfOpts, "useGrouping", false).
    number_format_options
        .create_data_property_or_throw(vm, &names.useGrouping, Value::from_bool(false))
        .must();

    // 11. Let fractionDigits be durationFormat.[[FractionalDigits]].
    // 12. If fractionDigits is undefined, then
    if !duration_format.has_fractional_digits() {
        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "maximumFractionDigits", 9𝔽).
        number_format_options
            .create_data_property_or_throw(vm, &names.maximumFractionDigits, Value::from_i32(9))
            .must();

        // b. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumFractionDigits", +0𝔽).
        number_format_options
            .create_data_property_or_throw(vm, &names.minimumFractionDigits, Value::from_i32(0))
            .must();
    }
    // 13. Else,
    else {
        let fraction_digits = duration_format.fractional_digits();

        // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "maximumFractionDigits", fractionDigits).
        number_format_options
            .create_data_property_or_throw(
                vm,
                &names.maximumFractionDigits,
                Value::from_i32(i32::from(fraction_digits)),
            )
            .must();

        // b. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumFractionDigits", fractionDigits).
        number_format_options
            .create_data_property_or_throw(
                vm,
                &names.minimumFractionDigits,
                Value::from_i32(i32::from(fraction_digits)),
            )
            .must();
    }

    // 14. Perform ! CreateDataPropertyOrThrow(nfOpts, "roundingMode", "trunc").
    number_format_options
        .create_data_property_or_throw(vm, &names.roundingMode, string_value(vm, "trunc"))
        .must();

    // 15. Let nf be ! Construct(%Intl.NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
    let number_format = construct_number_format(vm, &duration_format, number_format_options);

    // 16. Let secondsParts be PartitionNumberPattern(nf, secondsValue).
    let seconds_parts = partition_number_pattern(&number_format, seconds_value);

    // 17. For each Record { [[Type]], [[Value]] } part of secondsParts, do
    result.reserve(seconds_parts.len());

    for part in seconds_parts {
        // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: "second" } to result.
        result.push(DurationFormatPart {
            type_: part.type_,
            value: part.value,
            unit: Utf16String::from_utf8("second"),
        });
    }

    // 18. Return result.
    result
}

// 13.5.12 FormatNumericUnits ( durationFormat, duration, firstNumericUnit, signDisplayed ), https://tc39.es/ecma402/#sec-formatnumericunits
// 15.9.7 FormatNumericUnits ( durationFormat, duration, firstNumericUnit, signDisplayed ), https://tc39.es/proposal-temporal/#sec-formatnumericunits
pub fn format_numeric_units(
    vm: &Vm,
    duration_format: Gc<DurationFormat>,
    duration: Gc<Duration>,
    first_numeric_unit: Unit,
    mut sign_displayed: bool,
) -> Vec<DurationFormatPart> {
    // 1. Assert: firstNumericUnit is "hours", "minutes", or "seconds".
    assert!(matches!(
        first_numeric_unit,
        Unit::Hours | Unit::Minutes | Unit::Seconds
    ));

    // 2. Let numericPartsList be a new empty List.
    let mut numeric_parts_list: Vec<DurationFormatPart> = Vec::new();

    // 3. Let hoursValue be duration.[[Hours]].
    let hours_value = duration.hours();

    // 4. Let hoursDisplay be durationFormat.[[HoursOptions]].[[Display]].
    let hours_display = duration_format.hours_options().display;

    // 5. Let minutesDisplay be durationFormat.[[MinutesOptions]].[[Display]].
    let minutes_value = duration.minutes();

    // 6. Let minutesDisplay be durationFormat.[[MinutesDisplay]].
    let minutes_display = duration_format.minutes_options().display;

    // 7. Let secondsValue be duration.[[Seconds]].
    let mut seconds_value = BigFraction::from_double(duration.seconds());

    // 8. If duration.[[Milliseconds]] is not 0 or duration.[[Microseconds]] is not 0 or duration.[[Nanoseconds]] is not 0, then
    if duration.milliseconds() != 0.0 || duration.microseconds() != 0.0 || duration.nanoseconds() != 0.0 {
        // a. Set secondsValue to secondsValue + ComputeFractionalDigits(durationFormat, duration).
        seconds_value = &seconds_value + &compute_fractional_digits(&duration_format, &duration);
    }

    // 9. Let secondsDisplay be durationFormat.[[SecondsOptions]].[[Display]].
    let seconds_display = duration_format.seconds_options().display;

    // 10. Let hoursFormatted be false.
    let mut hours_formatted = false;

    // 11. If firstNumericUnit is "hours", then
    if first_numeric_unit == Unit::Hours {
        // a. If hoursValue is not 0 or hoursDisplay is "always", then
        if hours_value != 0.0 || hours_display == Display::Always {
            // i. Set hoursFormatted to true.
            hours_formatted = true;
        }
    }

    // 12. If secondsValue is not 0 or secondsDisplay is "always", then
    //     a. Let secondsFormatted be true.
    // 13. Else,
    //     a. Let secondsFormatted be false.
    let seconds_formatted = !seconds_value.is_zero() || seconds_display == Display::Always;

    // 14. Let minutesFormatted be false.
    let mut minutes_formatted = false;

    // 15. If firstNumericUnit is "hours" or firstNumericUnit is "minutes", then
    if matches!(first_numeric_unit, Unit::Hours | Unit::Minutes) {
        // a. If hoursFormatted is true and secondsFormatted is true, then
        #[allow(clippy::if_same_then_else, reason = "the branches are separate steps of the spec")]
        if hours_formatted && seconds_formatted {
            // i. Set minutesFormatted to true.
            minutes_formatted = true;
        }
        // b. Else if minutesValue is not 0 or minutesDisplay is "always", then
        else if minutes_value != 0.0 || minutes_display == Display::Always {
            // i. Set minutesFormatted to true.
            minutes_formatted = true;
        }
    }

    // 16. If hoursFormatted is true, then
    if hours_formatted {
        let mut hours_mv = MathematicalValue::from_number(hours_value);

        // a. If signDisplayed is true, then
        if sign_displayed {
            // i. If hoursValue is 0 and DurationSign(duration) is -1, then
            if hours_value == 0.0 && duration_sign(&duration) == -1 {
                // 1. Set hoursValue to NEGATIVE-ZERO.
                hours_mv = MathematicalValue::from_symbol(MathematicalValueSymbol::NegativeZero);
            }
        }

        // b. Let hoursParts be FormatNumericHours(durationFormat, hoursValue, signDisplayed).
        let hours_parts = format_numeric_hours(vm, duration_format, &hours_mv, sign_displayed);

        // b. Set numericPartsList to the list-concatenation of numericPartsList and hoursParts.
        numeric_parts_list.extend(hours_parts);

        // c. Set signDisplayed to false.
        sign_displayed = false;
    }

    // 17. If minutesFormatted is true, then
    if minutes_formatted {
        let mut minutes_mv = MathematicalValue::from_number(minutes_value);

        // a. If signDisplayed is true, then
        if sign_displayed {
            // i. If minutesValue is 0 and DurationSign(duration) is -1, then
            if minutes_value == 0.0 && duration_sign(&duration) == -1 {
                // 1. Set minutesValue to NEGATIVE-ZERO.
                minutes_mv = MathematicalValue::from_symbol(MathematicalValueSymbol::NegativeZero);
            }
        }

        // b. Let minutesParts be FormatNumericMinutes(durationFormat, minutesValue, hoursFormatted, signDisplayed).
        let minutes_parts = format_numeric_minutes(vm, duration_format, &minutes_mv, hours_formatted, sign_displayed);

        // c. Set numericPartsList to the list-concatenation of numericPartsList and minutesParts.
        numeric_parts_list.extend(minutes_parts);

        // d. Set signDisplayed to false.
        sign_displayed = false;
    }

    // 18. If secondsFormatted is true, then
    if seconds_formatted {
        // a. Let secondsParts be FormatNumericSeconds(durationFormat, secondsValue, minutesFormatted, signDisplayed).
        let seconds_value_mv = MathematicalValue::from_string(seconds_value.to_utf16_string(9));
        let seconds_parts = format_numeric_seconds(
            vm,
            duration_format,
            &seconds_value_mv,
            minutes_formatted,
            sign_displayed,
        );

        // b. Set numericPartsList to the list-concatenation of numericPartsList and secondsParts.
        numeric_parts_list.extend(seconds_parts);
    }

    // 19. Return numericPartsList.
    numeric_parts_list
}

// 13.5.13 IsFractionalSecondUnitName ( unit ), https://tc39.es/ecma402/#sec-isfractionalsecondunitname
pub fn is_fractional_second_unit_name(unit: Unit) -> bool {
    // 1. If unit is one of "milliseconds", "microseconds", or "nanoseconds", return true.
    // 2. Return false.
    matches!(unit, Unit::Milliseconds | Unit::Microseconds | Unit::Nanoseconds)
}

// 13.5.14 ListFormatParts ( durationFormat, partitionedPartsList ), https://tc39.es/ecma402/#sec-listformatparts
pub fn list_format_parts(
    vm: &Vm,
    duration_format: Gc<DurationFormat>,
    partitioned_parts_list: Vec<Vec<DurationFormatPart>>,
) -> Vec<DurationFormatPart> {
    let realm = vm.current_realm().expect("ListFormatParts runs in a realm");
    let names = &vm.names;

    // 1. Let lfOpts be OrdinaryObjectCreate(null).
    let list_format_options = Object::create(vm, realm, None);

    // 2. Perform ! CreateDataPropertyOrThrow(lfOpts, "type", "unit").
    list_format_options
        .create_data_property_or_throw(vm, &names.type_, string_value(vm, "unit"))
        .must();

    // 3. Let listStyle be durationFormat.[[Style]].
    let mut list_style = duration_format.style();

    // 4. If listStyle is "digital", then
    if list_style == Style::Digital {
        // a. Set listStyle to "short".
        list_style = Style::Short;
    }

    // 5. Perform ! CreateDataPropertyOrThrow(lfOpts, "style", listStyle).
    let locale_list_style = unicode::style_to_string(match list_style {
        Style::Long => UnicodeStyle::Long,
        Style::Short => UnicodeStyle::Short,
        Style::Narrow => UnicodeStyle::Narrow,
        Style::Digital => unreachable!("the digital style was replaced by the short style"),
    });
    list_format_options
        .create_data_property_or_throw(vm, &names.style, string_value(vm, locale_list_style))
        .must();

    // 6. Let lf be ! Construct(%Intl.ListFormat%, « durationFormat.[[Locale]], lfOpts »).
    let list_format = construct_list_format(vm, &duration_format, list_format_options);

    // 7. Let strings be a new empty List.
    let mut strings: Vec<Utf16String> = Vec::with_capacity(partitioned_parts_list.len());

    // 8. For each element parts of partitionedPartsList, do
    for parts in &partitioned_parts_list {
        // a. Let string be the empty String.
        let mut string = Utf16StringBuilder::new();

        // b. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
        for part in parts {
            // i. Set string to the string-concatenation of string and part.[[Value]].
            string.append(Utf16View::of_string(&part.value));
        }

        // c. Append string to strings.
        strings.push(string.to_utf16_string());
    }

    // 9. Let formattedPartsList be CreatePartsFromList(lf, strings).
    let formatted_parts_list = create_parts_from_list(&list_format, &strings);

    // 10. Let partitionedPartsIndex be 0.
    let mut partitioned_parts_index = 0;

    // 11. Let partitionedLength be the number of elements in partitionedPartsList.
    let partitioned_length = partitioned_parts_list.len();

    let mut partitioned_parts_list = partitioned_parts_list.into_iter();

    // 12. Let flattenedPartsList be a new empty List.
    let mut flattened_parts_list: Vec<DurationFormatPart> = Vec::new();

    // 13. For each Record { [[Type]], [[Value]] } listPart in formattedPartsList, do
    for list_part in formatted_parts_list {
        // a. If listPart.[[Type]] is "element", then
        if Utf16View::of_string(&list_part.type_) == "element" {
            // i. Assert: partitionedPartsIndex < partitionedLength.
            assert!(partitioned_parts_index < partitioned_length);

            // ii. Let parts be partitionedPartsList[partitionedPartsIndex].
            let parts = partitioned_parts_list
                .next()
                .expect("there is a list of parts for each element");

            // iii. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
            for part in parts {
                // 1. Append part to flattenedPartsList.
                flattened_parts_list.push(part);
            }

            // iv. Set partitionedPartsIndex to partitionedPartsIndex + 1.
            partitioned_parts_index += 1;
        }
        // b. Else,
        else {
            // i. Assert: listPart.[[Type]] is "literal".
            assert!(Utf16View::of_string(&list_part.type_) == "literal");

            // ii. Append the Record { [[Type]]: "literal", [[Value]]: listPart.[[Value]], [[Unit]]: empty } to flattenedPartsList.
            flattened_parts_list.push(DurationFormatPart {
                type_: Utf16String::from_utf8("literal"),
                value: list_part.value,
                unit: Utf16String::default(),
            });
        }
    }

    // 14. Return flattenedPartsList.
    flattened_parts_list
}

// 13.5.15 PartitionDurationFormatPattern ( durationFormat, duration ), https://tc39.es/ecma402/#sec-partitiondurationformatpattern
// 15.9.8 PartitionDurationFormatPattern ( durationFormat, duration ), https://tc39.es/proposal-temporal/#sec-formatnumericunits
pub fn partition_duration_format_pattern(
    vm: &Vm,
    duration_format: Gc<DurationFormat>,
    duration: Gc<Duration>,
) -> Vec<DurationFormatPart> {
    let realm = vm
        .current_realm()
        .expect("PartitionDurationFormatPattern runs in a realm");
    let names = &vm.names;

    // 1. Let result be a new empty List.
    let mut result: Vec<Vec<DurationFormatPart>> = Vec::new();

    // 2. Let signDisplayed be true.
    let mut sign_displayed = true;

    // 3. Let numericUnitFound be false.
    let mut numeric_unit_found = false;

    // 4. While numericUnitFound is false, repeat for each row in Table 24 in table order, except the header row:
    let mut i = 0;
    while !numeric_unit_found && i < DURATION_INSTANCES_COMPONENTS.len() {
        let duration_instances_component = &DURATION_INSTANCES_COMPONENTS[i];
        i += 1;

        // a. Let value be the value of duration's field whose name is the Value Field value of the current row.
        let mut value = BigFraction::from_double((duration_instances_component.value_slot)(&duration));

        // b. Let unitOptions be the value of durationFormat's internal slot whose name is the Internal Slot value of the current row.
        // c. Let style be unitOptions.[[Style]].
        // d. Let display be unitOptions.[[Display]].
        let DurationUnitOptions { style, display } = (duration_instances_component.get_internal_slot)(&duration_format);

        // e. Let unit be the Unit value of the current row.
        let unit = duration_instances_component.unit;

        // f. Let numberFormatUnit be the NumberFormat Unit value of the current row.
        let number_format_unit = unit_to_number_format_property_key(vm, duration_instances_component.unit);

        // g. If style is "numeric" or "2-digit", then
        if style == ValueStyle::Numeric || style == ValueStyle::TwoDigit {
            // i. Let numericPartsList be FormatNumericUnits(durationFormat, duration, unit, signDisplayed).
            let numeric_parts_list = format_numeric_units(vm, duration_format, duration, unit, sign_displayed);

            // ii. If numericPartsList is not empty, append numericPartsList to result.
            if !numeric_parts_list.is_empty() {
                result.push(numeric_parts_list);
            }

            // iii. Set numericUnitFound to true.
            numeric_unit_found = true;
        }
        // h. Else,
        else {
            // i. Let nfOpts be OrdinaryObjectCreate(null).
            let number_format_options = Object::create(vm, realm, None);

            // ii. If NextUnitFractional(durationFormat, unit) is true, then
            if next_unit_fractional(&duration_format, unit) {
                // 1. Set value to value + ComputeFractionalDigits(durationFormat, duration).
                value = &value + &compute_fractional_digits(&duration_format, &duration);

                // 2. Let fractionDigits be durationFormat.[[FractionalDigits]].
                // 3. If fractionDigits is undefined, then
                if !duration_format.has_fractional_digits() {
                    // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "maximumFractionDigits", 9𝔽).
                    number_format_options
                        .create_data_property_or_throw(vm, &names.maximumFractionDigits, Value::from_i32(9))
                        .must();

                    // b. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumFractionDigits", +0𝔽).
                    number_format_options
                        .create_data_property_or_throw(vm, &names.minimumFractionDigits, Value::from_i32(0))
                        .must();
                }
                // 4. Else,
                else {
                    let fraction_digits = duration_format.fractional_digits();

                    // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "maximumFractionDigits", fractionDigits).
                    number_format_options
                        .create_data_property_or_throw(
                            vm,
                            &names.maximumFractionDigits,
                            Value::from_i32(i32::from(fraction_digits)),
                        )
                        .must();

                    // b. Perform ! CreateDataPropertyOrThrow(nfOpts, "minimumFractionDigits", fractionDigits).
                    number_format_options
                        .create_data_property_or_throw(
                            vm,
                            &names.minimumFractionDigits,
                            Value::from_i32(i32::from(fraction_digits)),
                        )
                        .must();
                }

                // 5. Perform ! CreateDataPropertyOrThrow(nfOpts, "roundingMode", "trunc").
                number_format_options
                    .create_data_property_or_throw(vm, &names.roundingMode, string_value(vm, "trunc"))
                    .must();

                // 6. Set numericUnitFound to true.
                numeric_unit_found = true;
            }

            // iii. If display is "always" or value is not 0, then
            if display == Display::Always || !value.is_zero() {
                let mut value_mv = MathematicalValue::from_string(value.to_utf16_string(9));

                // 1. Perform ! CreateDataPropertyOrThrow(nfOpts, "numberingSystem", durationFormat.[[NumberingSystem]]).
                number_format_options
                    .create_data_property_or_throw(
                        vm,
                        &names.numberingSystem,
                        Value::from_string(PrimitiveString::create(vm, duration_format.numbering_system())),
                    )
                    .must();

                // 2. If signDisplayed is true, then
                if sign_displayed {
                    // a. Set signDisplayed to false.
                    sign_displayed = false;

                    // b. If value is 0 and DurationSign(duration) is -1, set value to NEGATIVE-ZERO.
                    if value.is_zero() && duration_sign(&duration) == -1 {
                        value_mv = MathematicalValue::from_symbol(MathematicalValueSymbol::NegativeZero);
                    }
                }
                // 3. Else,
                else {
                    // a. Perform ! CreateDataPropertyOrThrow(nfOpts, "signDisplay", "never").
                    number_format_options
                        .create_data_property_or_throw(vm, &names.signDisplay, string_value(vm, "never"))
                        .must();
                }

                // 3. Perform ! CreateDataPropertyOrThrow(nfOpts, "style", "unit").
                number_format_options
                    .create_data_property_or_throw(vm, &names.style, string_value(vm, "unit"))
                    .must();

                // 4. Perform ! CreateDataPropertyOrThrow(nfOpts, "unit", numberFormatUnit).
                let unit_string = number_format_unit.to_utf16_string();
                number_format_options
                    .create_data_property_or_throw(
                        vm,
                        &names.unit,
                        Value::from_string(PrimitiveString::create(vm, unit_string.clone())),
                    )
                    .must();

                // 5. Perform ! CreateDataPropertyOrThrow(nfOpts, "unitDisplay", style).
                let locale_style = unicode::style_to_string(unicode_style_of(style));
                number_format_options
                    .create_data_property_or_throw(vm, &names.unitDisplay, string_value(vm, locale_style))
                    .must();

                // 6. Let nf be ! Construct(%Intl.NumberFormat%, « durationFormat.[[Locale]], nfOpts »).
                let number_format = construct_number_format(vm, &duration_format, number_format_options);

                // 7. Let parts be PartitionNumberPattern(nf, value).
                let parts = partition_number_pattern(&number_format, &value_mv);

                // 8. Let list be a new empty List.
                let mut list: Vec<DurationFormatPart> = Vec::with_capacity(parts.len());

                // 10. For each Record { [[Type]], [[Value]] } part of parts, do
                for part in parts {
                    // a. Append the Record { [[Type]]: part.[[Type]], [[Value]]: part.[[Value]], [[Unit]]: numberFormatUnit } to list.
                    list.push(DurationFormatPart {
                        type_: part.type_,
                        value: part.value,
                        unit: unit_string.clone(),
                    });
                }

                // 11. Append list to result.
                result.push(list);
            }
        }
    }

    // 5. Return ListFormatParts(durationFormat, result).
    list_format_parts(vm, duration_format, result)
}
