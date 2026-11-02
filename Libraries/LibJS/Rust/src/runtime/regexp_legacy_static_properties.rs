/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::regexp_constructor::RegExpConstructor;
use crate::runtime::value::same_value;

const PAREN_COUNT: usize = 9;

/// The captured ranges of a match that the legacy static properties record: those of $1 through $9, and that of the
/// last capture group, which can be past $9.
pub struct LegacyCaptureSlots {
    starts: [i32; PAREN_COUNT],
    ends: [i32; PAREN_COUNT],
    last_paren: (i32, i32),
}

impl LegacyCaptureSlots {
    /// `slot(2 * group)` and `slot(2 * group + 1)` are the start and end of each of the `group_count` capture groups, or
    /// -1 for a group that did not participate.
    pub fn new(group_count: usize, slot: impl Fn(usize) -> i32) -> Self {
        let mut captures = Self {
            starts: [-1; PAREN_COUNT],
            ends: [-1; PAREN_COUNT],
            last_paren: (-1, -1),
        };
        for index in 0..group_count.min(PAREN_COUNT) {
            captures.starts[index] = slot((index + 1) * 2);
            captures.ends[index] = slot((index + 1) * 2 + 1);
        }
        if group_count > 0 {
            captures.last_paren = (slot(group_count * 2), slot(group_count * 2 + 1));
        }
        captures
    }
}

// https://github.com/tc39/proposal-regexp-legacy-features#regexp
// The %RegExp% intrinsic object, which is the builtin RegExp constructor, has the following additional internal slots:
// [[RegExpInput]]
// [[RegExpLastMatch]]
// [[RegExpLastParen]]
// [[RegExpLeftContext]]
// [[RegExpRightContext]]
// [[RegExpParen1]] ... [[RegExpParen9]]
#[derive(Trace)]
pub struct RegExpLegacyStaticProperties {
    input: Cell<Option<Gc<PrimitiveString>>>,
    match_source: Cell<Option<Gc<PrimitiveString>>>,
    last_paren: Cell<Option<Gc<PrimitiveString>>>,
    parens: [Cell<Option<Gc<PrimitiveString>>>; PAREN_COUNT],

    #[gc(untraced)]
    last_match_start: Cell<usize>,
    #[gc(untraced)]
    last_match_length: Cell<usize>,
    #[gc(untraced)]
    left_context_start: Cell<usize>,
    #[gc(untraced)]
    left_context_length: Cell<usize>,
    #[gc(untraced)]
    right_context_start: Cell<usize>,
    #[gc(untraced)]
    right_context_length: Cell<usize>,
    last_match_string: Cell<Option<Gc<PrimitiveString>>>,
    left_context_string: Cell<Option<Gc<PrimitiveString>>>,
    right_context_string: Cell<Option<Gc<PrimitiveString>>>,
    #[gc(untraced)]
    last_paren_start: Cell<i32>,
    #[gc(untraced)]
    last_paren_end: Cell<i32>,

    // Lazy capture storage: indices into match_source.
    // -1 means not captured. Strings materialized on access.
    #[gc(untraced)]
    paren_starts: [Cell<i32>; PAREN_COUNT],
    #[gc(untraced)]
    paren_ends: [Cell<i32>; PAREN_COUNT],
    #[gc(untraced)]
    parens_materialized: Cell<bool>,
}

/// One of the getters of RegExpLegacyStaticProperties.
pub type LegacyStaticPropertyGetter = fn(&RegExpLegacyStaticProperties, &Vm) -> Option<Gc<PrimitiveString>>;

/// One of the setters of RegExpLegacyStaticProperties.
pub type LegacyStaticPropertySetter = fn(&RegExpLegacyStaticProperties, Gc<PrimitiveString>);

impl Default for RegExpLegacyStaticProperties {
    fn default() -> Self {
        Self {
            input: Cell::new(None),
            match_source: Cell::new(None),
            last_paren: Cell::new(None),
            parens: Default::default(),
            last_match_start: Cell::new(0),
            last_match_length: Cell::new(0),
            left_context_start: Cell::new(0),
            left_context_length: Cell::new(0),
            right_context_start: Cell::new(0),
            right_context_length: Cell::new(0),
            last_match_string: Cell::new(None),
            left_context_string: Cell::new(None),
            right_context_string: Cell::new(None),
            last_paren_start: Cell::new(-1),
            last_paren_end: Cell::new(-1),
            paren_starts: core::array::from_fn(|_| Cell::new(-1)),
            paren_ends: core::array::from_fn(|_| Cell::new(-1)),
            parens_materialized: Cell::new(true),
        }
    }
}

impl RegExpLegacyStaticProperties {
    pub fn input(&self, _vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.input.get()
    }

    pub fn dollar_1(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.lazy_paren(vm, 0)
    }

    pub fn dollar_2(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.lazy_paren(vm, 1)
    }

    pub fn dollar_3(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.lazy_paren(vm, 2)
    }

