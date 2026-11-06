/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The string, identifier and property key tables of an executable. Their order is part of the output: a text takes the
//! next index of a table the first time it comes up, whether it comes up as a name of the tree or as any other text, and
//! both ways have to land on the same entry.
//!
//! Generating code takes no global lock and touches no fly string: an entry is the `Name` it came from, and entries are
//! found by a hash of their text that is known once per name of a script. Fly strings only come into it when the tables
//! leave the compile, and then only once per name of a script on this thread. Everything here is per thread, so
//! compiling the functions of a script on several threads needs no lock on the script either.

use std::sync::Arc;
use std::sync::Weak;

use ak::Utf16FlyString;

use crate::ast::Name;
use crate::ast::SyntaxTree;
use crate::bytecode::operand::IdentifierTableIndex;
use crate::bytecode::operand::PropertyKeyTableIndex;
use crate::bytecode::operand::StringTableIndex;
use crate::layout::buffer::BlockSlice;
use crate::parser::ScriptData;
use crate::slice_block::SliceBlock;
use crate::slice_block::SliceBlockBuilder;
use crate::u32_from_usize;
use crate::utf16::Utf16View;

use super::constants::IndexTable;
use super::constants::hash_of_units;
use super::context::MAXIMUM_SPARE_CAPACITY;

const NO_INDEX: u32 = u32::MAX;

/// How many texts that are no names a thread remembers of a script.
const MAXIMUM_SCRIPT_TEXTS: usize = 1 << 16;

/// What code generation treats specially about a name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum NameClass {
    Plain,
    Undefined,
    NaN,
    Infinity,
    Eval,
    Length,
}

impl NameClass {
    fn of(text: Utf16View<'_>) -> Self {
        if text == utf16!("undefined") {
            Self::Undefined
        } else if text == utf16!("NaN") {
            Self::NaN
        } else if text == utf16!("Infinity") {
            Self::Infinity
        } else if text == utf16!("eval") {
            Self::Eval
        } else if text == utf16!("length") {
            Self::Length
        } else {
            Self::Plain
        }
    }
}

/// What is known about a name once it came up.
#[derive(Clone, Copy)]
struct NameFacts {
    /// The hash of the name's text, or zero if the name did not come up yet.
    hash: u64,
    class: NameClass,
}

/// What a script's names turn into, kept from one function of the script to the next on this thread.
#[derive(Default)]
struct ScriptNames {
    /// Compared by address only. The weak reference keeps the address from going to another script.
    script: Weak<ScriptData>,
    facts: Vec<NameFacts>,
    fly_strings: Vec<Option<Utf16FlyString>>,
    /// Texts that are no names, which code that was minified repeats a lot: `a.b` for the error of a call, for example.
    texts: Vec<Utf16FlyString>,
    text_hashes: Vec<u64>,
    text_index: IndexTable,
}

impl ScriptNames {
    fn adopt(&mut self, script: &Arc<ScriptData>) {
        if self.script.as_ptr() != Arc::as_ptr(script) {
            self.script = Arc::downgrade(script);
            self.facts.clear();
            self.fly_strings.clear();
            self.forget_texts();
        }
    }

    fn forget_texts(&mut self) {
        self.texts.clear();
        self.text_hashes.clear();
        self.text_index.clear();
    }

    /// The id of a text, which only takes the global table once per text of a script on this thread.
    #[inline(always)]
    fn text_id(&mut self, text: Utf16View<'_>, hash: u64) -> u32 {
        if self.texts.len() >= MAXIMUM_SCRIPT_TEXTS {
            self.forget_texts();
        }
        let texts = &self.texts;
        let found = self
            .text_index
            .find(hash, |index| Utf16View::of_fly_string(&texts[index as usize]) == text);
        match found {
            Ok(index) => index,
            Err(slot) => {
                let index = u32_from_usize(self.texts.len());
                self.texts.push(text.to_utf16_fly_string());
                self.text_hashes.push(hash);
                self.text_index.insert_at(slot, hash, index);
                index
            }
        }
    }

