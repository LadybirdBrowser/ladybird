/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::abstract_operations::StringOrBoolean;
use crate::runtime::intl::intl_object::{IntlObject, ResolutionOptionDescriptor};
use crate::runtime::intl::mathematical_value::{MathematicalValue, MathematicalValueSymbol, bigint_to_base_10};
use crate::runtime::intl::number_format_function::NumberFormatFunction;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::value::PreferredType;
use crate::runtime::value_conversions::{
    parse_string_numeric_literal, string_to_number, unsigned_big_integer_from_base,
};
use crate::unicode::intl::{self as unicode_intl, Style};
use crate::unicode::number_format::{
    self as unicode, CompactDisplay, CurrencyDisplay, CurrencySign, DisplayOptions, Grouping, Notation,
    NumberFormatPartition, NumberFormatStyle, RoundingMode, RoundingOptions, RoundingType, SignDisplay,
    TrailingZeroDisplay,
};
use crate::utf16::Utf16View;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ComputedRoundingPriority {
    Auto,
    MorePrecision,
    LessPrecision,
    Invalid,
}

/// The slots Intl.NumberFormat and Intl.PluralRules share.
#[repr(C)]
#[derive(Trace)]
pub struct NumberFormatBase {
    base: Object,
    #[gc(untraced)]
    locale: GcRefCell<Utf16String>, // [[Locale]]
    #[gc(untraced)]
    min_integer_digits: Cell<i32>, // [[MinimumIntegerDigits]]
    #[gc(untraced)]
    min_fraction_digits: Cell<Option<i32>>, // [[MinimumFractionDigits]]
    #[gc(untraced)]
    max_fraction_digits: Cell<Option<i32>>, // [[MaximumFractionDigits]]
    #[gc(untraced)]
    min_significant_digits: Cell<Option<i32>>, // [[MinimumSignificantDigits]]
    #[gc(untraced)]
    max_significant_digits: Cell<Option<i32>>, // [[MaximumSignificantDigits]]
    #[gc(untraced)]
    notation: Cell<Notation>, // [[Notation]]
    #[gc(untraced)]
    compact_display: Cell<Option<CompactDisplay>>, // [[CompactDisplay]]
    #[gc(untraced)]
    rounding_type: Cell<RoundingType>, // [[RoundingType]]
    #[gc(untraced)]
    computed_rounding_priority: Cell<ComputedRoundingPriority>, // [[ComputedRoundingPriority]]
    #[gc(untraced)]
    rounding_mode: Cell<RoundingMode>, // [[RoundingMode]]
    #[gc(untraced)]
    rounding_increment: Cell<i32>, // [[RoundingIncrement]]
    #[gc(untraced)]
    trailing_zero_display: Cell<TrailingZeroDisplay>, // [[TrailingZeroDisplay]]

    // Non-standard. Stores the ICU number formatter for the Intl object's formatting options.
    #[gc(untraced)]
    formatter: GcRefCell<Option<unicode::NumberFormat>>,
}

define_cell!(NumberFormatBase, Object, extends: [Object], finalize: finalize);

impl Deref for NumberFormatBase {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for NumberFormatBase {
    fn finalize(&self) {
        drop(self.formatter.replace(None));
    }
}

impl NumberFormatBase {
    pub fn new(vm: &Vm, class: &'static Class, prototype: Gc<Object>) -> NumberFormatBase {
        NumberFormatBase {
            base: Object::new_with_prototype(vm, class, prototype, MayInterfereWithIndexedPropertyAccess::No),
            locale: GcRefCell::default(),
            min_integer_digits: Cell::new(0),
            min_fraction_digits: Cell::new(None),
            max_fraction_digits: Cell::new(None),
            min_significant_digits: Cell::new(None),
            max_significant_digits: Cell::new(None),
            notation: Cell::new(Notation::Standard),
            compact_display: Cell::new(None),
            rounding_type: Cell::new(RoundingType::MorePrecision),
            computed_rounding_priority: Cell::new(ComputedRoundingPriority::Invalid),
            rounding_mode: Cell::new(RoundingMode::HalfExpand),
            rounding_increment: Cell::new(1),
            trailing_zero_display: Cell::new(TrailingZeroDisplay::Auto),
            formatter: GcRefCell::new(None),
        }
    }

