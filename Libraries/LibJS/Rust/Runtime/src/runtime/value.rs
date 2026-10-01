/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of Libraries/LibJS/Runtime/Value.cpp the runtime implements so far.

use core::ptr::NonNull;

use crate::build_configuration::HEAP_REGION_OFFSET_MASK;
use crate::gc::capi::js_heap_region_base;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::primitive_string::PrimitiveString;
use crate::layout::value::Value;
use crate::runtime::accessor::Accessor;
use crate::runtime::big_int::BigInt;
use crate::runtime::symbol::Symbol;
use libjs_abi::value as nan_box;

/// Makes the whole encoded value live up to this point. The conservative stack scan recognizes a cell by its tagged
/// value or by its address, but not by its bare heap offset, which the compiler could otherwise keep instead of the
/// value while the value is held across an allocation.
#[inline(always)]
fn keep_encoded_value_alive(encoded: u64) {
    // SAFETY: The assembly is empty: it only has the compiler put `encoded` in a register here.
    unsafe { core::arch::asm!("/* {0} */", in(reg) encoded, options(nomem, nostack, preserves_flags)) };
}

impl Value {
    pub const fn from_bool(value: bool) -> Self {
        if value { Self::TRUE } else { Self::FALSE }
    }

    pub const fn from_i32(value: i32) -> Self {
        Self(nan_box::SHIFTED_INT32_TAG | value as u32 as u64)
    }

    /// Like the C++ Value(double): integral doubles that fit in an i32, other than negative zero, are stored as
    /// Int32, and every NaN becomes the canonical NaN.
    pub fn from_f64(value: f64) -> Self {
        let is_negative_zero = value.to_bits() == nan_box::NEGATIVE_ZERO_BITS;
        if value >= f64::from(i32::MIN) && value <= f64::from(i32::MAX) && value.trunc() == value && !is_negative_zero {
            return Self::from_i32(value as i32);
        }
        if value.is_nan() {
            return Self(nan_box::CANON_NAN_BITS);
        }
        Self(value.to_bits())
    }

    pub const fn tag(self) -> u64 {
        self.0 >> nan_box::TAG_SHIFT
    }

    pub const fn is_empty(self) -> bool {
        self.0 == nan_box::EMPTY_VALUE
    }

    pub const fn is_undefined(self) -> bool {
        self.0 == nan_box::UNDEFINED_VALUE
    }

    pub const fn is_null(self) -> bool {
        self.0 == nan_box::NULL_VALUE
    }

    pub const fn is_nullish(self) -> bool {
        (self.tag() & nan_box::IS_NULLISH_EXTRACT_PATTERN) == nan_box::IS_NULLISH_PATTERN
    }

    pub const fn is_boolean(self) -> bool {
        self.tag() == nan_box::BOOLEAN_TAG
    }

    pub const fn is_int32(self) -> bool {
        self.tag() == nan_box::INT32_TAG
    }

    pub const fn is_double(self) -> bool {
        (self.0 & nan_box::CANON_NAN_BITS) != nan_box::CANON_NAN_BITS || self.0 == nan_box::CANON_NAN_BITS
    }

    pub const fn is_number(self) -> bool {
        self.is_double() || self.is_int32()
    }

    pub const fn is_cell(self) -> bool {
        (self.tag() & nan_box::IS_CELL_PATTERN) == nan_box::IS_CELL_PATTERN
    }

    pub const fn is_object(self) -> bool {
        self.tag() == nan_box::OBJECT_TAG
    }

    pub const fn is_string(self) -> bool {
        self.tag() == nan_box::STRING_TAG
    }

    pub const fn is_symbol(self) -> bool {
        self.tag() == nan_box::SYMBOL_TAG
    }

    pub const fn is_bigint(self) -> bool {
        self.tag() == nan_box::BIGINT_TAG
    }

    pub const fn is_accessor(self) -> bool {
        self.tag() == nan_box::ACCESSOR_TAG
    }

    pub fn as_bool(self) -> bool {
        assert!(self.is_boolean());
        self.0 & 1 != 0
    }

    pub fn as_i32(self) -> i32 {
        debug_assert!(self.is_int32());
        self.0 as u32 as i32
    }

