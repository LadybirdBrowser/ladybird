/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The constant pool of an executable. The numbering is part of the output: every value that is not a duplicate takes
//! the next index, and only the kinds that deduplicate here do so.

use crate::bytecode::constant::AbstractOperationKind;
use crate::bytecode::constant::WellKnownSymbolKind;
use crate::bytecode::executable_data::ConstantValue;
use crate::bytecode::executable_data::MAXIMUM_INTERNED_CONSTANT_LENGTH;
use crate::bytecode::operand::Operand;
use crate::u32_from_usize;
use crate::utf16::Utf16View;
use ak::Utf16String;

use super::context::MAXIMUM_SPARE_CAPACITY;
use super::names::NameTables;

/// An open-addressed table from a key to a constant index that forgets everything by moving on to a new generation.
#[derive(Default)]
pub(super) struct IndexTable {
    slots: Vec<IndexSlot>,
    used_slots: usize,
    generation: u32,
}

#[derive(Clone, Copy, Default)]
struct IndexSlot {
    key: u64,
    index: u32,
    generation: u32,
}

impl IndexTable {
    const INITIAL_SLOT_COUNT: usize = 32;

    pub(super) fn clear(&mut self) {
        if self.slots.len() > MAXIMUM_SPARE_CAPACITY {
            self.slots = Vec::new();
            self.generation = 0;
        }
        self.used_slots = 0;
        self.generation = self.generation.wrapping_add(1);
        if self.generation == 0 {
            self.slots.fill(IndexSlot::default());
            self.generation = 1;
        }
    }

    #[inline(always)]
    fn first_slot_of(key: u64, slot_count: usize) -> usize {
        (key.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> 40) as usize & (slot_count - 1)
    }

    /// The slots with the key, in the order a lookup visits them, until an empty one: `matches` decides on each index,
    /// and the result is the index it accepted or the slot to insert at.
    #[inline(always)]
    pub(super) fn find(&mut self, key: u64, matches: impl Fn(u32) -> bool) -> Result<u32, usize> {
        if (self.used_slots + 1) * 4 > self.slots.len() * 3 {
            self.grow();
        }
        let mask = self.slots.len() - 1;
        let mut slot = Self::first_slot_of(key, self.slots.len());
        loop {
            let entry = &self.slots[slot];
            if entry.generation != self.generation {
                return Err(slot);
            }
            if entry.key == key && matches(entry.index) {
                return Ok(entry.index);
            }
            slot = (slot + 1) & mask;
        }
    }

    #[inline(always)]
    pub(super) fn insert_at(&mut self, slot: usize, key: u64, index: u32) {
        self.used_slots += 1;
        self.slots[slot] = IndexSlot {
            key,
            index,
            generation: self.generation,
        };
    }

    #[cold]
    fn grow(&mut self) {
        // NB: Slots that were never used are of generation zero, which no table is.
        self.generation = self.generation.max(1);
        let slot_count = (self.slots.len() * 2).max(Self::INITIAL_SLOT_COUNT);
        let old_slots = std::mem::replace(&mut self.slots, vec![IndexSlot::default(); slot_count]);
        for old_slot in old_slots {
            if old_slot.generation == self.generation {
                let mut slot = Self::first_slot_of(old_slot.key, slot_count);
                while self.slots[slot].generation == self.generation {
                    slot = (slot + 1) & (slot_count - 1);
                }
                self.slots[slot] = old_slot;
            }
        }
    }
}

#[derive(Default)]
pub(super) struct ConstantTable {
    pub(super) values: Vec<ConstantValue>,
    true_constant: Option<u32>,
    false_constant: Option<u32>,
    null_constant: Option<u32>,
    undefined_constant: Option<u32>,
    empty_constant: Option<u32>,
    numbers: IndexTable,
    strings: IndexTable,
}

/// A hash of the code units of a string, which does not depend on how the string stores them.
#[inline(always)]
pub(super) fn hash_of_units(text: Utf16View<'_>) -> u64 {
    const MULTIPLIER: u64 = 0x517C_C1B7_2722_0A95;
    let mut hash = text.length_in_code_units() as u64;
    match text {
        Utf16View::Ascii(units) | Utf16View::Latin1(units) => {
            for &unit in units {
                hash = (hash.rotate_left(5) ^ u64::from(unit)).wrapping_mul(MULTIPLIER);
            }
        }
        Utf16View::Utf16(units) => {
            for &unit in units {
                hash = (hash.rotate_left(5) ^ u64::from(unit)).wrapping_mul(MULTIPLIER);
            }
        }
    }
    // NB: Zero means that a name has no hash yet.
    hash | 1
}

