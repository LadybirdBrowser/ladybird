/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::fmt;
use core::hash::{Hash, Hasher};
use core::num::NonZeroUsize;
use core::ptr::NonNull;

use ak::{Utf16FlyString, Utf16String};

use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::symbol::Symbol;
use crate::utf16::{Utf16View, to_utf16_fly_string};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StringMayBeNumber {
    Yes,
    No,
}

/// The key of a property: one tagged word with the same bits as JS::PropertyKey. The low two bits say what it holds:
/// a fly string's raw word, which is either the address of its data or a short string, a symbol's address, or an
/// array index shifted left by two. Array-index strings are always stored as numbers, and fly strings are interned,
/// so two keys are equal exactly when their words are.
#[repr(transparent)]
pub struct PropertyKey(NonZeroUsize);

const _: () = assert!(size_of::<PropertyKey>() == size_of::<usize>());
const _: () = assert!(size_of::<Option<PropertyKey>>() == size_of::<usize>());
const _: () = assert!(size_of::<Utf16FlyString>() == size_of::<usize>());

impl PropertyKey {
    pub const NORMAL_STRING_FLAG: usize = 0;
    pub const SHORT_STRING_FLAG: usize = 1;
    pub const SYMBOL_FLAG: usize = 2;
    pub const NUMBER_FLAG: usize = 3;
    const FLAG_MASK: usize = 3;

    pub fn from_value(vm: &Vm, value: Value) -> ThrowCompletionOr<PropertyKey> {
        assert!(!value.is_empty());
        if value.is_symbol() {
            return Ok(PropertyKey::from_symbol(value.as_symbol()));
        }
        if value.is_integral_number() && value.as_f64() >= 0.0 && value.as_f64() < f64::from(u32::MAX) {
            return Ok(PropertyKey::from_number(value.as_f64() as u64));
        }
        Ok(PropertyKey::from_utf16_string(&value_to_utf16_string(vm, value)?))
    }

    pub fn is_string(&self) -> bool {
        let flag = self.flag();
        flag == Self::NORMAL_STRING_FLAG || flag == Self::SHORT_STRING_FLAG
    }

    pub fn is_number(&self) -> bool {
        self.flag() == Self::NUMBER_FLAG
    }

    pub fn is_symbol(&self) -> bool {
        self.flag() == Self::SYMBOL_FLAG
    }

    pub fn is_private(&self) -> bool {
        self.is_symbol() && self.as_symbol().is_private()
    }

    /// Mirrors the PropertyKey constructor from an integer: indices below u32::MAX are numbers, and larger ones are
    /// strings, since they cannot be array indices.
    pub fn from_number(index: u64) -> Self {
        if index >= u64::from(u32::MAX) {
            return Self::from_fly_string_without_number_check(Utf16FlyString::from_utf8(&index.to_string()));
        }
        Self::from_array_index(index as u32)
    }

    pub fn from_fly_string(string: Utf16FlyString, string_may_be_number: StringMayBeNumber) -> Self {
        if string_may_be_number == StringMayBeNumber::Yes
            && let Some(property_index) = array_index_of_canonical_string(Utf16View::of_fly_string(&string))
        {
            return Self::from_array_index(property_index);
        }

        Self::from_fly_string_without_number_check(string)
    }

    pub fn from_utf16_string(string: &Utf16String) -> Self {
        Self::from(to_utf16_fly_string(string))
    }

    pub fn from_utf8(string: &str) -> Self {
        Self::from(Utf16FlyString::from_utf8(string))
    }

    pub fn from_symbol(symbol: Gc<Symbol>) -> Self {
        let address = symbol.as_ptr().expose_provenance();
        debug_assert!(address & Self::FLAG_MASK == 0);
        Self(NonZeroUsize::new(address | Self::SYMBOL_FLAG).expect("a tagged symbol address is not zero"))
    }

    fn from_array_index(index: u32) -> Self {
        Self(NonZeroUsize::new((index as usize) << 2 | Self::NUMBER_FLAG).expect("a tagged number is not zero"))
    }

    fn from_fly_string_without_number_check(string: Utf16FlyString) -> Self {
        let raw = string.into_raw();
        debug_assert!(
            raw & Self::FLAG_MASK == Self::NORMAL_STRING_FLAG || raw & Self::FLAG_MASK == Self::SHORT_STRING_FLAG
        );
        Self(NonZeroUsize::new(raw).expect("a raw AK string is never zero"))
    }

    fn flag(&self) -> usize {
        self.0.get() & Self::FLAG_MASK
    }

    pub fn as_number(&self) -> u32 {
        assert!(self.is_number());
        (self.0.get() >> 2) as u32
    }

    pub fn as_string(&self) -> &Utf16FlyString {
        assert!(self.is_string());
        // SAFETY: A string key's word is the raw word of the fly string it owns, and Utf16FlyString is a transparent
        // wrapper around that word.
        unsafe { &*core::ptr::from_ref(&self.0).cast::<Utf16FlyString>() }
    }