    /// The numeric value of a number.
    pub fn as_f64(self) -> f64 {
        debug_assert!(self.is_number());
        if self.is_int32() {
            return f64::from(self.as_i32());
        }
        f64::from_bits(self.0)
    }

    pub fn is_integral_number(self) -> bool {
        if self.is_int32() {
            return true;
        }
        self.is_finite_number() && self.as_f64().trunc() == self.as_f64()
    }

    pub fn is_finite_number(self) -> bool {
        if !self.is_number() {
            return false;
        }
        if self.is_int32() {
            return true;
        }
        self.as_f64().is_finite()
    }

    fn with_cell_tag<T>(tag: u64, cell: Gc<T>) -> Self {
        let address = cell.as_ptr() as usize as u64;
        Self((tag << nan_box::TAG_SHIFT) | (address & HEAP_REGION_OFFSET_MASK))
    }

    /// # Safety
    ///
    /// The value must hold a cell of type T.
    unsafe fn cell<T>(self) -> Gc<T> {
        debug_assert!(self.is_cell());
        keep_encoded_value_alive(self.0);
        // SAFETY: Cell values are offsets into the heap region, whose base LibGC fixed before any cell existed.
        let base = unsafe { js_heap_region_base } as u64;
        let address = (base + (self.0 & HEAP_REGION_OFFSET_MASK)) as usize;
        // SAFETY: The caller guarantees the value holds a live cell of type T.
        unsafe { Gc::from_non_null(NonNull::new_unchecked(core::ptr::without_provenance_mut::<T>(address))) }
    }

    pub fn from_object<T>(object: Gc<T>) -> Self {
        Self::with_cell_tag(nan_box::OBJECT_TAG, object)
    }

    pub fn from_string(string: Gc<PrimitiveString>) -> Self {
        Self::with_cell_tag(nan_box::STRING_TAG, string)
    }

    pub fn from_symbol(symbol: Gc<Symbol>) -> Self {
        Self::with_cell_tag(nan_box::SYMBOL_TAG, symbol)
    }

    pub fn from_bigint(bigint: Gc<BigInt>) -> Self {
        Self::with_cell_tag(nan_box::BIGINT_TAG, bigint)
    }

    pub fn from_accessor(accessor: Gc<Accessor>) -> Self {
        Self::with_cell_tag(nan_box::ACCESSOR_TAG, accessor)
    }

    pub fn as_object(self) -> Gc<Object> {
        assert!(self.is_object());
        // SAFETY: The tag says the value holds an object.
        unsafe { self.cell() }
    }

    pub fn as_string(self) -> Gc<PrimitiveString> {
        assert!(self.is_string());
        // SAFETY: The tag says the value holds a string.
        unsafe { self.cell() }
    }

    pub fn as_symbol(self) -> Gc<Symbol> {
        assert!(self.is_symbol());
        // SAFETY: The tag says the value holds a symbol.
        unsafe { self.cell() }
    }

    pub fn as_bigint(self) -> Gc<BigInt> {
        assert!(self.is_bigint());
        // SAFETY: The tag says the value holds a BigInt.
        unsafe { self.cell() }
    }

    pub fn as_accessor(self) -> Gc<Accessor> {
        assert!(self.is_accessor());
        // SAFETY: The tag says the value holds an accessor.
        unsafe { self.cell() }
    }
}

impl core::fmt::Debug for Value {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(formatter, "Value(0x{:016x})", self.0)
    }
}

/// The text and UTF-16 builders Number::toString writes its ASCII output into.
pub trait NumberStringBuilder {
    fn append_ascii(&mut self, text: &[u8]);
    fn append_repeated_ascii(&mut self, code_unit: u8, count: usize);
}

impl NumberStringBuilder for String {
    fn append_ascii(&mut self, text: &[u8]) {
        self.extend(text.iter().copied().map(char::from));
    }

    fn append_repeated_ascii(&mut self, code_unit: u8, count: usize) {
        self.extend(core::iter::repeat_n(char::from(code_unit), count));
    }
}

impl NumberStringBuilder for Vec<u16> {
    fn append_ascii(&mut self, text: &[u8]) {
        self.extend(text.iter().copied().map(u16::from));
    }