    pub fn locale(&self) -> Utf16String {
        self.locale.borrow().clone()
    }

    pub fn set_locale(&self, locale: Utf16String) {
        self.locale.replace(locale);
    }

    pub fn min_integer_digits(&self) -> i32 {
        self.min_integer_digits.get()
    }

    pub fn set_min_integer_digits(&self, min_integer_digits: i32) {
        self.min_integer_digits.set(min_integer_digits);
    }

    pub fn has_min_fraction_digits(&self) -> bool {
        self.min_fraction_digits.get().is_some()
    }

    pub fn min_fraction_digits(&self) -> i32 {
        self.min_fraction_digits
            .get()
            .expect("the minimum fraction digits are set")
    }

    pub fn set_min_fraction_digits(&self, min_fraction_digits: i32) {
        self.min_fraction_digits.set(Some(min_fraction_digits));
    }

    pub fn has_max_fraction_digits(&self) -> bool {
        self.max_fraction_digits.get().is_some()
    }

    pub fn max_fraction_digits(&self) -> i32 {
        self.max_fraction_digits
            .get()
            .expect("the maximum fraction digits are set")
    }

    pub fn set_max_fraction_digits(&self, max_fraction_digits: i32) {
        self.max_fraction_digits.set(Some(max_fraction_digits));
    }

    pub fn has_min_significant_digits(&self) -> bool {
        self.min_significant_digits.get().is_some()
    }

    pub fn min_significant_digits(&self) -> i32 {
        self.min_significant_digits
            .get()
            .expect("the minimum significant digits are set")
    }

    pub fn set_min_significant_digits(&self, min_significant_digits: i32) {
        self.min_significant_digits.set(Some(min_significant_digits));
    }

    pub fn has_max_significant_digits(&self) -> bool {
        self.max_significant_digits.get().is_some()
    }

    pub fn max_significant_digits(&self) -> i32 {
        self.max_significant_digits
            .get()
            .expect("the maximum significant digits are set")
    }

    pub fn set_max_significant_digits(&self, max_significant_digits: i32) {
        self.max_significant_digits.set(Some(max_significant_digits));
    }

    pub fn notation(&self) -> Notation {
        self.notation.get()
    }

    pub fn notation_string(&self) -> &'static str {
        unicode::notation_to_string(self.notation.get())
    }

    pub fn set_notation(&self, notation: Utf16View<'_>) {
        self.notation.set(unicode::notation_from_string(notation));
    }

    pub fn has_compact_display(&self) -> bool {
        self.compact_display.get().is_some()
    }

    pub fn compact_display(&self) -> CompactDisplay {
        self.compact_display.get().expect("the compact display is set")
    }

