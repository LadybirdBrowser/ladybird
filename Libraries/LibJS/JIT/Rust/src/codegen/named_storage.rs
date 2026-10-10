/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Growing the named property storage of objects in JIT code, for property
//! additions the storage has no room for, like
//! `Object::ensure_named_storage_capacity()`: a storage cell of the size
//! class for twice the old capacity (or the new property count, if that is
//! more), allocated per the allocation contract (see `ArrayAllocationInfo`),
//! with the old values copied over and the rest `undefined`. Storage cells
//! never start a collection, so nothing can collect between the allocation
//! and the store of the new shape.
//!
//! Every addition that may need it calls one subroutine out of line, which
//! keeps every register but the scratch register, in which it returns
//! whether it grew the storage. It gives up, and the addition takes its
//! slow path, on malloc storage, on capacities beyond the largest storage
//! cell, and on empty free lists.

use super::Codegen;
use super::probes::CacheRegisters;
use super::slow_path_calls::slow_path_saved_registers;
use crate::asm::Address;
use crate::asm::Architecture;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;
use crate::ir::NodeId;
use crate::ir::value;

/// The registers the subroutine works with, which it saves in the slow
/// path save area after its two inputs.
const WORK_REGISTER_COUNT: usize = 5;

/// The out of line part of a property addition that grows the storage.
pub(super) struct DeferredStorageGrowth {
    node: NodeId,
    entry: Label,
    /// Where the addition continues once the storage has room.
    resume: Label,
    miss: Label,
    object: Gpr,
    /// Holds the property count of the new shape.
    needed: Gpr,
    /// The cache entry, whose new shape the addition reloads.
    cache_entry: Gpr,
    shape: Gpr,
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    fn growth_registers() -> Option<Vec<Gpr>> {
        let registers = slow_path_saved_registers::<M>().iter().collect::<Vec<_>>();
        (registers.len() >= WORK_REGISTER_COUNT + 2).then_some(registers)
    }

    /// Whether JIT code can grow named property storage itself.
    fn can_grow_named_storage(&self) -> bool {
        let info = &self.runtime.array_allocation;
        Self::growth_registers().is_some()
            && info.allocator != 0
            && !info.storage_size_classes.is_empty()
            && info.storage_values_offset as usize == 8 * info.storage_template.len()
            && info.storage_capacity_offset + 8 == info.storage_values_offset
            && i64::from(self.runtime.layout.named_properties_capacity) == -8
            && info
                .storage_size_classes
                .iter()
                .all(|size_class| size_class.cell_size == info.storage_values_offset + 8 * size_class.capacity)
    }

    /// The label that grows the named property storage of the object in
    /// `registers.object` to hold `needed` properties and continues at
    /// `resume`, with the new shape of `registers.entry` in
    /// `registers.shape`, or goes to `miss`. Or `miss` itself if JIT code
    /// cannot grow storage.
    pub(super) fn storage_growth(
        &mut self,
        node: NodeId,
        registers: &CacheRegisters,
        needed: Gpr,
        resume: Label,
        miss: Label,
    ) -> Label {
        if !self.can_grow_named_storage() {
            return miss;
        }
        let entry = self.masm.new_label();
        self.deferred_storage_growths.push(DeferredStorageGrowth {
            node,
            entry,
            resume,
            miss,
            object: registers.object,
            needed,
            cache_entry: registers.entry,
            shape: registers.shape,
        });
        entry
    }

    pub(super) fn emit_deferred_storage_growths(&mut self) {
        let growths = std::mem::take(&mut self.deferred_storage_growths);
        if growths.is_empty() {
            return;
        }
        let registers = Self::growth_registers().expect("growth needs its registers");
        let subroutine = self.masm.new_label();
        let layout = self.runtime.layout;
        for growth in growths {
            self.annotate(super::CodeAnnotation::Deferred(growth.node));
            self.masm.bind(growth.entry);
            let (object_input, needed_input) = (self.growth_save_address(0, 0), self.growth_save_address(1, 0));
            self.masm.store64(&object_input, growth.object);
            self.masm.store64(&needed_input, growth.needed);
            self.masm.call(subroutine);
            self.masm
                .branch_test32(Condition::Zero, self.pinned.scratch, u32::MAX, growth.miss);
            self.masm.load64(
                growth.shape,
                &Address::new(growth.cache_entry, layout.property_lookup_cache_entry_shape as i32),
            );
            self.masm.jump(growth.resume);
        }
        self.annotate(super::CodeAnnotation::Tail("grow named property storage"));
        self.masm.bind(subroutine);
        self.emit_storage_growth_subroutine(&registers);
    }

    /// Slot `index` of the slow path save area, as the subroutine (which
    /// runs with `return_address_bytes` more on the stack on some targets)
    /// or its callers see it.
    fn growth_save_address(&self, index: u32, return_address_bytes: u32) -> Address {
        self.local_address(self.locals.saves + 8 * index + return_address_bytes)
    }