    #[inline(always)]
    fn facts_of(&mut self, tree: &SyntaxTree, name: Name) -> NameFacts {
        let index = name.0 as usize;
        if index >= self.facts.len() || self.facts[index].hash == 0 {
            return self.learn(tree, name);
        }
        self.facts[index]
    }

    #[cold]
    fn learn(&mut self, tree: &SyntaxTree, name: Name) -> NameFacts {
        let index = name.0 as usize;
        if index >= self.facts.len() {
            let unknown = NameFacts {
                hash: 0,
                class: NameClass::Plain,
            };
            self.facts.resize((index + 1).next_power_of_two(), unknown);
        }
        let text = tree.name_text(name);
        self.facts[index] = NameFacts {
            hash: hash_of_units(text),
            class: NameClass::of(text),
        };
        self.facts[index]
    }

    #[inline(always)]
    fn fly_string(&mut self, tree: &SyntaxTree, name: Name) -> &Utf16FlyString {
        let index = name.0 as usize;
        if index >= self.fly_strings.len() {
            self.fly_strings.resize((index + 1).next_power_of_two(), None);
        }
        self.fly_strings[index].get_or_insert_with(|| tree.name_text(name).to_utf16_fly_string())
    }
}

#[derive(Clone, Copy, Default)]
struct NameSlot {
    name: u32,
    generation: u32,
    identifier: u32,
    property_key: u32,
}

/// Where the names of the tree went in the tables, in an open-addressed table that forgets everything by moving on to a
/// new generation: the names of a function are few, and looking them up takes no strings.
#[derive(Default)]
struct NameIndices {
    slots: Vec<NameSlot>,
    used_slots: usize,
    generation: u32,
}

impl NameIndices {
    const INITIAL_SLOT_COUNT: usize = 64;

    fn clear(&mut self) {
        if self.slots.len() > MAXIMUM_SPARE_CAPACITY {
            self.slots = Vec::new();
            self.generation = 0;
        }
        self.used_slots = 0;
        self.generation = self.generation.wrapping_add(1);
        if self.generation == 0 {
            self.slots.fill(NameSlot::default());
            self.generation = 1;
        }
    }

    #[inline(always)]
    fn first_slot_of(name: u32, slot_count: usize) -> usize {
        // NB: Names count up from zero in order of appearance, which an odd multiplier spreads over the slots.
        (name.wrapping_mul(0x9E37_79B1) >> 7) as usize & (slot_count - 1)
    }

    #[inline(always)]
    fn slot_of(&mut self, name: Name) -> &mut NameSlot {
        if (self.used_slots + 1) * 4 > self.slots.len() * 3 {
            self.grow();
        }
        let mask = self.slots.len() - 1;
        let mut slot = Self::first_slot_of(name.0, self.slots.len());
        loop {
            let entry = self.slots[slot];
            if entry.generation != self.generation {
                self.used_slots += 1;
                self.slots[slot] = NameSlot {
                    name: name.0,
                    generation: self.generation,
                    identifier: NO_INDEX,
                    property_key: NO_INDEX,
                };
                return &mut self.slots[slot];
            }
            if entry.name == name.0 {
                return &mut self.slots[slot];
            }
            slot = (slot + 1) & mask;
        }
    }

    #[cold]
    fn grow(&mut self) {
        // NB: Slots that were never used are of generation zero, which no table is.
        self.generation = self.generation.max(1);
        let slot_count = (self.slots.len() * 2).max(Self::INITIAL_SLOT_COUNT);
        let old_slots = std::mem::replace(&mut self.slots, vec![NameSlot::default(); slot_count]);
        for old_slot in old_slots {
            if old_slot.generation == self.generation {
                let mut slot = Self::first_slot_of(old_slot.name, slot_count);
                while self.slots[slot].generation == self.generation {
                    slot = (slot + 1) & (slot_count - 1);
                }
                self.slots[slot] = old_slot;
            }
        }
    }
}