    pub fn compact_display_string(&self) -> &'static str {
        unicode::compact_display_to_string(self.compact_display())
    }

    pub fn set_compact_display(&self, compact_display: Utf16View<'_>) {
        self.compact_display
            .set(Some(unicode::compact_display_from_string(compact_display)));
    }

    pub fn rounding_type(&self) -> RoundingType {
        self.rounding_type.get()
    }

    pub fn rounding_type_string(&self) -> &'static str {
        unicode::rounding_type_to_string(self.rounding_type.get())
    }

    pub fn set_rounding_type(&self, rounding_type: RoundingType) {
        self.rounding_type.set(rounding_type);
    }

    pub fn computed_rounding_priority(&self) -> ComputedRoundingPriority {
        self.computed_rounding_priority.get()
    }

    pub fn computed_rounding_priority_string(&self) -> &'static str {
        match self.computed_rounding_priority.get() {
            ComputedRoundingPriority::Auto => "auto",
            ComputedRoundingPriority::MorePrecision => "morePrecision",
            ComputedRoundingPriority::LessPrecision => "lessPrecision",
            ComputedRoundingPriority::Invalid => unreachable!("SetNumberFormatDigitOptions computes the priority"),
        }
    }

    pub fn set_computed_rounding_priority(&self, computed_rounding_priority: ComputedRoundingPriority) {
        self.computed_rounding_priority.set(computed_rounding_priority);
    }

    pub fn rounding_mode(&self) -> RoundingMode {
        self.rounding_mode.get()
    }

    pub fn rounding_mode_string(&self) -> &'static str {
        unicode::rounding_mode_to_string(self.rounding_mode.get())
    }

    pub fn set_rounding_mode(&self, rounding_mode: Utf16View<'_>) {
        self.rounding_mode
            .set(unicode::rounding_mode_from_string(rounding_mode));
    }

    pub fn rounding_increment(&self) -> i32 {
        self.rounding_increment.get()
    }

    pub fn set_rounding_increment(&self, rounding_increment: i32) {
        self.rounding_increment.set(rounding_increment);
    }

    pub fn trailing_zero_display(&self) -> TrailingZeroDisplay {
        self.trailing_zero_display.get()
    }

    pub fn trailing_zero_display_string(&self) -> &'static str {
        unicode::trailing_zero_display_to_string(self.trailing_zero_display.get())
    }

    pub fn set_trailing_zero_display(&self, trailing_zero_display: Utf16View<'_>) {
        self.trailing_zero_display
            .set(unicode::trailing_zero_display_from_string(trailing_zero_display));
    }

    pub fn display_options(&self) -> DisplayOptions {
        DisplayOptions {
            notation: self.notation.get(),
            compact_display: self.compact_display.get(),
            ..DisplayOptions::default()
        }
    }

    pub fn rounding_options(&self) -> RoundingOptions {
        RoundingOptions {
            type_: self.rounding_type.get(),
            mode: self.rounding_mode.get(),
            trailing_zero_display: self.trailing_zero_display.get(),
            min_significant_digits: self.min_significant_digits.get(),
            max_significant_digits: self.max_significant_digits.get(),
            min_fraction_digits: self.min_fraction_digits.get(),
            max_fraction_digits: self.max_fraction_digits.get(),
            min_integer_digits: self.min_integer_digits.get(),
            rounding_increment: self.rounding_increment.get(),
        }
    }

    /// Calls `callback` with the ICU number formatter, which must not allocate or call into the VM.
    pub fn with_formatter<R>(&self, callback: impl FnOnce(&unicode::NumberFormat) -> R) -> R {
        let formatter = self.formatter.borrow();
        callback(
            formatter
                .as_ref()
                .expect("the Intl object's constructor creates the ICU number formatter"),
        )
    }

    pub fn set_formatter(&self, formatter: unicode::NumberFormat) {
        self.formatter.replace(Some(formatter));
    }
}

/// 16 NumberFormat Objects, https://tc39.es/ecma402/#numberformat-objects
#[repr(C)]
#[derive(Trace)]
pub struct NumberFormat {
    base: NumberFormatBase,
    #[gc(untraced)]
    numbering_system: GcRefCell<Utf16String>, // [[NumberingSystem]]
    #[gc(untraced)]
    style: Cell<NumberFormatStyle>, // [[Style]]
    #[gc(untraced)]
    currency: GcRefCell<Option<Utf16String>>, // [[Currency]]
    #[gc(untraced)]
    currency_display: Cell<Option<CurrencyDisplay>>, // [[CurrencyDisplay]]
    #[gc(untraced)]
    currency_sign: Cell<Option<CurrencySign>>, // [[CurrencySign]]
    #[gc(untraced)]
    unit: GcRefCell<Option<Utf16String>>, // [[Unit]]
    #[gc(untraced)]
    unit_display: Cell<Option<Style>>, // [[UnitDisplay]]
    #[gc(untraced)]
    use_grouping: Cell<Grouping>, // [[UseGrouping]]
    #[gc(untraced)]
    sign_display: Cell<SignDisplay>, // [[SignDisplay]]
    bound_format: Cell<Option<Gc<NumberFormatFunction>>>, // [[BoundFormat]]
}

define_cell!(NumberFormat, Object, extends: [NumberFormatBase, Object]);

impl Deref for NumberFormat {
    type Target = NumberFormatBase;

    fn deref(&self) -> &NumberFormatBase {
        &self.base
    }
}

