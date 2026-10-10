/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Probes of the interpreter's property lookup caches: those of named
//! property accesses, of keyed accesses, and the hash tables of megamorphic
//! caches.

mod keyed;
mod megamorphic;
mod property;

pub(super) use property::CacheRegisters;
pub(super) use property::DeferredCacheProbe;
pub(super) use property::PUT_BY_ID_RECORD_BYTES;

use super::Codegen;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// Branches to `miss` unless the dictionary generation of the shape in
    /// `shape` is the one a cache recorded at `expected`. Loads the two into
    /// `temps`, the first of which may be `shape`.
    pub(super) fn branch_unless_dictionary_generation_is(
        &mut self,
        shape: Gpr,
        expected: Address,
        temps: [Gpr; 2],
        miss: Label,
    ) {
        self.masm.load32(
            temps[0],
            &Address::new(shape, self.runtime.offsets.shape_dictionary_generation as i32),
        );
        self.masm.load32(temps[1], &expected);
        self.masm.branch32(Condition::NotEqual, temps[0], temps[1], miss);
    }

    /// Branches to `miss` unless the prototype chain validity in `validity`
    /// is still valid. Clobbers `validity`.
    pub(super) fn branch_unless_prototype_chain_valid(&mut self, validity: Gpr, miss: Label) {
        self.masm.load8(
            validity,
            &Address::new(validity, self.runtime.offsets.prototype_chain_validity_valid as i32),
        );
        self.masm.branch_test32(Condition::Zero, validity, 0xFF, miss);
    }
}
