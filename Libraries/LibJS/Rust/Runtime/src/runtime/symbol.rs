/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::HashMap;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::utf16::Utf16View;

/// Mirrors JS::Symbol::Kind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Kind {
    Unique,
    Global,
    Private,
}

#[repr(C)]
#[derive(Trace)]
pub struct Symbol {
    header: CellHeader,
    #[gc(untraced)]
    kind: Kind,
    description: Option<Utf16String>,
}

define_cell!(Symbol, Symbol);

/// Calls `$callback` with the name and the snake_case name of every well-known symbol, like
/// JS_ENUMERATE_WELL_KNOWN_SYMBOLS.
macro_rules! enumerate_well_known_symbols {
    ($callback:ident) => {
        $callback! {
            (asyncDispose, async_dispose),
            (asyncIterator, async_iterator),
            (dispose, dispose),
            (hasInstance, has_instance),
            (isConcatSpreadable, is_concat_spreadable),
            (iterator, iterator),
            (match, match_),
            (matchAll, match_all),
            (replace, replace),
            (replaceAll, replace_all),
            (search, search),
            (species, species),
            (split, split),
            (toPrimitive, to_primitive),
            (toStringTag, to_string_tag),
            (unscopables, unscopables),
        }
    };
}

pub(crate) use enumerate_well_known_symbols;

impl Symbol {
    fn new(description: Option<Utf16String>, kind: Kind) -> Self {
        Self {
            header: CellHeader::for_class(Self::CLASS),
            kind,
            description,
        }
    }

    pub fn create(vm: &Vm, description: Option<Utf16String>, kind: Kind) -> Gc<Symbol> {
        vm.heap().allocate(Self::new(description, kind))
    }

    pub fn create_private(vm: &Vm) -> Gc<Symbol> {
        Self::create(vm, None, Kind::Private)
    }

    pub fn description(&self) -> Option<&Utf16String> {
        self.description.as_ref()
    }

    pub fn is_global(&self) -> bool {
        self.kind == Kind::Global
    }

    pub fn is_private(&self) -> bool {
        self.kind == Kind::Private
    }

    // 20.4.3.3.1 SymbolDescriptiveString ( sym ), https://tc39.es/ecma262/#sec-symboldescriptivestring
    pub fn descriptive_string(&self) -> Utf16String {
        // 1. Let desc be sym's [[Description]] value.
        // 2. If desc is undefined, set desc to the empty String.
        // 3. Assert: desc is a String.
        let description = self.description.as_ref().map_or(Utf16View::EMPTY, Utf16View::of_string);

        // 4. Return the string-concatenation of "Symbol(", desc, and ")".
        crate::utf16::concatenate(&[Utf16View::Ascii(b"Symbol("), description, Utf16View::Ascii(b")")])
    }

    // 20.4.5.1 KeyForSymbol ( sym ), https://tc39.es/ecma262/#sec-keyforsymbol
    pub fn key(&self) -> Option<Utf16String> {
        // 1. For each element e of the GlobalSymbolRegistry List, do
        //    a. If SameValue(e.[[Symbol]], sym) is true, return e.[[Key]].
        if self.is_global() {
            // NOTE: Global symbols should always have a description string
            assert!(self.description.is_some());
            return self.description.clone();
        }

        // 2. Assert: GlobalSymbolRegistry does not currently contain an entry for sym.
        // 3. Return undefined.
        None
    }
}

/// The global symbols by the code units of their keys.
struct SymbolsByKey(HashMap<Vec<u16>, Gc<Symbol>>);

// SAFETY: Visits every symbol of the map.
unsafe impl Trace for SymbolsByKey {
    fn trace(&self, visitor: &mut Visitor) {
        for symbol in self.0.values() {
            symbol.trace(visitor);
        }
    }
}

/// The C++ VM::m_global_symbol_registry, the GlobalSymbolRegistry List, https://tc39.es/ecma262/#table-globalsymbolregistry-record-fields.
/// The VM roots it, so the symbols it holds live as long as the VM.
#[repr(C)]
#[derive(Trace)]
pub struct GlobalSymbolRegistry {
    header: CellHeader,
    symbols: GcRefCell<SymbolsByKey>,
}

define_cell!(GlobalSymbolRegistry, Other);

impl GlobalSymbolRegistry {
    pub fn create(vm: &Vm) -> Gc<GlobalSymbolRegistry> {
        vm.heap().allocate(GlobalSymbolRegistry {
            header: CellHeader::for_class(Self::CLASS),
            symbols: GcRefCell::new(SymbolsByKey(HashMap::new())),
        })
    }

    pub fn get(&self, key: &Utf16String) -> Option<Gc<Symbol>> {
        let key: Vec<u16> = Utf16View::of_string(key).code_units().collect();
        self.symbols.borrow().0.get(&key).copied()
    }

    pub fn set(&self, key: &Utf16String, symbol: Gc<Symbol>) {
        let key = Utf16View::of_string(key).code_units().collect();
        self.symbols.borrow_mut().0.insert(key, symbol);
    }
}