/// An entry of a table, in the form it came up in.
enum TableEntry {
    Name(Name),
    Text(Utf16FlyString),
}

/// One table of the executable. Entries are found by the hash of their text, which also finds a name by its text.
#[derive(Default)]
struct Table {
    entries: Vec<TableEntry>,
    index_of: IndexTable,
}

impl Table {
    fn clear(&mut self) {
        self.entries.clear();
        self.index_of.clear();
    }

    #[inline(always)]
    fn intern_name(&mut self, tree: &SyntaxTree, name: Name, hash: u64) -> u32 {
        let entries = &self.entries;
        let text = tree.name_text(name);
        let found = self.index_of.find(hash, |index| match &entries[index as usize] {
            TableEntry::Name(other) => *other == name,
            TableEntry::Text(other) => Utf16View::of_fly_string(other) == text,
        });
        match found {
            Ok(index) => index,
            Err(slot) => {
                let index = u32_from_usize(self.entries.len());
                self.entries.push(TableEntry::Name(name));
                self.index_of.insert_at(slot, hash, index);
                index
            }
        }
    }

    fn intern_text(&mut self, tree: &SyntaxTree, script_names: &mut ScriptNames, text: Utf16View<'_>) -> u32 {
        let hash = hash_of_units(text);
        let entries = &self.entries;
        let found = self.index_of.find(hash, |index| match &entries[index as usize] {
            TableEntry::Name(other) => tree.name_text(*other) == text,
            TableEntry::Text(other) => Utf16View::of_fly_string(other) == text,
        });
        match found {
            Ok(index) => index,
            Err(slot) => {
                let index = u32_from_usize(self.entries.len());
                let id = script_names.text_id(text, hash);
                self.entries
                    .push(TableEntry::Text(script_names.texts[id as usize].clone()));
                self.index_of.insert_at(slot, hash, index);
                index
            }
        }
    }
}

#[derive(Default)]
pub(super) struct NameTables {
    script_names: ScriptNames,
    name_indices: NameIndices,
    identifiers: Table,
    property_keys: Table,
    strings: Table,
    length_property_key: Option<u32>,
}

macro_rules! define_intern_methods {
    ($name_method:ident, $text_method:ident, $index_type:ident, $table:ident, $slot_field:ident) => {
        #[inline(always)]
        pub(super) fn $name_method(&mut self, tree: &SyntaxTree, name: Name) -> $index_type {
            let index = self.name_indices.slot_of(name).$slot_field;
            if index != NO_INDEX {
                return $index_type(index);
            }
            let hash = self.script_names.facts_of(tree, name).hash;
            let index = self.$table.intern_name(tree, name, hash);
            self.name_indices.slot_of(name).$slot_field = index;
            $index_type(index)
        }

        pub(super) fn $text_method(&mut self, tree: &SyntaxTree, text: Utf16View<'_>) -> $index_type {
            $index_type(self.$table.intern_text(tree, &mut self.script_names, text))
        }
    };
}

impl NameTables {
    pub(super) fn begin_compile(&mut self, script: &Arc<ScriptData>) {
        self.script_names.adopt(script);
    }

    pub(super) fn clear(&mut self) {
        self.name_indices.clear();
        self.identifiers.clear();
        self.property_keys.clear();
        self.strings.clear();
        self.length_property_key = None;
    }

    define_intern_methods!(
        intern_identifier,
        intern_identifier_text,
        IdentifierTableIndex,
        identifiers,
        identifier
    );
    pub(super) fn intern_string_text(&mut self, tree: &SyntaxTree, text: Utf16View<'_>) -> StringTableIndex {
        StringTableIndex(self.strings.intern_text(tree, &mut self.script_names, text))
    }

