/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, UnsafeCell};

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use crate::gc::class::{Class, ExternalMemorySize, GcCell, define_cell};
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::{StringToAtomCacheEntry, Vm};
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::primitive_string::{DEFERRED_KIND_MASK, INTERNED_FLAG};
pub use crate::layout::primitive_string::{DeferredKind, INLINE_STRING_CAPACITY, InlineString, PrimitiveString};
use crate::layout::value::Value;
use crate::layout_forward::Utf16StringSlot;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::value::DecimalDigits;
use crate::utf16::{
    Utf16Display, Utf16StringBuilder, Utf16View, concatenate, concatenate_short_ascii_strings, has_fly_string_storage,
    has_short_ascii_storage, to_utf16_fly_string, utf16_string_external_memory_size,
};

define_cell!(PrimitiveString, PrimitiveString, external_memory_size: external_memory_size);
define_cell!(RopeString, PrimitiveString, extends: [PrimitiveString]);
define_cell!(Substring, PrimitiveString, extends: [PrimitiveString]);
define_cell!(InlineString, PrimitiveString, extends: [PrimitiveString]);

// PrimitiveString::external_memory_size(): the storage of the string once it has one.
impl ExternalMemorySize for PrimitiveString {
    fn external_memory_size(&self) -> usize {
        self.resolved_utf16_string()
            .map_or(0, utf16_string_external_memory_size)
    }
}

// SAFETY: A string's own data holds no cells. The deferred kinds trace the strings they are made of.
unsafe impl Trace for PrimitiveString {
    fn trace(&self, _: &mut Visitor) {}
}

#[repr(C)]
#[derive(Trace)]
pub struct RopeString {
    base: PrimitiveString,
    lhs: Cell<Option<Gc<PrimitiveString>>>,
    rhs: Cell<Option<Gc<PrimitiveString>>>,
}

#[repr(C)]
#[derive(Trace)]
pub struct Substring {
    base: PrimitiveString,
    source_string: Cell<Option<Gc<PrimitiveString>>>,
    code_unit_offset: usize,
}

// SAFETY: The characters hold no cells.
unsafe impl Trace for InlineString {
    fn trace(&self, _: &mut Visitor) {}
}

/// Mirrors AK::u64_hash, the MurmurHash3 64-bit finalizer.
pub(crate) fn u64_hash(mut key: u64) -> u32 {
    key ^= key >> 33;
    key = key.wrapping_mul(0xff51_afd7_ed55_8ccd);
    key ^= key >> 33;
    key = key.wrapping_mul(0xc4ce_b9fe_1a85_ec53);
    key ^= key >> 33;
    key as u32
}

fn is_ascii(code_unit: u16) -> bool {
    code_unit < 0x80
}

impl PrimitiveString {
    fn fly_string_cache_hash(string: &Utf16FlyString) -> usize {
        u64_hash(string.raw_identity() as u64) as usize
    }

    fn short_flat_string(&self) -> Option<&Utf16String> {
        if self.deferred_kind() != DeferredKind::None {
            return None;
        }

        let string = self.resolved_utf16_string()?;
        has_short_ascii_storage(string).then_some(string)
    }

    fn try_create_short_flat_concatenated_string(
        vm: &Vm,
        lhs: &PrimitiveString,
        rhs: &PrimitiveString,
    ) -> Option<Gc<PrimitiveString>> {
        let string = concatenate_short_ascii_strings(lhs.short_flat_string()?, rhs.short_flat_string()?)?;
        Some(Self::create(vm, string))
    }

    pub fn create(vm: &Vm, string: Utf16String) -> Gc<PrimitiveString> {
        let view = Utf16View::of_string(&string);
        if view.is_empty() {
            return vm.empty_string();
        }

        if view.length_in_code_units() == 1 {
            let code_unit = view.code_unit_at(0);
            if is_ascii(code_unit) {
                return vm.single_ascii_character_string(code_unit as u8);
            }
        }

        if has_short_ascii_storage(&string) {
            return Self::create_from_fly_string(vm, &to_utf16_fly_string(&string));
        }

        vm.heap().allocate(Self::new(string))
    }

