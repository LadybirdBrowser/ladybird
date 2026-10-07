/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The number formatting, plural rules, currency codes and digital duration format of LibUnicode
//! (Libraries/LibUnicode/NumberFormat.h, PluralRules.h, CurrencyCode.h and DurationFormat.h), through the C exports of
//! Libraries/LibUnicode/NumberFormatExports.h.

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;

use super::intl::{Style, UnicodeIntlText};
use super::{UnicodeTextMappingOutput, collect_text};
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Clone, Copy)]
struct UnicodeNumberFormatValue {
    is_string: bool,
    number: f64,
    string: UnicodeIntlText,
}

#[repr(C)]
struct UnicodeNumberFormatDisplayOptions {
    style: u8,
    sign_display: u8,
    notation: u8,
    has_compact_display: bool,
    compact_display: u8,
    grouping: u8,
    has_currency: bool,
    currency: UnicodeIntlText,
    has_currency_display: bool,
    currency_display: u8,
    has_currency_sign: bool,
    currency_sign: u8,
    has_unit: bool,
    unit: UnicodeIntlText,
    has_unit_display: bool,
    unit_display: u8,
}

#[repr(C)]
struct UnicodeNumberFormatRoundingOptions {
    type_: u8,
    mode: u8,
    trailing_zero_display: u8,
    has_min_significant_digits: bool,
    min_significant_digits: i32,
    has_max_significant_digits: bool,
    max_significant_digits: i32,
    has_min_fraction_digits: bool,
    min_fraction_digits: i32,
    has_max_fraction_digits: bool,
    max_fraction_digits: i32,
    min_integer_digits: i32,
    rounding_increment: i32,
}

type UnicodeAppendNumberFormatPart = unsafe extern "C" fn(
    context: *mut c_void,
    type_: *const u16,
    type_length: usize,
    value: *const u16,
    value_length: usize,
    source: *const u16,
    source_length: usize,
);
type UnicodeAppendPluralCategory = unsafe extern "C" fn(context: *mut c_void, category: u8);

unsafe extern "C" {
    fn unicode_number_format_create(
        locale: UnicodeIntlText,
        display_options: *const UnicodeNumberFormatDisplayOptions,
        rounding_options: *const UnicodeNumberFormatRoundingOptions,
    ) -> *mut c_void;
    fn unicode_number_format_format(
        number_format: *const c_void,
        value: UnicodeNumberFormatValue,
        output: UnicodeTextMappingOutput,
    );
    fn unicode_number_format_format_to_parts(
        number_format: *const c_void,
        value: UnicodeNumberFormatValue,
        context: *mut c_void,
        append_part: UnicodeAppendNumberFormatPart,
    );
    fn unicode_number_format_format_range(
        number_format: *const c_void,
        start: UnicodeNumberFormatValue,
        end: UnicodeNumberFormatValue,
        output: UnicodeTextMappingOutput,
    );
    fn unicode_number_format_format_range_to_parts(
        number_format: *const c_void,
        start: UnicodeNumberFormatValue,
        end: UnicodeNumberFormatValue,
        context: *mut c_void,
        append_part: UnicodeAppendNumberFormatPart,
    );
    fn unicode_number_format_create_plural_rules(number_format: *mut c_void, plural_form: u8);
    fn unicode_number_format_select_plural(number_format: *const c_void, value: UnicodeNumberFormatValue) -> u8;
    fn unicode_number_format_select_plural_range(
        number_format: *const c_void,
        start: UnicodeNumberFormatValue,
        end: UnicodeNumberFormatValue,
    ) -> u8;
    fn unicode_number_format_available_plural_categories(
        number_format: *const c_void,
        context: *mut c_void,
        append_category: UnicodeAppendPluralCategory,
    );
    fn unicode_number_format_destroy(number_format: *mut c_void);

    fn unicode_get_currency_code(currency: UnicodeIntlText, has_minor_unit: *mut bool, minor_unit: *mut i32) -> bool;

    fn unicode_digital_format(
        locale: UnicodeIntlText,
        hours_minutes_separator: UnicodeTextMappingOutput,
        minutes_seconds_separator: UnicodeTextMappingOutput,
        uses_two_digit_hours: *mut bool,
    );
}