impl NumberFormat {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> NumberFormat {
        NumberFormat {
            base: NumberFormatBase::new(vm, Self::CLASS, prototype),
            numbering_system: GcRefCell::default(),
            style: Cell::new(NumberFormatStyle::Decimal),
            currency: GcRefCell::new(None),
            currency_display: Cell::new(None),
            currency_sign: Cell::new(None),
            unit: GcRefCell::new(None),
            unit_display: Cell::new(None),
            use_grouping: Cell::new(Grouping::False),
            sign_display: Cell::new(SignDisplay::Auto),
            bound_format: Cell::new(None),
        }
    }

    pub fn numbering_system(&self) -> Utf16String {
        self.numbering_system.borrow().clone()
    }

    pub fn set_numbering_system(&self, numbering_system: Utf16String) {
        self.numbering_system.replace(numbering_system);
    }

    pub fn style(&self) -> NumberFormatStyle {
        self.style.get()
    }

    pub fn style_string(&self) -> &'static str {
        unicode::number_format_style_to_string(self.style.get())
    }

    pub fn set_style(&self, style: Utf16View<'_>) {
        self.style.set(unicode::number_format_style_from_string(style));
    }

    pub fn has_currency(&self) -> bool {
        self.currency.borrow().is_some()
    }

    pub fn currency(&self) -> Utf16String {
        self.currency.borrow().clone().expect("the currency is set")
    }

    pub fn set_currency(&self, currency: Utf16String) {
        self.currency.replace(Some(currency));
    }

    pub fn has_currency_display(&self) -> bool {
        self.currency_display.get().is_some()
    }

    pub fn currency_display(&self) -> CurrencyDisplay {
        self.currency_display.get().expect("the currency display is set")
    }

    pub fn currency_display_string(&self) -> &'static str {
        unicode::currency_display_to_string(self.currency_display())
    }

    pub fn set_currency_display(&self, currency_display: Utf16View<'_>) {
        self.currency_display
            .set(Some(unicode::currency_display_from_string(currency_display)));
    }

    pub fn has_currency_sign(&self) -> bool {
        self.currency_sign.get().is_some()
    }

    pub fn currency_sign(&self) -> CurrencySign {
        self.currency_sign.get().expect("the currency sign is set")
    }

    pub fn currency_sign_string(&self) -> &'static str {
        unicode::currency_sign_to_string(self.currency_sign())
    }

    pub fn set_currency_sign(&self, currency_sign: Utf16View<'_>) {
        self.currency_sign
            .set(Some(unicode::currency_sign_from_string(currency_sign)));
    }

    pub fn has_unit(&self) -> bool {
        self.unit.borrow().is_some()
    }

    pub fn unit(&self) -> Utf16String {
        self.unit.borrow().clone().expect("the unit is set")
    }

    pub fn set_unit(&self, unit: Utf16String) {
        self.unit.replace(Some(unit));
    }

    pub fn has_unit_display(&self) -> bool {
        self.unit_display.get().is_some()
    }

    pub fn unit_display(&self) -> Style {
        self.unit_display.get().expect("the unit display is set")
    }

    pub fn unit_display_string(&self) -> &'static str {
        unicode_intl::style_to_string(self.unit_display())
    }

    pub fn set_unit_display(&self, unit_display: Utf16View<'_>) {
        self.unit_display
            .set(Some(unicode_intl::style_from_string(unit_display)));
    }

    pub fn use_grouping(&self) -> Grouping {
        self.use_grouping.get()
    }

    pub fn use_grouping_to_value(&self, vm: &Vm) -> Value {
        match self.use_grouping.get() {
            Grouping::Always | Grouping::Auto | Grouping::Min2 => Value::from_string(
                PrimitiveString::create_from_utf8(vm, unicode::grouping_to_string(self.use_grouping.get())),
            ),
            Grouping::False => Value::from_bool(false),
        }
    }

    pub fn set_use_grouping(&self, use_grouping: StringOrBoolean) {
        match use_grouping {
            StringOrBoolean::String(grouping) => self
                .use_grouping
                .set(unicode::grouping_from_string(Utf16View::Ascii(grouping.as_bytes()))),
            StringOrBoolean::Boolean(grouping) => {
                assert!(!grouping);
                self.use_grouping.set(Grouping::False);
            }
        }
    }

    pub fn sign_display(&self) -> SignDisplay {
        self.sign_display.get()
    }

    pub fn sign_display_string(&self) -> &'static str {
        unicode::sign_display_to_string(self.sign_display.get())
    }

    pub fn set_sign_display(&self, sign_display: Utf16View<'_>) {
        self.sign_display.set(unicode::sign_display_from_string(sign_display));
    }

    pub fn bound_format(&self) -> Option<Gc<NumberFormatFunction>> {
        self.bound_format.get()
    }

    pub fn set_bound_format(&self, bound_format: Option<Gc<NumberFormatFunction>>) {
        self.bound_format.set(bound_format);
    }

    pub fn display_options(&self) -> DisplayOptions {
        DisplayOptions {
            style: self.style.get(),
            sign_display: self.sign_display.get(),
            grouping: self.use_grouping.get(),
            currency: self.currency.borrow().clone(),
            currency_display: self.currency_display.get(),
            currency_sign: self.currency_sign.get(),
            unit: self.unit.borrow().clone(),
            unit_display: self.unit_display.get(),
            ..self.base.display_options()
        }
    }
}