impl ConstantTable {
    pub(super) fn clear(&mut self) {
        self.values.clear();
        self.true_constant = None;
        self.false_constant = None;
        self.null_constant = None;
        self.undefined_constant = None;
        self.empty_constant = None;
        self.numbers.clear();
        self.strings.clear();
    }

    #[inline(always)]
    fn append(&mut self, value: ConstantValue) -> u32 {
        let index = u32_from_usize(self.values.len());
        self.values.push(value);
        index
    }

    /// Deduplicates by bit pattern, which tells -0 from 0, and takes every other number for itself.
    #[inline(always)]
    pub(super) fn add_number(&mut self, value: f64) -> Operand {
        let key = value.to_bits();
        match self.numbers.find(key, |_| true) {
            Ok(index) => Operand::constant(index),
            Err(slot) => {
                let index = self.append(ConstantValue::Number(value));
                self.numbers.insert_at(slot, key, index);
                Operand::constant(index)
            }
        }
    }

    #[inline(always)]
    pub(super) fn add_boolean(&mut self, value: bool) -> Operand {
        let slot = if value {
            &mut self.true_constant
        } else {
            &mut self.false_constant
        };
        if let Some(index) = *slot {
            return Operand::constant(index);
        }
        let index = u32_from_usize(self.values.len());
        *slot = Some(index);
        self.values.push(ConstantValue::Boolean(value));
        Operand::constant(index)
    }

    #[inline(always)]
    pub(super) fn add_null(&mut self) -> Operand {
        if let Some(index) = self.null_constant {
            return Operand::constant(index);
        }
        let index = self.append(ConstantValue::Null);
        self.null_constant = Some(index);
        Operand::constant(index)
    }

    #[inline(always)]
    pub(super) fn add_undefined(&mut self) -> Operand {
        if let Some(index) = self.undefined_constant {
            return Operand::constant(index);
        }
        let index = self.append(ConstantValue::Undefined);
        self.undefined_constant = Some(index);
        Operand::constant(index)
    }

    #[inline(always)]
    pub(super) fn add_empty(&mut self) -> Operand {
        if let Some(index) = self.empty_constant {
            return Operand::constant(index);
        }
        let index = self.append(ConstantValue::Empty);
        self.empty_constant = Some(index);
        Operand::constant(index)
    }

    /// A string whose value is `text`, which is only looked at if the table does not have it yet: a text that gets
    /// interned when the code is linked comes from what `names` knows of the script, and any other gets copied.
    #[inline(always)]
    pub(super) fn add_string_view(&mut self, text: Utf16View<'_>, names: &mut NameTables) -> Operand {
        let key = hash_of_units(text);
        let values = &self.values;
        let found = self.strings.find(key, |index| {
            matches!(&values[index as usize], ConstantValue::String(string) if Utf16View::of_string(string) == text)
        });
        match found {
            Ok(index) => Operand::constant(index),
            Err(slot) => {
                let string = if text.length_in_code_units() <= MAXIMUM_INTERNED_CONSTANT_LENGTH {
                    Utf16String::from(names.fly_string_of_text(text, key))
                } else {
                    text.into()
                };
                let index = self.append(ConstantValue::String(string));
                self.strings.insert_at(slot, key, index);
                Operand::constant(index)
            }
        }
    }

    pub(super) fn add_string(&mut self, value: Utf16String) -> Operand {
        let text = Utf16View::of_string(&value);
        let key = hash_of_units(text);
        let values = &self.values;
        let found = self.strings.find(key, |index| {
            matches!(&values[index as usize], ConstantValue::String(string) if Utf16View::of_string(string) == text)
        });
        match found {
            Ok(index) => Operand::constant(index),
            Err(slot) => {
                let index = self.append(ConstantValue::String(value));
                self.strings.insert_at(slot, key, index);
                Operand::constant(index)
            }
        }
    }

    pub(super) fn add_bigint(&mut self, value: String) -> Operand {
        Operand::constant(self.append(ConstantValue::BigInt(value)))
    }

    pub(super) fn add_well_known_symbol(&mut self, symbol: WellKnownSymbolKind) -> Operand {
        Operand::constant(self.append(ConstantValue::WellKnownSymbol(symbol)))
    }

    pub(super) fn add_abstract_operation(&mut self, operation: AbstractOperationKind) -> Operand {
        Operand::constant(self.append(ConstantValue::AbstractOperation(operation)))
    }

    #[inline(always)]
    pub(super) fn get(&self, operand: Operand) -> Option<&ConstantValue> {
        if operand.is_constant() {
            self.values.get(operand.index() as usize)
        } else {
            None
        }
    }
}