/// Defines a LibUnicode enumeration with its from_string and to_string functions, which map each variant to the
/// string Intl uses for it.
macro_rules! define_unicode_enum {
    (
        $(#[$attribute:meta])* $name:ident, $from_string:ident, $to_string:ident,
        { $($variant:ident => $string:literal,)* }
    ) => {
        $(#[$attribute])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
        #[repr(u8)]
        pub enum $name {
            $($variant,)*
        }

        #[doc = concat!("Unicode::", stringify!($from_string), ".")]
        pub fn $from_string(string: Utf16View<'_>) -> $name {
            $(
                if string == $string {
                    return $name::$variant;
                }
            )*
            unreachable!(concat!("the ", stringify!($name), " is one of the values GetOption allows"))
        }

        #[doc = concat!("Unicode::", stringify!($to_string), ".")]
        pub fn $to_string(value: $name) -> &'static str {
            match value {
                $($name::$variant => $string,)*
            }
        }
    };
}

define_unicode_enum!(
    /// Unicode::NumberFormatStyle.
    NumberFormatStyle, number_format_style_from_string, number_format_style_to_string, {
        Decimal => "decimal",
        Percent => "percent",
        Currency => "currency",
        Unit => "unit",
    }
);

define_unicode_enum!(
    /// Unicode::SignDisplay.
    SignDisplay, sign_display_from_string, sign_display_to_string, {
        Auto => "auto",
        Never => "never",
        Always => "always",
        ExceptZero => "exceptZero",
        Negative => "negative",
    }
);

define_unicode_enum!(
    /// Unicode::Notation.
    Notation, notation_from_string, notation_to_string, {
        Standard => "standard",
        Scientific => "scientific",
        Engineering => "engineering",
        Compact => "compact",
    }
);

define_unicode_enum!(
    /// Unicode::CompactDisplay.
    CompactDisplay, compact_display_from_string, compact_display_to_string, {
        Short => "short",
        Long => "long",
    }
);

define_unicode_enum!(
    /// Unicode::Grouping.
    Grouping, grouping_from_string, grouping_to_string, {
        Always => "always",
        Auto => "auto",
        Min2 => "min2",
        False => "false",
    }
);

define_unicode_enum!(
    /// Unicode::CurrencyDisplay.
    CurrencyDisplay, currency_display_from_string, currency_display_to_string, {
        Code => "code",
        Symbol => "symbol",
        NarrowSymbol => "narrowSymbol",
        Name => "name",
    }
);

define_unicode_enum!(
    /// Unicode::CurrencySign.
    CurrencySign, currency_sign_from_string, currency_sign_to_string, {
        Standard => "standard",
        Accounting => "accounting",
    }
);

define_unicode_enum!(
    /// Unicode::RoundingType.
    RoundingType, rounding_type_from_string, rounding_type_to_string, {
        SignificantDigits => "significantDigits",
        FractionDigits => "fractionDigits",
        MorePrecision => "morePrecision",
        LessPrecision => "lessPrecision",
    }
);

define_unicode_enum!(
    /// Unicode::RoundingMode.
    RoundingMode, rounding_mode_from_string, rounding_mode_to_string, {
        Ceil => "ceil",
        Expand => "expand",
        Floor => "floor",
        HalfCeil => "halfCeil",
        HalfEven => "halfEven",
        HalfExpand => "halfExpand",
        HalfFloor => "halfFloor",
        HalfTrunc => "halfTrunc",
        Trunc => "trunc",
    }
);

define_unicode_enum!(
    /// Unicode::TrailingZeroDisplay.
    TrailingZeroDisplay, trailing_zero_display_from_string, trailing_zero_display_to_string, {
        Auto => "auto",
        StripIfInteger => "stripIfInteger",
    }
);

define_unicode_enum!(
    /// Unicode::PluralForm.
    PluralForm, plural_form_from_string, plural_form_to_string, {
        Cardinal => "cardinal",
        Ordinal => "ordinal",
    }
);

define_unicode_enum!(
    /// Unicode::PluralCategory, sorted in the preferred order of Intl.PluralRules.prototype.resolvedOptions, then the
    /// explicit 0 and 1 rules of https://unicode.org/reports/tr35/tr35-numbers.html#Explicit_0_1_rules.
    PluralCategory, plural_category_from_string, plural_category_to_string, {
        Zero => "zero",
        One => "one",
        Two => "two",
        Few => "few",
        Many => "many",
        Other => "other",
        ExactlyZero => "0",
        ExactlyOne => "1",
    }
);

impl PluralCategory {
    fn from_u8(category: u8) -> Self {
        const CATEGORIES: [PluralCategory; 8] = [
            PluralCategory::Zero,
            PluralCategory::One,
            PluralCategory::Two,
            PluralCategory::Few,
            PluralCategory::Many,
            PluralCategory::Other,
            PluralCategory::ExactlyZero,
            PluralCategory::ExactlyOne,
        ];
        CATEGORIES[usize::from(category)]
    }
}

/// Unicode::DisplayOptions.
#[derive(Clone)]
pub struct DisplayOptions {
    pub style: NumberFormatStyle,
    pub sign_display: SignDisplay,
    pub notation: Notation,
    pub compact_display: Option<CompactDisplay>,
    pub grouping: Grouping,
    pub currency: Option<Utf16String>,
    pub currency_display: Option<CurrencyDisplay>,
    pub currency_sign: Option<CurrencySign>,
    pub unit: Option<Utf16String>,
    pub unit_display: Option<Style>,
}

impl Default for DisplayOptions {
    fn default() -> Self {
        Self {
            style: NumberFormatStyle::Decimal,
            sign_display: SignDisplay::Auto,
            notation: Notation::Standard,
            compact_display: None,
            grouping: Grouping::Always,
            currency: None,
            currency_display: None,
            currency_sign: None,
            unit: None,
            unit_display: None,
        }
    }
}

/// Unicode::RoundingOptions.
#[derive(Clone)]
pub struct RoundingOptions {
    pub type_: RoundingType,
    pub mode: RoundingMode,
    pub trailing_zero_display: TrailingZeroDisplay,
    pub min_significant_digits: Option<i32>,
    pub max_significant_digits: Option<i32>,
    pub min_fraction_digits: Option<i32>,
    pub max_fraction_digits: Option<i32>,
    pub min_integer_digits: i32,
    pub rounding_increment: i32,
}

impl Default for RoundingOptions {
    fn default() -> Self {
        Self {
            type_: RoundingType::MorePrecision,
            mode: RoundingMode::HalfExpand,
            trailing_zero_display: TrailingZeroDisplay::Auto,
            min_significant_digits: None,
            max_significant_digits: None,
            min_fraction_digits: None,
            max_fraction_digits: None,
            min_integer_digits: 0,
            rounding_increment: 1,
        }
    }
}

/// Unicode::NumberFormat::Value: a double, or a decimal number as a string.
#[derive(Clone)]
pub enum NumberFormatValue {
    Number(f64),
    String(Utf16String),
}

impl NumberFormatValue {
    fn to_export(&self) -> UnicodeNumberFormatValue {
        match self {
            NumberFormatValue::Number(number) => UnicodeNumberFormatValue {
                is_string: false,
                number: *number,
                string: UnicodeIntlText::of(Utf16View::Ascii(&[])),
            },
            NumberFormatValue::String(string) => UnicodeNumberFormatValue {
                is_string: true,
                number: 0.0,
                string: UnicodeIntlText::of(Utf16View::of_string(string)),
            },
        }
    }
}

/// Unicode::NumberFormat::Partition.
pub struct NumberFormatPartition {
    pub type_: Utf16String,
    pub value: Utf16String,
    pub source: Utf16String,
}

unsafe extern "C" fn append_number_format_part(
    context: *mut c_void,
    type_: *const u16,
    type_length: usize,
    value: *const u16,
    value_length: usize,
    source: *const u16,
    source_length: usize,
) {
    // SAFETY: The context is the list the caller passed, and the export passes valid code units for the lengths.
    let (parts, type_, value, source) = unsafe {
        (
            &mut *context.cast::<Vec<NumberFormatPartition>>(),
            core::slice::from_raw_parts(type_, type_length),
            core::slice::from_raw_parts(value, value_length),
            core::slice::from_raw_parts(source, source_length),
        )
    };
    parts.push(NumberFormatPartition {
        type_: Utf16String::from_utf16(type_),
        value: Utf16String::from_utf16(value),
        source: Utf16String::from_utf16(source),
    });
}

unsafe extern "C" fn append_plural_category(context: *mut c_void, category: u8) {
    // SAFETY: The context is the list the caller passed.
    let categories = unsafe { &mut *context.cast::<Vec<PluralCategory>>() };
    categories.push(PluralCategory::from_u8(category));
}

fn text_output(write: impl FnOnce(UnicodeTextMappingOutput)) -> Utf16String {
    let ((), text) = collect_text(write);
    Utf16String::from_utf16(&text)
}

fn optional_text(text: Option<&Utf16String>) -> (bool, UnicodeIntlText) {
    match text {
        Some(text) => (true, UnicodeIntlText::of(Utf16View::of_string(text))),
        None => (false, UnicodeIntlText::of(Utf16View::Ascii(&[]))),
    }
}

/// A Unicode::NumberFormat, which this owns.
pub struct NumberFormat {
    number_format: NonNull<c_void>,
}

impl NumberFormat {
    /// Unicode::NumberFormat::create.
    pub fn create(locale: Utf16View<'_>, display_options: &DisplayOptions, rounding_options: &RoundingOptions) -> Self {
        let (has_currency, currency) = optional_text(display_options.currency.as_ref());
        let (has_unit, unit) = optional_text(display_options.unit.as_ref());
        let display_options = UnicodeNumberFormatDisplayOptions {
            style: display_options.style as u8,
            sign_display: display_options.sign_display as u8,
            notation: display_options.notation as u8,
            has_compact_display: display_options.compact_display.is_some(),
            compact_display: display_options.compact_display.map_or(0, |value| value as u8),
            grouping: display_options.grouping as u8,
            has_currency,
            currency,
            has_currency_display: display_options.currency_display.is_some(),
            currency_display: display_options.currency_display.map_or(0, |value| value as u8),
            has_currency_sign: display_options.currency_sign.is_some(),
            currency_sign: display_options.currency_sign.map_or(0, |value| value as u8),
            has_unit,
            unit,
            has_unit_display: display_options.unit_display.is_some(),
            unit_display: display_options.unit_display.map_or(0, |value| value as u8),
        };
        let rounding_options = UnicodeNumberFormatRoundingOptions {
            type_: rounding_options.type_ as u8,
            mode: rounding_options.mode as u8,
            trailing_zero_display: rounding_options.trailing_zero_display as u8,
            has_min_significant_digits: rounding_options.min_significant_digits.is_some(),
            min_significant_digits: rounding_options.min_significant_digits.unwrap_or(0),
            has_max_significant_digits: rounding_options.max_significant_digits.is_some(),
            max_significant_digits: rounding_options.max_significant_digits.unwrap_or(0),
            has_min_fraction_digits: rounding_options.min_fraction_digits.is_some(),
            min_fraction_digits: rounding_options.min_fraction_digits.unwrap_or(0),
            has_max_fraction_digits: rounding_options.max_fraction_digits.is_some(),
            max_fraction_digits: rounding_options.max_fraction_digits.unwrap_or(0),
            min_integer_digits: rounding_options.min_integer_digits,
            rounding_increment: rounding_options.rounding_increment,
        };
        // SAFETY: The locale and the strings of the options are valid for their lengths during the call.
        let number_format = unsafe {
            unicode_number_format_create(
                UnicodeIntlText::of(locale),
                &raw const display_options,
                &raw const rounding_options,
            )
        };
        Self {
            number_format: NonNull::new(number_format).expect("Unicode::NumberFormat::create returns a number format"),
        }
    }

    pub fn format(&self, value: &NumberFormatValue) -> Utf16String {
        // SAFETY: The number format is alive, the value's string outlives the call, and the output writes into a Vec.
        text_output(|output| unsafe {
            unicode_number_format_format(self.number_format.as_ptr(), value.to_export(), output);
        })
    }

    pub fn format_to_parts(&self, value: &NumberFormatValue) -> Vec<NumberFormatPartition> {
        let mut parts: Vec<NumberFormatPartition> = Vec::new();
        // SAFETY: The number format is alive, the value's string outlives the call, and the callback appends to a Vec.
        unsafe {
            unicode_number_format_format_to_parts(
                self.number_format.as_ptr(),
                value.to_export(),
                (&raw mut parts).cast(),
                append_number_format_part,
            );
        }
        parts
    }

    pub fn format_range(&self, start: &NumberFormatValue, end: &NumberFormatValue) -> Utf16String {
        // SAFETY: The number format is alive, the values' strings outlive the call, and the output writes into a Vec.
        text_output(|output| unsafe {
            unicode_number_format_format_range(self.number_format.as_ptr(), start.to_export(), end.to_export(), output);
        })
    }

    pub fn format_range_to_parts(
        &self,
        start: &NumberFormatValue,
        end: &NumberFormatValue,
    ) -> Vec<NumberFormatPartition> {
        let mut parts: Vec<NumberFormatPartition> = Vec::new();
        // SAFETY: The number format is alive, the values' strings outlive the call, and the callback appends to a Vec.
        unsafe {
            unicode_number_format_format_range_to_parts(
                self.number_format.as_ptr(),
                start.to_export(),
                end.to_export(),
                (&raw mut parts).cast(),
                append_number_format_part,
            );
        }
        parts
    }

    pub fn create_plural_rules(&mut self, plural_form: PluralForm) {
        // SAFETY: The number format is alive, and this has the only reference to it.
        unsafe { unicode_number_format_create_plural_rules(self.number_format.as_ptr(), plural_form as u8) };
    }

    pub fn select_plural(&self, value: &NumberFormatValue) -> PluralCategory {
        // SAFETY: The number format is alive, and the value's string outlives the call.
        PluralCategory::from_u8(unsafe {
            unicode_number_format_select_plural(self.number_format.as_ptr(), value.to_export())
        })
    }

    pub fn select_plural_range(&self, start: &NumberFormatValue, end: &NumberFormatValue) -> PluralCategory {
        // SAFETY: The number format is alive, and the values' strings outlive the call.
        PluralCategory::from_u8(unsafe {
            unicode_number_format_select_plural_range(self.number_format.as_ptr(), start.to_export(), end.to_export())
        })
    }

    pub fn available_plural_categories(&self) -> Vec<PluralCategory> {
        let mut categories: Vec<PluralCategory> = Vec::new();
        // SAFETY: The number format is alive, and the callback appends to a Vec.
        unsafe {
            unicode_number_format_available_plural_categories(
                self.number_format.as_ptr(),
                (&raw mut categories).cast(),
                append_plural_category,
            );
        }
        categories
    }
}

impl Drop for NumberFormat {
    fn drop(&mut self) {
        // SAFETY: This owns the number format, which nothing uses after it is dropped.
        unsafe { unicode_number_format_destroy(self.number_format.as_ptr()) };
    }
}

/// Unicode::CurrencyCode.
pub struct CurrencyCode {
    pub minor_unit: Option<i32>,
}

/// Unicode::get_currency_code.
pub fn get_currency_code(currency: Utf16View<'_>) -> Option<CurrencyCode> {
    let mut has_minor_unit = false;
    let mut minor_unit = 0;
    // SAFETY: The currency is valid for its length.
    let found = unsafe {
        unicode_get_currency_code(
            UnicodeIntlText::of(currency),
            &raw mut has_minor_unit,
            &raw mut minor_unit,
        )
    };
    found.then(|| CurrencyCode {
        minor_unit: has_minor_unit.then_some(minor_unit),
    })
}

/// Unicode::DigitalFormat.
pub struct DigitalFormat {
    pub hours_minutes_separator: Utf16String,
    pub minutes_seconds_separator: Utf16String,
    pub uses_two_digit_hours: bool,
}

/// Unicode::digital_format.
pub fn digital_format(locale: Utf16View<'_>) -> DigitalFormat {
    let mut uses_two_digit_hours = false;
    let mut minutes_seconds_separator: Vec<u16> = Vec::new();
    let ((), hours_minutes_separator) = collect_text(|hours_minutes_separator| {
        let ((), separator) = collect_text(|minutes_seconds_separator| {
            // SAFETY: The locale is valid for its length, and the outputs write into Vecs.
            unsafe {
                unicode_digital_format(
                    UnicodeIntlText::of(locale),
                    hours_minutes_separator,
                    minutes_seconds_separator,
                    &raw mut uses_two_digit_hours,
                );
            }
        });
        minutes_seconds_separator = separator;
    });
    DigitalFormat {
        hours_minutes_separator: Utf16String::from_utf16(&hours_minutes_separator),
        minutes_seconds_separator: Utf16String::from_utf16(&minutes_seconds_separator),
        uses_two_digit_hours,
    }
}