impl IntlObject for NumberFormat {
    // 16.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.numberformat-internal-slots
    fn relevant_extension_keys(&self) -> &'static [&'static str] {
        // The value of the [[RelevantExtensionKeys]] internal slot is « "nu" ».
        &["nu"]
    }

    // 16.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.numberformat-internal-slots
    fn resolution_option_descriptors<'vm>(&self, vm: &'vm Vm) -> Vec<ResolutionOptionDescriptor<'vm>> {
        // The value of the [[ResolutionOptionDescriptors]] internal slot is « { [[Key]]: "nu", [[Property]]: "numberingSystem" } ».
        vec![ResolutionOptionDescriptor::string("nu", &vm.names.numberingSystem)]
    }
}

// 16.5.1 CurrencyDigits ( currency ), https://tc39.es/ecma402/#sec-currencydigits
pub fn currency_digits(currency: Utf16View<'_>) -> i32 {
    // 1. If the ISO 4217 currency and funds code list contains currency as an alphabetic code, return the minor
    //    unit value corresponding to the currency from the list; otherwise, return 2.
    if let Some(currency_code) = unicode::get_currency_code(currency) {
        return currency_code.minor_unit.unwrap_or(2);
    }
    2
}

// 16.5.4 PartitionNumberPattern ( numberFormat, x ), https://tc39.es/ecma402/#sec-partitionnumberpattern
pub fn partition_number_pattern(
    number_format: &NumberFormat,
    number: &MathematicalValue,
) -> Vec<NumberFormatPartition> {
    let value = number.to_value();
    number_format.with_formatter(|formatter| formatter.format_to_parts(&value))
}

// 16.5.6 FormatNumeric ( numberFormat, x ), https://tc39.es/ecma402/#sec-formatnumber
pub fn format_numeric(number_format: &NumberFormat, number: &MathematicalValue) -> Utf16String {
    // 1. Let parts be ? PartitionNumberPattern(numberFormat, x).
    // 2. Let result be the empty String.
    // 3. For each Record { [[Type]], [[Value]] } part in parts, do
    //     a. Set result to the string-concatenation of result and part.[[Value]].
    // 4. Return result.
    let value = number.to_value();
    number_format.with_formatter(|formatter| formatter.format(&value))
}