    /// Creates a string of ASCII characters.
    pub fn create_from_ascii(vm: &Vm, characters: &[u8]) -> Gc<PrimitiveString> {
        debug_assert!(characters.is_ascii());
        if characters.len() < size_of::<usize>() || characters.len() > INLINE_STRING_CAPACITY {
            return Self::create(vm, Utf16String::from_ascii(characters));
        }
        vm.heap().allocate(InlineString::new(characters)).upcast()
    }

    pub fn create_from_utf16_view(vm: &Vm, string: Utf16View<'_>) -> Gc<PrimitiveString> {
        Self::create(vm, string.to_utf16_string())
    }

    pub fn create_from_utf8(vm: &Vm, string: &str) -> Gc<PrimitiveString> {
        Self::create(vm, Utf16String::from_utf8(string))
    }

    pub fn create_from_fly_string(vm: &Vm, string: &Utf16FlyString) -> Gc<PrimitiveString> {
        let view = Utf16View::of_fly_string(string);
        if view.is_empty() {
            return vm.empty_string();
        }

        if view.length_in_code_units() == 1 {
            let code_unit = view.code_unit_at(0);
            if is_ascii(code_unit) {
                return vm.single_ascii_character_string(code_unit as u8);
            }
        }

        let string_cache = vm.fly_string_cache();
        let cache_slot = &string_cache[Self::fly_string_cache_hash(string) & (string_cache.len() - 1)];
        if let Some(cached_string) = cache_slot.get()
            && cached_string
                .resolved_utf16_string()
                .is_some_and(|cached| cached.raw_identity() == string.raw_identity())
        {
            return cached_string;
        }

        // Prefer the interned string, so that comparisons with constants are pointer comparisons.
        let interned_string = vm.interned_strings().borrow().get(&string.raw_identity()).copied();
        if let Some(interned_string) = interned_string {
            cache_slot.set(Some(interned_string));
            return interned_string;
        }

        let new_string = vm.heap().allocate(Self::new(Utf16String::from(string)));
        cache_slot.set(Some(new_string));
        new_string
    }

    /// The interned string with these contents, see is_interned().
    pub fn create_interned(vm: &Vm, string: &Utf16FlyString) -> Gc<PrimitiveString> {
        let view = Utf16View::of_fly_string(string);
        if view.is_empty() {
            return vm.empty_string();
        }

        if view.length_in_code_units() == 1 {
            let code_unit = view.code_unit_at(0);
            if is_ascii(code_unit) {
                return vm.single_ascii_character_string(code_unit as u8);
            }
        }

        let string_cache = vm.fly_string_cache();
        let cache_slot = &string_cache[Self::fly_string_cache_hash(string) & (string_cache.len() - 1)];

        let interned_string = vm.interned_strings().borrow().get(&string.raw_identity()).copied();
        if let Some(interned_string) = interned_string {
            cache_slot.set(Some(interned_string));
            return interned_string;
        }

        // NB: There is no interned string with these contents yet, so a cached one can become it.
        let interned_string = match cache_slot.get() {
            Some(cached_string)
                if cached_string
                    .resolved_utf16_string()
                    .is_some_and(|cached| cached.raw_identity() == string.raw_identity()) =>
            {
                cached_string
            }
            _ => vm.heap().allocate(Self::new(Utf16String::from(string))),
        };
        interned_string.set_interned();
        vm.interned_strings()
            .borrow_mut()
            .insert(string.raw_identity(), interned_string);
        cache_slot.set(Some(interned_string));
        interned_string
    }

