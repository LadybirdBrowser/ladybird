/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Symbol values. Every function runs on the thread that owns the VM.

use crate::embedding::abi_types::{
    JSOwnedUtf16String, JSSymbol, cell_from_abi, cell_into_abi, owned_utf16_string_into_abi, vm_from_abi,
};
use crate::layout::host_class::JSVM;

/// The well-known symbols, numbered in the order of JS_ENUMERATE_WELL_KNOWN_SYMBOLS.
pub type JSWellKnownSymbol = u8;

pub const JS_WELL_KNOWN_SYMBOL_ASYNC_DISPOSE: JSWellKnownSymbol = 0;
pub const JS_WELL_KNOWN_SYMBOL_ASYNC_ITERATOR: JSWellKnownSymbol = 1;
pub const JS_WELL_KNOWN_SYMBOL_DISPOSE: JSWellKnownSymbol = 2;
pub const JS_WELL_KNOWN_SYMBOL_HAS_INSTANCE: JSWellKnownSymbol = 3;
pub const JS_WELL_KNOWN_SYMBOL_IS_CONCAT_SPREADABLE: JSWellKnownSymbol = 4;
pub const JS_WELL_KNOWN_SYMBOL_ITERATOR: JSWellKnownSymbol = 5;
pub const JS_WELL_KNOWN_SYMBOL_MATCH: JSWellKnownSymbol = 6;
pub const JS_WELL_KNOWN_SYMBOL_MATCH_ALL: JSWellKnownSymbol = 7;
pub const JS_WELL_KNOWN_SYMBOL_REPLACE: JSWellKnownSymbol = 8;
pub const JS_WELL_KNOWN_SYMBOL_REPLACE_ALL: JSWellKnownSymbol = 9;
pub const JS_WELL_KNOWN_SYMBOL_SEARCH: JSWellKnownSymbol = 10;
pub const JS_WELL_KNOWN_SYMBOL_SPECIES: JSWellKnownSymbol = 11;
pub const JS_WELL_KNOWN_SYMBOL_SPLIT: JSWellKnownSymbol = 12;
pub const JS_WELL_KNOWN_SYMBOL_TO_PRIMITIVE: JSWellKnownSymbol = 13;
pub const JS_WELL_KNOWN_SYMBOL_TO_STRING_TAG: JSWellKnownSymbol = 14;
pub const JS_WELL_KNOWN_SYMBOL_UNSCOPABLES: JSWellKnownSymbol = 15;

/// SymbolDescriptiveString(symbol), "Symbol(description)", which the caller owns. Call on the VM's thread.
///
/// # Safety
///
/// `symbol` must be a symbol of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_symbol_descriptive_string(symbol: *mut JSSymbol) -> JSOwnedUtf16String {
    // SAFETY: The caller passes a symbol of the VM.
    owned_utf16_string_into_abi(unsafe { cell_from_abi(symbol) }.descriptive_string())
}

/// The VM's well-known symbol `symbol`, which lives as long as the VM. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_symbol_well_known(vm: *mut JSVM, symbol: JSWellKnownSymbol) -> *mut JSSymbol {
    // SAFETY: The caller passes its VM.
    let symbols = unsafe { vm_from_abi(vm) }.well_known_symbols();
    let symbol = match symbol {
        JS_WELL_KNOWN_SYMBOL_ASYNC_DISPOSE => symbols.async_dispose,
        JS_WELL_KNOWN_SYMBOL_ASYNC_ITERATOR => symbols.async_iterator,
        JS_WELL_KNOWN_SYMBOL_DISPOSE => symbols.dispose,
        JS_WELL_KNOWN_SYMBOL_HAS_INSTANCE => symbols.has_instance,
        JS_WELL_KNOWN_SYMBOL_IS_CONCAT_SPREADABLE => symbols.is_concat_spreadable,
        JS_WELL_KNOWN_SYMBOL_ITERATOR => symbols.iterator,
        JS_WELL_KNOWN_SYMBOL_MATCH => symbols.match_,
        JS_WELL_KNOWN_SYMBOL_MATCH_ALL => symbols.match_all,
        JS_WELL_KNOWN_SYMBOL_REPLACE => symbols.replace,
        JS_WELL_KNOWN_SYMBOL_REPLACE_ALL => symbols.replace_all,
        JS_WELL_KNOWN_SYMBOL_SEARCH => symbols.search,
        JS_WELL_KNOWN_SYMBOL_SPECIES => symbols.species,
        JS_WELL_KNOWN_SYMBOL_SPLIT => symbols.split,
        JS_WELL_KNOWN_SYMBOL_TO_PRIMITIVE => symbols.to_primitive,
        JS_WELL_KNOWN_SYMBOL_TO_STRING_TAG => symbols.to_string_tag,
        JS_WELL_KNOWN_SYMBOL_UNSCOPABLES => symbols.unscopables,
        symbol => panic!("the embedder asked for an unknown well-known symbol {symbol}"),
    };
    cell_into_abi(symbol)
}