// 16.5.7 FormatNumericToParts ( numberFormat, x ), https://tc39.es/ecma402/#sec-formatnumbertoparts
pub fn format_numeric_to_parts(vm: &Vm, number_format: Gc<NumberFormat>, number: &MathematicalValue) -> Gc<Array> {
    let realm = vm.current_realm().expect("FormatNumericToParts runs in a realm");

    // 1. Let parts be ? PartitionNumberPattern(numberFormat, x).
    let parts = partition_number_pattern(&number_format, number);

    // 2. Let result be ! ArrayCreate(0).
    let result = Array::create(vm, realm, 0, None).must();

    // 3. Let n be 0.
    // 4. For each Record { [[Type]], [[Value]] } part in parts, do
    for (n, part) in parts.into_iter().enumerate() {
        // a. Let O be OrdinaryObjectCreate(%Object.prototype%).
        let object = Object::create(vm, realm, Some(realm.object_prototype()));

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.type_,
                Value::from_string(PrimitiveString::create(vm, part.type_)),
            )
            .must();

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.value,
                Value::from_string(PrimitiveString::create(vm, part.value)),
            )
            .must();

        // d. Perform ! CreateDataPropertyOrThrow(result, ! ToString(n), O).
        result
            .create_data_property_or_throw(vm, &PropertyKey::from_number(n as u64), Value::from_object(object))
            .must();

        // e. Increment n by 1.
    }

    // 5. Return result.
    result
}

// 16.5.16 ToIntlMathematicalValue ( value ), https://tc39.es/ecma402/#sec-tointlmathematicalvalue
pub fn to_intl_mathematical_value(vm: &Vm, value: Value) -> ThrowCompletionOr<MathematicalValue> {
    // 1. Let primValue be ? ToPrimitive(value, number).
    let primitive_value = value.to_primitive(vm, PreferredType::Number)?;

    // 2. If primValue is a BigInt, return ℝ(primValue).
    if primitive_value.is_bigint() {
        return Ok(MathematicalValue::from_string(bigint_to_base_10(primitive_value)));
    }

    // 3. If primValue is a String, then
    //     a. Let str be primValue.
    // 4. Else,
    //     a. Let x be ? ToNumber(primValue).
    //     b. If x is -0𝔽, return negative-zero.
    //     c. Let str be Number::toString(x, 10).
    // NB: Parsing the string form of a Number in the remaining steps yields a value that formats the same as the
    //     Number itself, so the Number is returned as is.
    if !primitive_value.is_string() {
        let number = primitive_value.to_number(vm)?;
        return Ok(MathematicalValue::from_number(number.as_f64()));
    }

    let string = primitive_value.as_string().utf16_string();
    let code_units = string.to_utf16();

    // 5. Let text be StringToCodePoints(str).
    // 6. Let literal be ParseText(text, StringNumericLiteral).
    let literal = parse_string_numeric_literal(&code_units);

    // 7. If literal is a List of errors, return not-a-number.
    // 8. Let intlMV be the StringIntlMV of literal.
    // 9. If intlMV is a mathematical value, then
    //     a. Let rounded be RoundMVResult(abs(intlMV)).
    // NB: StringToNumber performs the parse and the rounding, and its sign carries the sign of intlMV.
    let rounded = string_to_number(&code_units);

    if rounded.is_nan() {
        return Ok(MathematicalValue::from_symbol(MathematicalValueSymbol::NotANumber));
    }

    //     b. If rounded is +∞𝔽 and intlMV < 0, return negative-infinity.
    if rounded == f64::NEG_INFINITY {
        return Ok(MathematicalValue::from_symbol(
            MathematicalValueSymbol::NegativeInfinity,
        ));
    }

    //     c. If rounded is +∞𝔽, return positive-infinity.
    if rounded == f64::INFINITY {
        return Ok(MathematicalValue::from_symbol(
            MathematicalValueSymbol::PositiveInfinity,
        ));
    }

    //     d. If rounded is +0𝔽 and intlMV < 0, return negative-zero.
    if rounded == 0.0 && rounded.is_sign_negative() {
        return Ok(MathematicalValue::from_symbol(MathematicalValueSymbol::NegativeZero));
    }

    //     e. If rounded is +0𝔽, return 0.
    if rounded == 0.0 {
        return Ok(MathematicalValue::from_number(0.0));
    }

    // 10. Return intlMV.
    // NB: The exact value is kept as decimal source text. A non-decimal integer literal is converted to its decimal
    //     digits first.
    let literal = literal.expect("a string that parses to a finite non-zero number is a StringNumericLiteral");
    if literal.base == 10 {
        return Ok(MathematicalValue::from_string(Utf16String::from_utf16(literal.literal)));
    }

    let integer = unsigned_big_integer_from_base(literal.base, literal.literal);
    Ok(MathematicalValue::from_string(Utf16String::from_utf8(
        &integer.to_string(),
    )))
}