    fn emit_storage_growth_subroutine(&mut self, registers: &[Gpr]) {
        let return_address_bytes = match M::ARCHITECTURE {
            Architecture::X86_64 => 8,
            Architecture::AArch64 => 0,
        };
        let info = self.runtime.array_allocation.clone();
        let objects = self.runtime.object_allocation.clone();
        let offsets = self.runtime.offsets;
        let scratch = self.pinned.scratch;
        let at = |base: Gpr, offset: u32| Address::new(base, offset as i32);
        let values =
            |base: Gpr, index: Gpr| Address::indexed(base, index, Scale::Eight, info.storage_values_offset as i32);
        let work = &registers[..WORK_REGISTER_COUNT];
        let (object, storage, old_capacity, capacity, cell) = (work[0], work[1], work[2], work[3], work[4]);
        let save = |index: usize| 2 + index as u32;

        for (index, register) in work.iter().enumerate() {
            let address = self.growth_save_address(save(index), return_address_bytes);
            self.masm.store64(&address, *register);
        }
        let fail = self.masm.new_label();
        let done = self.masm.new_label();

        // The capacity of the old storage, inline or a storage cell.
        let heap_storage = self.masm.new_label();
        let have_capacity = self.masm.new_label();
        self.masm
            .load64(object, &self.growth_save_address(0, return_address_bytes));
        self.masm.load64(storage, &at(object, offsets.object_named_properties));
        self.masm
            .load_effective_address(scratch, &at(object, objects.inline_storage_offset));
        self.masm.branch64(Condition::NotEqual, storage, scratch, heap_storage);
        self.masm
            .load8(old_capacity, &at(object, objects.inline_capacity_offset));
        self.masm.jump(have_capacity);
        self.masm.bind(heap_storage);
        // NB: The header right before the values holds the capacity and the
        //     kind, which must be a cell's: malloc storage is freed when
        //     replaced.
        let capacity_offset = self.runtime.layout.named_properties_capacity;
        let cell_kind = info.storage_template[info.storage_capacity_offset as usize / 8] >> 32;
        self.masm.load32(old_capacity, &Address::new(storage, capacity_offset));
        self.masm.load32(scratch, &Address::new(storage, capacity_offset + 4));
        self.masm
            .branch32_imm(Condition::NotEqual, scratch, cell_kind as i32, fail);
        self.masm.bind(have_capacity);

        // The size class for twice the old capacity, or the new property
        // count if that is more.
        self.masm
            .load32(capacity, &self.growth_save_address(1, return_address_bytes));
        self.masm.add32(scratch, old_capacity, old_capacity);
        let at_least_needed = self.masm.new_label();
        self.masm
            .branch32(Condition::AboveOrEqual, capacity, scratch, at_least_needed);
        self.masm.move32(capacity, scratch);
        self.masm.bind(at_least_needed);
        let found = self.masm.new_label();
        for size_class in &info.storage_size_classes {
            let next = self.masm.new_label();
            self.masm
                .branch32_imm(Condition::Above, capacity, size_class.capacity as i32, next);
            self.masm.move_imm32(capacity, size_class.capacity);
            self.masm.move_imm64(scratch, size_class.allocator);
            self.masm.jump(found);
            self.masm.bind(next);
        }
        self.masm.jump(fail);
        self.masm.bind(found);

        // The cell, off the size class's local free list.
        self.emit_pop_free_list_of(scratch, cell, storage, fail);
        // NB: Storage cells only count towards the heap's total.
        self.masm.move32(object, capacity);
        self.masm.shl64_imm(object, object, 3);
        self.masm
            .add64_imm(object, object, i64::from(info.storage_values_offset));
        self.masm.move_imm64(scratch, objects.heap);
        let total = at(scratch, objects.heap_total_allocated_bytes_offset);
        self.masm.load64(storage, &total);
        self.masm.add64(storage, storage, object);
        self.masm.store64(&total, storage);

        // The header, then the values: undefined beyond the old ones.
        for (index, word) in info.storage_template.iter().enumerate() {
            self.masm.store_imm64(&at(cell, 8 * index as u32), *word);
        }
        self.masm.store32(&at(cell, info.storage_capacity_offset), capacity);
        let fill = self.masm.new_label();
        let filled = self.masm.new_label();
        self.masm.bind(fill);
        self.masm
            .branch32(Condition::BelowOrEqual, capacity, old_capacity, filled);
        self.masm.sub32_imm(capacity, capacity, 1);
        self.masm.store_imm64(&values(cell, capacity), value::UNDEFINED);
        self.masm.jump(fill);
        self.masm.bind(filled);
        self.masm
            .load64(object, &self.growth_save_address(0, return_address_bytes));
        self.masm.load64(storage, &at(object, offsets.object_named_properties));
        let copy = self.masm.new_label();
        let copied = self.masm.new_label();
        self.masm.bind(copy);
        self.masm.branch_test32(Condition::Zero, old_capacity, u32::MAX, copied);
        self.masm.sub32_imm(old_capacity, old_capacity, 1);
        self.masm
            .load64(scratch, &Address::indexed(storage, old_capacity, Scale::Eight, 0));
        self.masm.store64(&values(cell, old_capacity), scratch);
        self.masm.jump(copy);
        self.masm.bind(copied);
        self.masm
            .load_effective_address(storage, &at(cell, info.storage_values_offset));
        self.masm.store64(&at(object, offsets.object_named_properties), storage);
        self.masm.move_imm32(scratch, 1);
        self.masm.jump(done);

        self.masm.bind(fail);
        self.masm.move_imm32(scratch, 0);
        self.masm.bind(done);
        for (index, register) in work.iter().enumerate() {
            let address = self.growth_save_address(save(index), return_address_bytes);
            self.masm.load64(*register, &address);
        }
        self.masm.ret();
    }
}