    pub fn create_from_unsigned_integer(vm: &Vm, number: u64) -> Gc<PrimitiveString> {
        let numeric_string_cache = vm.numeric_string_cache();
        if number < numeric_string_cache.len() as u64 {
            let cache_slot = &numeric_string_cache[number as usize];
            if cache_slot.get().is_none() {
                let string = Utf16FlyString::from_utf8(DecimalDigits::new(number).as_str());
                cache_slot.set(Some(Self::create_from_fly_string(vm, &string)));
            }
            return cache_slot.get().expect("the numeric string was just cached");
        }

        let large_cache = vm.large_numeric_string_cache();
        let cache_entry = &large_cache[(number & (large_cache.len() as u64 - 1)) as usize];
        if cache_entry.string.get().is_none() || cache_entry.number.get() != number {
            cache_entry.number.set(number);
            let string = Utf16FlyString::from_utf8(DecimalDigits::new(number).as_str());
            cache_entry.string.set(Some(Self::create_from_fly_string(vm, &string)));
        }
        cache_entry.string.get().expect("the numeric string was just cached")
    }

    fn try_create_inline_concatenated_string(
        vm: &Vm,
        lhs: &PrimitiveString,
        rhs: &PrimitiveString,
    ) -> Option<Gc<PrimitiveString>> {
        let length = lhs.length_in_utf16_code_units() + rhs.length_in_utf16_code_units();
        if length > INLINE_STRING_CAPACITY
            || lhs.deferred_kind() == DeferredKind::Rope
            || rhs.deferred_kind() == DeferredKind::Rope
        {
            return None;
        }
        let (Utf16View::Ascii(lhs_characters), Utf16View::Ascii(rhs_characters)) =
            (lhs.utf16_string_view(), rhs.utf16_string_view())
        else {
            return None;
        };
        let mut characters = [0; INLINE_STRING_CAPACITY];
        characters[..lhs_characters.len()].copy_from_slice(lhs_characters);
        characters[lhs_characters.len()..length].copy_from_slice(rhs_characters);
        Some(Self::create_from_ascii(vm, &characters[..length]))
    }