/// Throws the RangeError of the first step of PartitionNumberRangePattern if x or y is NaN.
fn throw_if_range_is_nan(vm: &Vm, start: &MathematicalValue, end: &MathematicalValue) -> ThrowCompletionOr<()> {
    // 1. If x is NaN or y is NaN, throw a RangeError exception.
    if start.is_nan() {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::NumberIsNaN, &[&"start"]);
    }
    if end.is_nan() {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::NumberIsNaN, &[&"end"]);
    }
    Ok(())
}

// 16.5.19 PartitionNumberRangePattern ( numberFormat, x, y ), https://tc39.es/ecma402/#sec-partitionnumberrangepattern
pub fn partition_number_range_pattern(
    vm: &Vm,
    number_format: &NumberFormat,
    start: &MathematicalValue,
    end: &MathematicalValue,
) -> ThrowCompletionOr<Vec<NumberFormatPartition>> {
    // 1. If x is NaN or y is NaN, throw a RangeError exception.
    throw_if_range_is_nan(vm, start, end)?;

    let (start, end) = (start.to_value(), end.to_value());
    Ok(number_format.with_formatter(|formatter| formatter.format_range_to_parts(&start, &end)))
}

// 16.5.22 FormatNumericRange ( numberFormat, x, y ), https://tc39.es/ecma402/#sec-formatnumericrange
pub fn format_numeric_range(
    vm: &Vm,
    number_format: &NumberFormat,
    start: &MathematicalValue,
    end: &MathematicalValue,
) -> ThrowCompletionOr<Utf16String> {
    // 1. Let parts be ? PartitionNumberRangePattern(numberFormat, x, y).
    {
        // NOTE: We short-circuit PartitionNumberRangePattern as we do not need individual partitions. But we must still
        //       perform the NaN sanity checks from its first step.

        // 1. If x is NaN or y is NaN, throw a RangeError exception.
        throw_if_range_is_nan(vm, start, end)?;
    }

    // 2. Let result be the empty String.
    // 3. For each part in parts, do
    //     a. Set result to the string-concatenation of result and part.[[Value]].
    // 4. Return result.
    let (start, end) = (start.to_value(), end.to_value());
    Ok(number_format.with_formatter(|formatter| formatter.format_range(&start, &end)))
}

// 16.5.23 FormatNumericRangeToParts ( numberFormat, x, y ), https://tc39.es/ecma402/#sec-formatnumericrangetoparts
pub fn format_numeric_range_to_parts(
    vm: &Vm,
    number_format: Gc<NumberFormat>,
    start: &MathematicalValue,
    end: &MathematicalValue,
) -> ThrowCompletionOr<Gc<Array>> {
    let realm = vm.current_realm().expect("FormatNumericRangeToParts runs in a realm");

    // 1. Let parts be ? PartitionNumberRangePattern(numberFormat, x, y).
    let parts = partition_number_range_pattern(vm, &number_format, start, end)?;

    // 2. Let result be ! ArrayCreate(0).
    let result = Array::create(vm, realm, 0, None).must();

    // 3. Let n be 0.
    // 4. For each Record { [[Type]], [[Value]] } part in parts, do
    for (n, part) in parts.into_iter().enumerate() {
        // a. Let O be OrdinaryObjectCreate(%Object.prototype%).
        let object = Object::create(vm, realm, Some(realm.object_prototype()));

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.type_,
                Value::from_string(PrimitiveString::create(vm, part.type_)),
            )
            .must();

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.value,
                Value::from_string(PrimitiveString::create(vm, part.value)),
            )
            .must();

        // d. Perform ! CreateDataPropertyOrThrow(O, "source", part.[[Source]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.source,
                Value::from_string(PrimitiveString::create(vm, part.source)),
            )
            .must();

        // e. Perform ! CreateDataPropertyOrThrow(result, ! ToString(n), O).
        result
            .create_data_property_or_throw(vm, &PropertyKey::from_number(n as u64), Value::from_object(object))
            .must();

        // f. Increment n by 1.
    }

    // 5. Return result.
    Ok(result)
}
