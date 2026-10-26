/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Inline lookups in the hash tables of megamorphic property lookup caches,
//! like `PropertyLookupCache::entries_for_shape()`: the entry for a shape
//! (and, for keyed accesses, a key) is in the primary table at the top bits
//! of their 32-bit Fibonacci hash, or in the secondary table at the next
//! bits.

use super::super::Codegen;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;

/// What the entries of a megamorphic cache are for.
#[derive(Debug, Clone, Copy)]
pub(super) enum MegamorphicKey {
    /// A named access: entries have no key.
    Named,
    /// A keyed access with the encoded key `Value` in `key`: entries have
    /// that key, which the hash mixes in.
    Keyed { key: Gpr },
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// Branches to `found` if cache entry `candidate` applies to objects of
    /// the shape in `shape`.
    pub(super) fn branch_if_entry_shape(&mut self, candidate: Gpr, shape: Gpr, found: Label) {
        let candidate_shape = Address::new(candidate, self.runtime.layout.property_lookup_cache_entry_shape as i32);
        self.masm
            .branch64_memory(Condition::Equal, &candidate_shape, shape, found);
    }

    /// Looks up the entry for the shape in `shape` (and the key, if keyed)
    /// in the megamorphic cache data that `entry` points to, and branches to
    /// `found` with the entry in `entry`, or to `miss`. Clobbers the scratch
    /// register.
    pub(super) fn emit_megamorphic_cache_lookup(
        &mut self,
        shape: Gpr,
        entry: Gpr,
        key: MegamorphicKey,
        found: Label,
        miss: Label,
    ) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let bits = layout.property_lookup_cache_megamorphic_index_bits;
        let entry_size = layout.property_lookup_cache_entry_size;
        assert!(entry_size.is_power_of_two(), "megamorphic entries are found by masking");
        let entry_size_log2 = entry_size.trailing_zeros();
        let tables = [
            (1, layout.property_lookup_cache_megamorphic_primary_entries),
            (2, layout.property_lookup_cache_megamorphic_secondary_entries),
        ];
        for (level, table) in tables {
            // The byte offset of the entry in the table: the index (the
            // level-th `bits` from the top of the hash) times the entry size.
            match key {
                MegamorphicKey::Named => self.masm.move32(scratch, shape),
                MegamorphicKey::Keyed { key } => self.masm.xor32(scratch, shape, key),
            }
            self.masm.mul32_imm(
                scratch,
                scratch,
                layout.property_lookup_cache_megamorphic_hash_multiplier,
            );
            let shift = 32 - level * bits - entry_size_log2;
            self.masm.shr32_imm(scratch, scratch, shift as u8);
            self.masm
                .and32_imm(scratch, scratch, ((1u32 << bits) - 1) << entry_size_log2);
            let candidate = Address::indexed(entry, scratch, Scale::One, table as i32);
            let next = self.masm.new_label();
            if let MegamorphicKey::Keyed { key } = key {
                let candidate_key = Address {
                    displacement: candidate.displacement + layout.property_lookup_cache_entry_key as i32,
                    ..candidate
                };
                self.masm
                    .branch64_memory(Condition::NotEqual, &candidate_key, key, next);
            }
            let candidate_shape = Address {
                displacement: candidate.displacement + layout.property_lookup_cache_entry_shape as i32,
                ..candidate
            };
            self.masm
                .branch64_memory(Condition::NotEqual, &candidate_shape, shape, next);
            self.masm.load_effective_address(entry, &candidate);
            self.masm.jump(found);
            self.masm.bind(next);
        }
        self.masm.jump(miss);
    }
}