    pub fn create_from_concatenation(
        vm: &Vm,
        lhs: Gc<PrimitiveString>,
        rhs: Gc<PrimitiveString>,
    ) -> ThrowCompletionOr<Gc<PrimitiveString>> {
        if rhs.length_in_utf16_code_units() >= u32::MAX as usize - lhs.length_in_utf16_code_units() {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"string"]);
        }

        // We're here to concatenate two strings into a new rope string. However, if any of them are empty, no rope is required.
        let lhs_empty = lhs.is_empty();
        let rhs_empty = rhs.is_empty();

        if lhs_empty && rhs_empty {
            return Ok(vm.empty_string());
        }

        if lhs_empty {
            return Ok(rhs);
        }

        if rhs_empty {
            return Ok(lhs);
        }

        if let Some(short_flat_string) = Self::try_create_short_flat_concatenated_string(vm, &lhs, &rhs) {
            return Ok(short_flat_string);
        }

        // OPTIMIZATION: Copying a few ASCII characters is cheaper than making a rope and resolving it later.
        if let Some(inline_string) = Self::try_create_inline_concatenated_string(vm, &lhs, &rhs) {
            return Ok(inline_string);
        }

        Ok(vm.heap().allocate(RopeString::new(lhs, rhs)).upcast())
    }

    pub fn create_from_substring(
        vm: &Vm,
        string: Gc<PrimitiveString>,
        code_unit_offset: usize,
        code_unit_length: usize,
    ) -> Gc<PrimitiveString> {
        let string_length = string.length_in_utf16_code_units();
        assert!(code_unit_offset <= string_length);
        assert!(code_unit_length <= string_length - code_unit_offset);

        if code_unit_length == 0 {
            return vm.empty_string();
        }

        if code_unit_offset == 0 && code_unit_length == string_length {
            return string;
        }

        if code_unit_length == 1 {
            let code_unit = string.utf16_string_view().code_unit_at(code_unit_offset);
            if is_ascii(code_unit) {
                return vm.single_ascii_character_string(code_unit as u8);
            }
        }

        // OPTIMIZATION: A few ASCII characters are copied rather than kept in their source, which a substring would keep
        //               alive and copy out of when resolved.
        if code_unit_length <= INLINE_STRING_CAPACITY
            && string.deferred_kind() != DeferredKind::Rope
            && let Utf16View::Ascii(characters) = string.utf16_string_view()
        {
            let mut copied_characters = [0; INLINE_STRING_CAPACITY];
            copied_characters[..code_unit_length]
                .copy_from_slice(&characters[code_unit_offset..code_unit_offset + code_unit_length]);
            return Self::create_from_ascii(vm, &copied_characters[..code_unit_length]);
        }

        if string.deferred_kind() == DeferredKind::Substring {
            let substring = string.as_substring();
            return Self::create_from_substring(
                vm,
                substring.source_string(),
                substring.code_unit_offset + code_unit_offset,
                code_unit_length,
            );
        }

        vm.heap()
            .allocate(Substring::new(string, code_unit_offset, code_unit_length))
            .upcast()
    }

    fn new_deferred(class: &'static Class, deferred_kind: DeferredKind, length_in_utf16_code_units: usize) -> Self {
        assert!(length_in_utf16_code_units < u32::MAX as usize);
        Self {
            header: CellHeader::for_class(class),
            deferred_kind_and_flags: Cell::new(deferred_kind as u8),
            length_in_utf16_code_units: Cell::new(length_in_utf16_code_units as u32),
            utf16_string: Utf16StringSlot(UnsafeCell::new(None)),
        }
    }

    pub(crate) fn new(string: Utf16String) -> Self {
        let length_in_utf16_code_units = Utf16View::of_string(&string).length_in_code_units() as u32;
        Self {
            header: CellHeader::for_class(Self::CLASS),
            deferred_kind_and_flags: Cell::new(DeferredKind::None as u8),
            length_in_utf16_code_units: Cell::new(length_in_utf16_code_units),
            utf16_string: Utf16StringSlot(UnsafeCell::new(Some(string))),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.length_in_utf16_code_units.get() == 0
    }

    pub fn utf16_string(&self) -> Utf16String {
        self.resolve_if_needed();

        self.resolved_utf16_string()
            .expect("a resolved string has its UTF-16 string")
            .clone()
    }

    pub fn property_key(&self, vm: &Vm) -> PropertyKey {
        // NB: The key of an inline string is made from its characters, so that it is not resolved.
        if self.deferred_kind() != DeferredKind::Inline {
            self.resolve_if_needed();
            let string = self
                .resolved_utf16_string()
                .expect("a resolved string has its UTF-16 string");
            if has_fly_string_storage(string) {
                return PropertyKey::from(to_utf16_fly_string(string));
            }
        }

        let this = core::ptr::from_ref(self);
        let mut string_to_atom_cache = vm.string_to_atom_cache().borrow_mut();
        let index = (u64_hash(this.addr() as u64) as usize) & (string_to_atom_cache.len() - 1);
        let entry = &mut string_to_atom_cache[index];
        if entry.string.is_some_and(|cached| core::ptr::eq(cached.as_ptr(), this)) {
            return PropertyKey::from(entry.atom.clone().expect("a cached string has its atom"));
        }

        let fly_string = match self.resolved_utf16_string() {
            Some(string) => to_utf16_fly_string(string),
            None => Utf16FlyString::from_utf8(
                core::str::from_utf8(self.as_inline_string().characters()).expect("an inline string is ASCII"),
            ),
        };
        *entry = StringToAtomCacheEntry {
            // SAFETY: Strings only exist as cells, since every way to create one allocates it.
            string: Some(unsafe { Gc::from_ref(self) }),
            atom: Some(fly_string.clone()),
        };
        PropertyKey::from(fly_string)
    }

    pub fn has_utf16_string(&self) -> bool {
        self.resolved_utf16_string().is_some()
    }

    pub fn length_in_utf16_code_units(&self) -> usize {
        self.length_in_utf16_code_units.get() as usize
    }

    pub fn code_unit_at(&self, index: usize) -> u16 {
        self.utf16_string_view().code_unit_at(index)
    }

    /// Converts to UTF-8, replacing unpaired surrogates with U+FFFD.
    pub fn to_utf8(&self) -> String {
        self.utf16_string_view().to_utf8()
    }

    pub fn get(&self, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<Option<Value>> {
        if property_key.is_symbol() {
            return Ok(None);
        }

        if property_key.is_string() && property_key.as_string() == vm.names.length.as_string() {
            return Ok(Some(Value::from_f64(self.length_in_utf16_code_units() as f64)));
        }

        // CanonicalNumericIndexString in CanonicalIndexMode::IgnoreNumericRoundtrip only treats number keys as indices.
        if !property_key.is_number() {
            return Ok(None);
        }
        let index = property_key.as_number() as usize;

        let string = self.utf16_string_view();
        if string.length_in_code_units() <= index {
            return Ok(None);
        }

        // SAFETY: Strings only exist as cells, since every way to create one allocates it.
        let this = unsafe { Gc::from_ref(self) };
        Ok(Some(Value::from_string(Self::create_from_substring(
            vm, this, index, 1,
        ))))
    }

    /// A view of the string's code units. A substring that is not resolved yet is viewed in the string it was taken
    /// from, so the view must not be held across anything that can collect garbage.
    pub fn utf16_string_view(&self) -> Utf16View<'_> {
        if !self.has_utf16_string() {
            if self.deferred_kind() == DeferredKind::Inline {
                return Utf16View::Ascii(self.as_inline_string().characters());
            }
            if self.deferred_kind() == DeferredKind::Substring {
                let substring = self.as_substring();
                let source_string = substring.source_string();
                // SAFETY: This substring keeps its source alive until it resolves, and resolving cannot collect
                // garbage, so the source outlives every view a caller may hold without collecting.
                let source_string = unsafe { source_string.as_non_null().as_ref() };
                return source_string
                    .utf16_string_view()
                    .substring_view(substring.code_unit_offset, self.length_in_utf16_code_units());
            }
            self.resolve_if_needed();
        }
        Utf16View::of_string(
            self.resolved_utf16_string()
                .expect("a resolved string has its UTF-16 string"),
        )
    }

    /// A view of the string's own code units, resolving a rope or a substring first. Unlike utf16_string_view(), the
    /// view stays valid for as long as the string lives, since a resolved string never changes.
    pub fn resolved_utf16_string_view(&self) -> Utf16View<'_> {
        if self.deferred_kind() == DeferredKind::Inline {
            return Utf16View::Ascii(self.as_inline_string().characters());
        }
        self.resolve_if_needed();
        Utf16View::of_string(
            self.resolved_utf16_string()
                .expect("a resolved string has its UTF-16 string"),
        )
    }

    fn resolved_utf16_string(&self) -> Option<&Utf16String> {
        // SAFETY: The slot is only written while it is empty, so a string read out of it is never replaced.
        unsafe { (*self.utf16_string.0.get()).as_ref() }
    }

    fn set_resolved_utf16_string(&self, string: Utf16String) {
        assert!(!self.has_utf16_string());
        // SAFETY: The slot is empty, so nothing borrows the string it holds.
        unsafe { *self.utf16_string.0.get() = Some(string) };
    }

    fn resolve_if_needed(&self) {
        match self.deferred_kind() {
            DeferredKind::None => {}
            DeferredKind::Rope => self.as_rope_string().resolve(),
            DeferredKind::Substring => self.as_substring().resolve(),
            DeferredKind::Inline => self.as_inline_string().resolve(),
        }
    }

    fn as_inline_string(&self) -> &InlineString {
        debug_assert!(self.header_class().is_subclass_of(InlineString::CLASS));
        // SAFETY: Only InlineString creates strings that are deferred as inline strings, and it starts with its
        //         PrimitiveString.
        unsafe { &*core::ptr::from_ref(self).cast::<InlineString>() }
    }

    fn as_rope_string(&self) -> &RopeString {
        debug_assert!(self.header_class().is_subclass_of(RopeString::CLASS));
        // SAFETY: Only RopeString creates strings that are deferred as ropes, and it starts with its PrimitiveString.
        unsafe { &*core::ptr::from_ref(self).cast::<RopeString>() }
    }

    fn as_substring(&self) -> &Substring {
        debug_assert!(self.header_class().is_subclass_of(Substring::CLASS));
        // SAFETY: Only Substring creates strings that are deferred as substrings, and it starts with its
        // PrimitiveString.
        unsafe { &*core::ptr::from_ref(self).cast::<Substring>() }
    }

    fn header_class(&self) -> &'static Class {
        self.header.class
    }

    pub fn deferred_kind(&self) -> DeferredKind {
        match self.deferred_kind_and_flags.get() & DEFERRED_KIND_MASK {
            0 => DeferredKind::None,
            1 => DeferredKind::Rope,
            2 => DeferredKind::Substring,
            3 => DeferredKind::Inline,
            _ => unreachable!("a string has a valid deferred kind"),
        }
    }

    fn set_deferred_kind(&self, deferred_kind: DeferredKind) {
        let flags = self.deferred_kind_and_flags.get() & !DEFERRED_KIND_MASK;
        self.deferred_kind_and_flags.set(flags | deferred_kind as u8);
    }

    /// An interned string is the only interned PrimitiveString with its contents, so two interned strings are equal if
    /// and only if they are the same string. The string constants of executables and the strings typeof produces are
    /// interned; create_from_fly_string() returns the interned string if there is one.
    pub fn is_interned(&self) -> bool {
        self.deferred_kind_and_flags.get() & INTERNED_FLAG != 0
    }

    pub(crate) fn set_interned(&self) {
        assert!(self.deferred_kind() == DeferredKind::None);
        self.deferred_kind_and_flags
            .set(self.deferred_kind_and_flags.get() | INTERNED_FLAG);
    }
}