    fn append_repeated_ascii(&mut self, code_unit: u8, count: usize) {
        self.extend(core::iter::repeat_n(u16::from(code_unit), count));
    }
}

pub fn number_to_string(value: f64) -> String {
    let mut builder = String::new();
    append_number_to_string(&mut builder, value);
    builder
}

pub fn number_to_utf16_string(value: f64) -> Vec<u16> {
    let mut builder = Vec::new();
    append_number_to_string(&mut builder, value);
    builder
}

// 6.1.6.1.20 Number::toString ( x ), https://tc39.es/ecma262/#sec-numeric-types-number-tostring
// Implementation for radix = 10
pub fn append_number_to_string(builder: &mut impl NumberStringBuilder, value: f64) {
    // 1. If x is NaN, return "NaN".
    if value.is_nan() {
        builder.append_ascii(b"NaN");
        return;
    }

    // 2. If x is +0𝔽 or -0𝔽, return "0".
    if value == 0.0 {
        builder.append_ascii(b"0");
        return;
    }

    // 4. If x is +∞𝔽, return "Infinity".
    if value.is_infinite() {
        builder.append_ascii(if value > 0.0 { b"Infinity" } else { b"-Infinity" });
        return;
    }

    // 5. Let n, k, and s be integers such that k ≥ 1, radix ^ (k - 1) ≤ s < radix ^ k, 𝔽(s × radix ^ (n - k)) is x,
    //    and k is as small as possible.
    let ak::DecimalExponentialForm {
        sign: is_negative,
        fraction: significand,
        exponent,
    } = ak::convert_to_decimal_exponential_form(value);
    let significand_digits = DecimalDigits::new(significand);
    let digits = significand_digits.as_bytes();
    let k = digits.len() as i32;
    let n = exponent + k;

    // 3. If x < -0𝔽, return the string-concatenation of "-" and Number::toString(-x, radix).
    if is_negative {
        builder.append_ascii(b"-");
    }

    // 6. If radix ≠ 10 or n is in the inclusive interval from -5 to 21, then
    if (-5..=21).contains(&n) {
        if n >= k {
            // a. If n ≥ k, return the k digits of s followed by n - k zeros.
            builder.append_ascii(digits);
            builder.append_repeated_ascii(b'0', (n - k) as usize);
        } else if n > 0 {
            // b. Else if n > 0, return the most significant n digits of s, ".", and the remaining k - n digits.
            builder.append_ascii(&digits[..n as usize]);
            builder.append_ascii(b".");
            builder.append_ascii(&digits[n as usize..]);
        } else {
            // c. Else, return "0.", -n zeros, and the k digits of s.
            builder.append_ascii(b"0.");
            builder.append_repeated_ascii(b'0', n.unsigned_abs() as usize);
            builder.append_ascii(digits);
        }
        return;
    }

    // 7. NOTE: In this case, the input will be represented using scientific E notation, such as 1.2e+3.
    // 9. If n < 0, let exponentSign be "-". 10. Else, let exponentSign be "+".
    let exponent_sign: &[u8] = if n < 0 { b"-" } else { b"+" };
    let exponent_digits = DecimalDigits::new(u64::from((n - 1).unsigned_abs()));

    // 11. If k is 1, return the single digit of s, "e", exponentSign, and the decimal representation of abs(n - 1).
    // 12. Return the most significant digit of s, ".", the remaining k - 1 digits, "e", exponentSign, and abs(n - 1).
    builder.append_ascii(&digits[..1]);
    if k > 1 {
        builder.append_ascii(b".");
        builder.append_ascii(&digits[1..]);
    }
    builder.append_ascii(b"e");
    builder.append_ascii(exponent_sign);
    builder.append_ascii(exponent_digits.as_bytes());
}

struct DecimalDigits {
    digits: [u8; 20],
    start: usize,
}

impl DecimalDigits {
    fn new(mut value: u64) -> Self {
        let mut digits = [0; 20];
        let mut start = digits.len();
        loop {
            start -= 1;
            digits[start] = b'0' + (value % 10) as u8;
            value /= 10;
            if value == 0 {
                break;
            }
        }
        Self { digits, start }
    }

    fn as_bytes(&self) -> &[u8] {
        &self.digits[self.start..]
    }
}