    pub fn as_symbol(&self) -> Gc<Symbol> {
        assert!(self.is_symbol());
        let address = self.0.get() & !Self::FLAG_MASK;
        // SAFETY: A symbol key holds the address of its symbol, which it keeps alive.
        unsafe {
            Gc::from_non_null(NonNull::new_unchecked(
                core::ptr::with_exposed_provenance_mut::<Symbol>(address),
            ))
        }
    }

    pub fn to_value(&self, vm: &Vm) -> Value {
        assert!(!self.is_private());
        if self.is_string() {
            return Value::from_string(PrimitiveString::create_from_fly_string(vm, self.as_string()));
        }
        if self.is_symbol() {
            return Value::from_symbol(self.as_symbol());
        }
        Value::from_string(PrimitiveString::create_from_unsigned_integer(
            vm,
            u64::from(self.as_number()),
        ))
    }

    pub fn to_utf16_string(&self) -> Utf16String {
        if self.is_string() {
            return Utf16String::from(self.as_string());
        }
        if self.is_symbol() {
            return self.as_symbol().descriptive_string();
        }
        Utf16String::from_utf8(&self.as_number().to_string())
    }
}

/// The index an array-index string stands for, if it is one: the canonical decimal form of an integer below
/// u32::MAX, the same strings that the C++ PropertyKey stores as numbers.
fn array_index_of_canonical_string(string: Utf16View<'_>) -> Option<u32> {
    if string.is_empty() {
        return None;
    }
    let first_code_unit = string.code_unit_at(0);
    let is_ascii_digit = |code_unit: u16| (u16::from(b'0')..=u16::from(b'9')).contains(&code_unit);
    if !is_ascii_digit(first_code_unit) || (first_code_unit == u16::from(b'0') && string.length_in_code_units() > 1) {
        return None;
    }
    let mut property_index: u32 = 0;
    for code_unit in string.code_units() {
        if !is_ascii_digit(code_unit) {
            return None;
        }
        property_index = property_index
            .checked_mul(10)?
            .checked_add(u32::from(code_unit - u16::from(b'0')))?;
    }
    (property_index < u32::MAX).then_some(property_index)
}

fn value_to_utf16_string(_vm: &Vm, value: Value) -> ThrowCompletionOr<Utf16String> {
    if value.is_string() {
        return Ok(value.as_string().utf16_string());
    }
    unimplemented_runtime_function("PropertyKey::from_value: Value::to_utf16_string of a non-string", 0)
}

impl From<u32> for PropertyKey {
    fn from(index: u32) -> Self {
        Self::from_number(u64::from(index))
    }
}

impl From<Utf16FlyString> for PropertyKey {
    fn from(string: Utf16FlyString) -> Self {
        Self::from_fly_string(string, StringMayBeNumber::Yes)
    }
}

impl From<&Utf16String> for PropertyKey {
    fn from(string: &Utf16String) -> Self {
        Self::from_utf16_string(string)
    }
}

impl From<Gc<Symbol>> for PropertyKey {
    fn from(symbol: Gc<Symbol>) -> Self {
        Self::from_symbol(symbol)
    }
}

impl Clone for PropertyKey {
    fn clone(&self) -> Self {
        if self.is_string() {
            // SAFETY: The key owns a reference to its fly string, so the string is live; the clone owns the new one.
            unsafe { ak::reference_utf16_string(self.0.get()) };
        }
        Self(self.0)
    }
}

impl Drop for PropertyKey {
    fn drop(&mut self) {
        if self.is_string() {
            // SAFETY: The key owns one reference to its fly string, which this releases.
            drop(unsafe { Utf16FlyString::from_raw_owned(self.0.get()) });
        }
    }
}

impl PartialEq for PropertyKey {
    fn eq(&self, other: &Self) -> bool {
        self.0 == other.0
    }
}

impl Eq for PropertyKey {}

impl Hash for PropertyKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.0.hash(state);
    }
}

// SAFETY: A key reaches a cell only when it holds a symbol, which this visits.
unsafe impl Trace for PropertyKey {
    fn trace(&self, visitor: &mut Visitor) {
        if self.is_symbol() {
            visitor.visit(self.as_symbol());
        }
    }
}

impl fmt::Display for PropertyKey {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.is_number() {
            return write!(formatter, "{}", self.as_number());
        }
        let string = self.to_utf16_string();
        write!(formatter, "{}", Utf16View::of_string(&string).to_utf8())
    }
}

impl fmt::Debug for PropertyKey {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.is_string() {
            return write!(
                formatter,
                "PropertyKey({:?})",
                Utf16View::of_fly_string(self.as_string()).to_utf8()
            );
        }
        write!(formatter, "PropertyKey({self})")
    }
}