impl PartialEq for PrimitiveString {
    fn eq(&self, other: &Self) -> bool {
        if core::ptr::eq(self, other) {
            return true;
        }
        if self.is_interned() && other.is_interned() {
            return false;
        }
        if self.length_in_utf16_code_units() != other.length_in_utf16_code_units() {
            return false;
        }
        if let (Some(string), Some(other_string)) = (self.resolved_utf16_string(), other.resolved_utf16_string()) {
            return string == other_string;
        }
        self.utf16_string_view() == other.utf16_string_view()
    }
}

impl Eq for PrimitiveString {}

impl Utf16Display for Gc<PrimitiveString> {
    fn fmt_utf16(&self, builder: &mut Utf16StringBuilder) {
        builder.append(self.utf16_string_view());
    }
}

impl RopeString {
    fn new(lhs: Gc<PrimitiveString>, rhs: Gc<PrimitiveString>) -> Self {
        Self {
            base: PrimitiveString::new_deferred(
                Self::CLASS,
                DeferredKind::Rope,
                lhs.length_in_utf16_code_units() + rhs.length_in_utf16_code_units(),
            ),
            lhs: Cell::new(Some(lhs)),
            rhs: Cell::new(Some(rhs)),
        }
    }

    fn lhs(&self) -> Gc<PrimitiveString> {
        self.lhs.get().expect("an unresolved rope has both sides")
    }