    pub fn dollar_4(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.lazy_paren(vm, 3)
    }

    pub fn dollar_5(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.lazy_paren(vm, 4)
    }

    pub fn dollar_6(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.lazy_paren(vm, 5)
    }

    pub fn dollar_7(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.lazy_paren(vm, 6)
    }

    pub fn dollar_8(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.lazy_paren(vm, 7)
    }

    pub fn dollar_9(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.lazy_paren(vm, 8)
    }

    pub fn set_input(&self, input: Gc<PrimitiveString>) {
        self.input.set(Some(input));
    }

    pub fn invalidate(&self) {
        self.input.set(None);
        self.match_source.set(None);
        self.last_match_start.set(0);
        self.last_match_length.set(0);
        self.last_match_string.set(None);
        self.last_paren.set(None);
        self.last_paren_start.set(-1);
        self.last_paren_end.set(-1);
        self.left_context_start.set(0);
        self.left_context_length.set(0);
        self.left_context_string.set(None);
        self.right_context_start.set(0);
        self.right_context_length.set(0);
        self.right_context_string.set(None);
        for paren in &self.parens {
            paren.set(None);
        }
        for start in &self.paren_starts {
            start.set(-1);
        }
        for end in &self.paren_ends {
            end.set(-1);
        }
        self.parens_materialized.set(true);
    }

    pub fn set_match_source(&self, match_source: Gc<PrimitiveString>) {
        self.match_source.set(Some(match_source));
        self.last_match_string.set(None);
        self.last_paren.set(None);
        self.left_context_string.set(None);
        self.right_context_string.set(None);
        for paren in &self.parens {
            paren.set(None);
        }
    }

    pub fn set_last_match(&self, start: usize, length: usize) {
        self.last_match_start.set(start);
        self.last_match_length.set(length);
        self.last_match_string.set(None);
    }

    pub fn set_left_context(&self, start: usize, length: usize) {
        self.left_context_start.set(start);
        self.left_context_length.set(length);
        self.left_context_string.set(None);
    }

    pub fn set_right_context(&self, start: usize, length: usize) {
        self.right_context_start.set(start);
        self.right_context_length.set(length);
        self.right_context_string.set(None);
    }

    fn empty_string(&self, vm: &Vm) -> Gc<PrimitiveString> {
        assert!(self.match_source.get().is_some());
        vm.empty_string()
    }

    fn substring_of_match_source(&self, vm: &Vm, start: usize, length: usize) -> Gc<PrimitiveString> {
        let match_source = self
            .match_source
            .get()
            .expect("there is a match source to take a substring of");
        PrimitiveString::create_from_substring(vm, match_source, start, length)
    }

    pub fn last_match(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.match_source.get()?;
        if self.last_match_string.get().is_none() {
            let last_match =
                self.substring_of_match_source(vm, self.last_match_start.get(), self.last_match_length.get());
            self.last_match_string.set(Some(last_match));
        }
        self.last_match_string.get()
    }

    pub fn last_paren(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.match_source.get()?;
        if self.last_paren.get().is_none() {
            let (start, end) = (self.last_paren_start.get(), self.last_paren_end.get());
            let last_paren = if start >= 0 && end >= 0 {
                self.substring_of_match_source(vm, start as usize, (end - start) as usize)
            } else {
                self.empty_string(vm)
            };
            self.last_paren.set(Some(last_paren));
        }
        self.last_paren.get()
    }

    pub fn left_context(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.match_source.get()?;
        if self.left_context_string.get().is_none() {
            let left_context =
                self.substring_of_match_source(vm, self.left_context_start.get(), self.left_context_length.get());
            self.left_context_string.set(Some(left_context));
        }
        self.left_context_string.get()
    }

    pub fn right_context(&self, vm: &Vm) -> Option<Gc<PrimitiveString>> {
        self.match_source.get()?;
        if self.right_context_string.get().is_none() {
            let right_context =
                self.substring_of_match_source(vm, self.right_context_start.get(), self.right_context_length.get());
            self.right_context_string.set(Some(right_context));
        }
        self.right_context_string.get()
    }

    // Lazy update: store capture indices without materializing strings.
    // Strings are created on demand when $1-$9 are accessed.
    pub fn set_captures_lazy(&self, vm: &Vm, captures: &LegacyCaptureSlots) {
        for index in 0..PAREN_COUNT {
            self.paren_starts[index].set(captures.starts[index]);
            self.paren_ends[index].set(captures.ends[index]);
        }
        // Clear any previously materialized strings.
        for paren in &self.parens {
            paren.set(None);
        }
        self.parens_materialized.set(false);

        // Set last_paren to the last captured value.
        let (last_paren_start, last_paren_end) = captures.last_paren;
        if last_paren_start >= 0 && last_paren_end >= 0 {
            self.last_paren.set(None);
            self.last_paren_start.set(last_paren_start);
            self.last_paren_end.set(last_paren_end);
        } else {
            self.last_paren.set(Some(self.empty_string(vm)));
            self.last_paren_start.set(-1);
            self.last_paren_end.set(-1);
        }
    }