    #[inline(always)]
    pub(super) fn intern_property_key(&mut self, tree: &SyntaxTree, name: Name) -> PropertyKeyTableIndex {
        let index = self.name_indices.slot_of(name).property_key;
        if index != NO_INDEX {
            return PropertyKeyTableIndex(index);
        }
        let facts = self.script_names.facts_of(tree, name);
        let index = self.property_keys.intern_name(tree, name, facts.hash);
        self.name_indices.slot_of(name).property_key = index;
        if facts.class == NameClass::Length {
            self.length_property_key = Some(index);
        }
        PropertyKeyTableIndex(index)
    }

    pub(super) fn intern_property_key_text(&mut self, tree: &SyntaxTree, text: Utf16View<'_>) -> PropertyKeyTableIndex {
        let index = self.property_keys.intern_text(tree, &mut self.script_names, text);
        if text == utf16!("length") {
            self.length_property_key = Some(index);
        }
        PropertyKeyTableIndex(index)
    }

    #[inline(always)]
    pub(super) fn class_of(&mut self, tree: &SyntaxTree, name: Name) -> NameClass {
        self.script_names.facts_of(tree, name).class
    }

    #[inline(always)]
    pub(super) fn is_length_property_key(&self, key: PropertyKeyTableIndex) -> bool {
        self.length_property_key == Some(key.0)
    }

    /// The text of an entry of the identifier table as a fly string, which is what environment scopes know names by.
    pub(super) fn identifier_fly_string(
        &mut self,
        tree: &SyntaxTree,
        identifier: IdentifierTableIndex,
    ) -> Utf16FlyString {
        match &self.identifiers.entries[identifier.0 as usize] {
            TableEntry::Name(name) => self.script_names.fly_string(tree, *name).clone(),
            TableEntry::Text(text) => text.clone(),
        }
    }

    /// The fly string of a text with `hash`, which only takes the global table once per text of a script on this thread.
    pub(super) fn fly_string_of_text(&mut self, text: Utf16View<'_>, hash: u64) -> &Utf16FlyString {
        let id = self.script_names.text_id(text, hash);
        &self.script_names.texts[id as usize]
    }

    /// The fly string of a name, which only takes the global table once per name of a script on this thread.
    pub(super) fn fly_string_of(&mut self, tree: &SyntaxTree, name: Name) -> Utf16FlyString {
        self.script_names.fly_string(tree, name).clone()
    }

    /// How much of a block the tables that stay with the code take.
    pub(super) fn room_for_tables(&self) -> usize {
        SliceBlock::room_for::<Utf16FlyString>(self.identifiers.entries.len())
            + SliceBlock::room_for::<Utf16FlyString>(self.strings.entries.len())
    }

    /// How much of a block the table that only linking reads takes.
    pub(super) fn room_for_link_tables(&self) -> usize {
        SliceBlock::room_for::<Utf16FlyString>(self.property_keys.entries.len())
    }

    /// Hands out the tables, as an executable keeps them: identifiers and strings from `block`, and property keys from
    /// `link_block`.
    pub(super) fn take_tables(
        &mut self,
        tree: &SyntaxTree,
        block: &mut SliceBlockBuilder,
        link_block: &mut SliceBlockBuilder,
    ) -> (
        BlockSlice<Utf16FlyString>,
        BlockSlice<Utf16FlyString>,
        BlockSlice<Utf16FlyString>,
    ) {
        let script_names = &mut self.script_names;
        let mut fly_string_of = |entry: &TableEntry| match entry {
            TableEntry::Name(name) => script_names.fly_string(tree, *name).clone(),
            TableEntry::Text(text) => text.clone(),
        };
        (
            block.slice(self.identifiers.entries.iter().map(&mut fly_string_of)),
            link_block.slice(self.property_keys.entries.iter().map(&mut fly_string_of)),
            block.slice(self.strings.entries.iter().map(&mut fly_string_of)),
        )
    }
}