    fn rhs(&self) -> Gc<PrimitiveString> {
        self.rhs.get().expect("an unresolved rope has both sides")
    }

    fn resolve(&self) {
        let string =
            if self.lhs().deferred_kind() != DeferredKind::Rope && self.rhs().deferred_kind() != DeferredKind::Rope {
                concatenate(&[self.lhs().utf16_string_view(), self.rhs().utf16_string_view()])
            } else {
                self.concatenate_pieces()
            };
        debug_assert_eq!(
            Utf16View::of_string(&string).length_in_code_units(),
            self.base.length_in_utf16_code_units()
        );

        self.base.set_resolved_utf16_string(string);
        self.base.set_deferred_kind(DeferredKind::None);
        self.lhs.set(None);
        self.rhs.set(None);
    }

    fn concatenate_pieces(&self) -> Utf16String {
        // OPTIMIZATION: Most ropes have a few pieces, which are gathered without allocating.
        const INLINE_PIECES: usize = 8;
        let mut pieces = [self.lhs(); INLINE_PIECES];
        let mut piece_count = 0;
        let mut stack = [self.lhs(); INLINE_PIECES];
        stack[0] = self.rhs();
        stack[1] = self.lhs();
        let mut stack_depth = 2;
        while stack_depth > 0 {
            stack_depth -= 1;
            let current = stack[stack_depth];
            if current.deferred_kind() == DeferredKind::Rope {
                if stack_depth + 2 > INLINE_PIECES {
                    return self.concatenate_many_pieces();
                }
                let current_rope_string = current.as_rope_string();
                stack[stack_depth] = current_rope_string.rhs();
                stack[stack_depth + 1] = current_rope_string.lhs();
                stack_depth += 2;
                continue;
            }
            if piece_count == INLINE_PIECES {
                return self.concatenate_many_pieces();
            }
            pieces[piece_count] = current;
            piece_count += 1;
        }

        let mut views = [Utf16View::Ascii(&[]); INLINE_PIECES];
        for (view, piece) in views.iter_mut().zip(&pieces[..piece_count]) {
            *view = piece.utf16_string_view();
        }
        concatenate(&views[..piece_count])
    }