    fn lazy_paren(&self, vm: &Vm, index: usize) -> Option<Gc<PrimitiveString>> {
        assert!(index < PAREN_COUNT);
        self.match_source.get()?;
        if !self.parens_materialized.get() {
            // Materialize all lazy parens from stored indices.
            for paren_index in 0..PAREN_COUNT {
                let (start, end) = (self.paren_starts[paren_index].get(), self.paren_ends[paren_index].get());
                let paren = if start >= 0 && end >= 0 {
                    self.substring_of_match_source(vm, start as usize, (end - start) as usize)
                } else {
                    self.empty_string(vm)
                };
                self.parens[paren_index].set(Some(paren));
            }
            self.parens_materialized.set(true);
        }
        self.parens[index].get()
    }
}

// GetLegacyRegExpStaticProperty( C, thisValue, internalSlotName ), https://github.com/tc39/proposal-regexp-legacy-features#getlegacyregexpstaticproperty-c-thisvalue-internalslotname-
pub fn get_legacy_regexp_static_property(
    vm: &Vm,
    constructor: Gc<RegExpConstructor>,
    this_value: Value,
    property_getter: LegacyStaticPropertyGetter,
) -> ThrowCompletionOr<Value> {
    // 1. Assert C is an object that has an internal slot named internalSlotName.

    // 2. If SameValue(C, thisValue) is false, throw a TypeError exception.
    if !same_value(Value::from_object(constructor), this_value) {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::GetLegacyRegExpStaticPropertyThisValueMismatch,
            &[],
        );
    }

    // 3. Let val be the value of the internal slot of C named internalSlotName.
    let value = property_getter(constructor.legacy_static_properties(), vm);

    // 4. If val is empty, throw a TypeError exception.
    // NOTE: The spec says to throw here, but all major browsers return "" instead.
    let Some(value) = value else {
        return Ok(Value::from_string(vm.empty_string()));
    };

    // 5. Return val.
    Ok(Value::from_string(value))
}

// SetLegacyRegExpStaticProperty( C, thisValue, internalSlotName, val ), https://github.com/tc39/proposal-regexp-legacy-features#setlegacyregexpstaticproperty-c-thisvalue-internalslotname-val-
pub fn set_legacy_regexp_static_property(
    vm: &Vm,
    constructor: Gc<RegExpConstructor>,
    this_value: Value,
    property_setter: LegacyStaticPropertySetter,
    value: Value,
) -> ThrowCompletionOr<()> {
    // 1. Assert C is an object that has an internal slot named internalSlotName.

    // 2. If SameValue(C, thisValue) is false, throw a TypeError exception.
    if !same_value(Value::from_object(constructor), this_value) {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::SetLegacyRegExpStaticPropertyThisValueMismatch,
            &[],
        );
    }

    // 3. Let strVal be ? ToString(val).
    let string_value = value.to_utf16_string(vm)?;

    // 4. Set the value of the internal slot of C named internalSlotName to strVal.
    property_setter(
        constructor.legacy_static_properties(),
        PrimitiveString::create(vm, string_value),
    );

    Ok(())
}

// UpdateLegacyRegExpStaticProperties ( C, S, startIndex, endIndex, capturedValues ), https://github.com/tc39/proposal-regexp-legacy-features#updatelegacyregexpstaticproperties--c-s-startindex-endindex-capturedvalues-

// Like update_legacy_regexp_static_properties, but defers $1-$9 string creation.
// Captures are stored as index pairs into the input string and materialized on access.
pub fn update_legacy_regexp_static_properties_lazy(
    vm: &Vm,
    constructor: Gc<RegExpConstructor>,
    string: Gc<PrimitiveString>,
    start_index: usize,
    end_index: usize,
    captures: &LegacyCaptureSlots,
) {
    let legacy_static_properties = constructor.legacy_static_properties();

    let length = string.length_in_utf16_code_units();
    assert!(start_index <= end_index);
    assert!(end_index <= length);

    legacy_static_properties.set_input(string);
    legacy_static_properties.set_match_source(string);

    legacy_static_properties.set_last_match(start_index, end_index - start_index);

    legacy_static_properties.set_left_context(0, start_index);

    legacy_static_properties.set_right_context(end_index, length - end_index);

    legacy_static_properties.set_captures_lazy(vm, captures);
}

// InvalidateLegacyRegExpStaticProperties ( C ), https://github.com/tc39/proposal-regexp-legacy-features#invalidatelegacyregexpstaticproperties--c
pub fn invalidate_legacy_regexp_static_properties(constructor: Gc<RegExpConstructor>) {
    // 1. Assert: C is an Object that has a [[RegExpInput]] internal slot.

    // 2. Set the value of the following internal slots of C to empty:
    constructor.legacy_static_properties().invalidate();
}