    fn concatenate_many_pieces(&self) -> Utf16String {
        // This vector will hold all the pieces of the rope that need to be assembled
        // into the resolved string.
        // NB: Resolving takes no VM, so it cannot allocate cells or collect garbage while these vectors hold strings
        //     that only the rope keeps alive.
        let mut pieces: Vec<Gc<PrimitiveString>> = Vec::with_capacity(2);

        // NOTE: We traverse the rope tree without using recursion, since we'd run out of
        //       stack space quickly when handling a long sequence of unresolved concatenations.
        let mut stack: Vec<Gc<PrimitiveString>> = Vec::with_capacity(2);
        stack.push(self.rhs());
        stack.push(self.lhs());
        while let Some(current) = stack.pop() {
            if current.deferred_kind() == DeferredKind::Rope {
                let current_rope_string = current.as_rope_string();
                stack.push(current_rope_string.rhs());
                stack.push(current_rope_string.lhs());
                continue;
            }

            pieces.push(current);
        }

        let views: Vec<Utf16View<'_>> = pieces.iter().map(|piece| piece.utf16_string_view()).collect();
        concatenate(&views)
    }
}

impl Substring {
    fn new(source_string: Gc<PrimitiveString>, code_unit_offset: usize, code_unit_length: usize) -> Self {
        Self {
            base: PrimitiveString::new_deferred(Self::CLASS, DeferredKind::Substring, code_unit_length),
            source_string: Cell::new(Some(source_string)),
            code_unit_offset,
        }
    }

    fn source_string(&self) -> Gc<PrimitiveString> {
        self.source_string
            .get()
            .expect("an unresolved substring has its source")
    }

    fn resolve(&self) {
        let source_string = self.source_string();
        let source_view = source_string
            .utf16_string_view()
            .substring_view(self.code_unit_offset, self.base.length_in_utf16_code_units());

        let string = source_view.to_utf16_string();
        self.base.set_resolved_utf16_string(string);
        self.base.set_deferred_kind(DeferredKind::None);
        self.source_string.set(None);
    }
}

impl InlineString {
    fn new(characters: &[u8]) -> Self {
        let mut string = Self {
            base: PrimitiveString::new_deferred(Self::CLASS, DeferredKind::Inline, characters.len()),
            characters: [0; INLINE_STRING_CAPACITY],
        };
        string.characters[..characters.len()].copy_from_slice(characters);
        string
    }

    fn characters(&self) -> &[u8] {
        &self.characters[..self.base.length_in_utf16_code_units()]
    }

    fn resolve(&self) {
        self.base
            .set_resolved_utf16_string(Utf16String::from_ascii(self.characters()));
        self.base.set_deferred_kind(DeferredKind::None);
    }
}
