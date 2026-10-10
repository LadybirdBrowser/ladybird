/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Probes of the keyed property lookup caches of instructions and of the
//! VM, like the interpreter's `GetByValue` and `PutByValue` handlers do
//! them for string and symbol keys, and the property tests of
//! `hasOwnProperty` and `in` (`ProbeHasProperty`).

use super::super::Codegen;
use super::megamorphic::EntryShape;
use super::megamorphic::MegamorphicKey;
use super::property::CacheProbe;
use super::property::CacheRegisters;
use super::property::DeferredCacheProbe;
use crate::CompileFailure;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;
use crate::ir::NodeId;
use crate::ir::value;
use crate::snapshot::PropertyCacheKind;

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// Looks up the own data property that the string `key` names on the
    /// object in `base` in the VM's keyed property lookup cache, like the
    /// runtime's keyed get does when the access's own cache has nothing for
    /// it (see `RuntimeLayout::keyed_lookup_cache_entries`): puts it in
    /// `output` and jumps to `done`, or jumps to `miss`. Clobbers the cache
    /// registers and the scratch register.
    fn emit_keyed_lookup_cache_get(
        &mut self,
        base: Gpr,
        key: Gpr,
        registers: CacheRegisters,
        output: Gpr,
        miss: Label,
        done: Label,
    ) {
        let entries = self.runtime.layout.keyed_lookup_cache_entries;
        let entry_type = self.runtime.layout.property_lookup_cache_entry_type_get_own_property;
        let Some(property) = self.emit_vm_keyed_cache_probe(base, key, &registers, entries, entry_type, miss) else {
            return;
        };
        let CacheRegisters { entry, .. } = registers;
        let scratch = self.pinned.scratch;
        self.masm.load64(entry, &property);
        self.branch_on_tag(Condition::Equal, entry, value::ACCESSOR_TAG, scratch, miss);
        self.masm.move64(output, entry);
        self.masm.jump(done);
    }

    /// Stores `source` into the writable own data property that the string
    /// `key` names on the object in `base`, if the VM's keyed property
    /// store cache has it (see `RuntimeLayout::keyed_store_cache_entries`),
    /// and jumps to `done`, or jumps to `miss`. Clobbers the cache registers
    /// and the scratch register.
    fn emit_keyed_store_cache_put(
        &mut self,
        base: Gpr,
        key: Gpr,
        source: Gpr,
        registers: CacheRegisters,
        miss: Label,
        done: Label,
    ) {
        let entries = self.runtime.layout.keyed_store_cache_entries;
        let entry_type = self.runtime.layout.property_lookup_cache_entry_type_change_own_property;
        let Some(property) = self.emit_vm_keyed_cache_probe(base, key, &registers, entries, entry_type, miss) else {
            return;
        };
        let CacheRegisters { entry, .. } = registers;
        self.masm.load64(entry, &property);
        self.branch_on_tag(Condition::Equal, entry, value::ACCESSOR_TAG, entry, miss);
        self.masm.store64(&property, source);
        self.masm.jump(done);
    }

    /// Finds the entry for the string `key` and the shape of the object in
    /// `base` in a VM-wide keyed cache with `entries` (laid out like the
    /// keyed property lookup cache's), with `entry_type` and the shape's
    /// dictionary generation, and returns the address of the named property
    /// at its offset in the object. Branches to `miss` otherwise, and
    /// returns nothing (having jumped to `miss`) if `entries` is 0. Clobbers
    /// the cache registers and the scratch register; the address uses the
    /// shape register and the scratch register.
    fn emit_vm_keyed_cache_probe(
        &mut self,
        base: Gpr,
        key: Gpr,
        registers: &CacheRegisters,
        entries: u64,
        entry_type: u32,
        miss: Label,
    ) -> Option<Address> {
        if !self.emit_vm_keyed_cache_entry(base, key, registers, entries, miss) {
            return None;
        }
        let CacheRegisters { object, shape, .. } = *registers;
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let cache_entry = object;
        self.masm.branch32_memory_imm(
            Condition::NotEqual,
            &Address::new(cache_entry, layout.keyed_lookup_cache_entry_type as i32),
            entry_type as i32,
            miss,
        );
        // The own property at the entry's offset.
        self.masm.load32(
            scratch,
            &Address::new(cache_entry, layout.keyed_lookup_cache_property_offset as i32),
        );
        self.emit_unbox_cell(object, base);
        self.masm.load64(
            shape,
            &Address::new(object, self.runtime.offsets.object_named_properties as i32),
        );
        Some(Address::indexed(shape, scratch, Scale::Eight, 0))
    }

    /// Finds the entry for the string `key` and the shape of the object in
    /// `base` in a VM-wide keyed cache with `entries` (laid out like the
    /// keyed property lookup cache's), for the shape's dictionary
    /// generation, and puts its address in the object register and the
    /// shape in the shape register. Branches to `miss` otherwise, and
    /// returns false (having jumped to `miss`) if `entries` is 0. Clobbers
    /// the cache registers and the scratch register.
    pub(in crate::codegen) fn emit_vm_keyed_cache_entry(
        &mut self,
        base: Gpr,
        key: Gpr,
        registers: &CacheRegisters,
        entries: u64,
        miss: Label,
    ) -> bool {
        let CacheRegisters { object, shape, entry } = *registers;
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        if entries == 0 {
            self.masm.jump(miss);
            return false;
        }
        // The identity of a string's name is the word of its storage, if that is a fly string; no entry has the
        // storage word of another string.
        let name = entry;
        self.branch_on_tag(Condition::NotEqual, key, value::STRING_TAG, scratch, miss);
        self.emit_unbox_cell(name, key);
        self.masm
            .load64(name, &Address::new(name, layout.primitive_string_storage as i32));
        self.emit_unbox_cell(object, base);
        self.masm
            .load64(shape, &Address::new(object, self.runtime.offsets.object_shape as i32));
        // The entry: the top bits of the Fibonacci hash of the shape and the name.
        let entry_size_log2 = layout.keyed_lookup_cache_entry_size.trailing_zeros();
        let bits = layout.keyed_lookup_cache_index_bits;
        self.masm.xor32(scratch, shape, name);
        self.masm.mul32_imm(
            scratch,
            scratch,
            layout.property_lookup_cache_megamorphic_hash_multiplier,
        );
        self.masm
            .shr32_imm(scratch, scratch, (32 - bits - entry_size_log2) as u8);
        self.masm
            .and32_imm(scratch, scratch, ((1u32 << bits) - 1) << entry_size_log2);
        let cache_entry = object;
        self.masm.move_imm64(cache_entry, entries);
        self.masm.add64(cache_entry, cache_entry, scratch);
        self.masm.branch64_memory(
            Condition::NotEqual,
            &Address::new(cache_entry, layout.keyed_lookup_cache_name as i32),
            name,
            miss,
        );
        self.masm.branch64_memory(
            Condition::NotEqual,
            &Address::new(cache_entry, layout.keyed_lookup_cache_shape as i32),
            shape,
            miss,
        );
        let expected = Address::new(cache_entry, layout.keyed_lookup_cache_dictionary_generation as i32);
        self.branch_unless_dictionary_generation_is(shape, expected, [scratch, name], miss);
        true
    }

    /// Loads the entry of property lookup cache `cache_index` of a keyed
    /// access for `key` and the shape of `object` (matching `entry_shape`
    /// of the entries) into `entry`: the most recently used entry, or
    /// another entry of a polymorphic or megamorphic cache. Branches to
    /// `miss` unless there is one, for an own property. Uses `shape` and the
    /// scratch register.
    #[allow(clippy::too_many_arguments)]
    fn probe_keyed_cache_entry(
        &mut self,
        object: Gpr,
        key: Gpr,
        shape: Gpr,
        entry: Gpr,
        executable: u32,
        cache_index: u32,
        entry_shape: EntryShape,
        miss: Label,
    ) -> Result<(), CompileFailure> {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let found = self.masm.new_label();
        let not_polymorphic = self.masm.new_label();
        self.property_cache_address(entry, executable, cache_index)?;
        self.masm.load64(entry, &Address::new(entry, 0));
        let was_megamorphic = self.executables[executable as usize]
            .property_caches
            .get(cache_index as usize)
            .is_some_and(|cache| cache.kind == PropertyCacheKind::Megamorphic);
        if was_megamorphic {
            // NB: Caches that were megamorphic look in their hash tables
            //     first, where the most recently used entry is too.
            let not_megamorphic = self.masm.new_label();
            self.masm.move64(scratch, entry);
            self.masm
                .and64_imm(scratch, scratch, !layout.property_lookup_cache_data_pointer_mask);
            self.masm.branch64_imm(
                Condition::NotEqual,
                scratch,
                layout.property_lookup_cache_megamorphic_tag as i64,
                not_megamorphic,
            );
            self.masm
                .and64_imm(entry, entry, layout.property_lookup_cache_data_pointer_mask);
            self.masm
                .load64(shape, &Address::new(object, self.runtime.offsets.object_shape as i32));
            self.emit_megamorphic_cache_lookup(shape, entry, MegamorphicKey::Keyed { key }, entry_shape, found, miss);
            self.masm.bind(not_megamorphic);
        }
        self.masm.branch_test64(Condition::Zero, entry, u64::MAX, miss);
        self.masm
            .move_imm64(scratch, layout.property_lookup_cache_keyed_generic);
        self.masm.branch64(Condition::Equal, entry, scratch, miss);
        self.masm.move64(scratch, entry);
        self.masm
            .and64_imm(entry, entry, layout.property_lookup_cache_data_pointer_mask);
        self.masm
            .load64(shape, &Address::new(object, self.runtime.offsets.object_shape as i32));
        let entry_key = Address::new(entry, layout.property_lookup_cache_entry_key as i32);
        for index in 0..layout.property_lookup_cache_polymorphic_entry_count {
            let next = self.masm.new_label();
            if index > 0 {
                self.masm
                    .add64_imm(entry, entry, i64::from(layout.property_lookup_cache_entry_size));
            }
            self.masm.branch64_memory(Condition::NotEqual, &entry_key, key, next);
            self.branch_if_entry_shape(entry, shape, entry_shape, found);
            self.masm.bind(next);
            // The other entries of a polymorphic cache follow the most
            // recently used one.
            if index == 0 {
                self.masm
                    .and64_imm(scratch, scratch, !layout.property_lookup_cache_data_pointer_mask);
                self.masm.branch64_imm(
                    Condition::NotEqual,
                    scratch,
                    layout.property_lookup_cache_polymorphic_tag as i64,
                    not_polymorphic,
                );
            }
        }
        self.masm.jump(miss);
        // Megamorphic caches keep the other entries in hash tables.
        self.masm.bind(not_polymorphic);
        self.masm.branch64_imm(
            Condition::NotEqual,
            scratch,
            layout.property_lookup_cache_megamorphic_tag as i64,
            miss,
        );
        self.emit_megamorphic_cache_lookup(shape, entry, MegamorphicKey::Keyed { key }, entry_shape, found, miss);
        self.masm.bind(found);
        let expected = Address::new(entry, layout.property_lookup_cache_entry_dictionary_generation as i32);
        self.branch_unless_dictionary_generation_is(shape, expected, [shape, scratch], miss);
        self.masm.branch64_memory_imm(
            Condition::NotEqual,
            &Address::new(entry, layout.property_lookup_cache_entry_prototype as i32),
            0,
            miss,
        );
        Ok(())
    }

    /// The address of the own named property of `object` that cache entry
    /// `entry` is for. Uses `storage` and the scratch register.
    fn keyed_cache_property_address(&mut self, object: Gpr, entry: Gpr, storage: Gpr) -> Address {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        self.masm.load32(
            scratch,
            &Address::new(entry, layout.property_lookup_cache_entry_property_offset as i32),
        );
        self.masm.load64(
            storage,
            &Address::new(object, self.runtime.offsets.object_named_properties as i32),
        );
        Address::indexed(storage, scratch, Scale::Eight, 0)
    }
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// Where a cache probe branches when it finds nothing: the target of the
    /// branch on its result that follows it, if code generation fused them,
    /// or a new label. Returns whether it is fused.
    pub(in crate::codegen) fn probe_failure_label(&mut self) -> (Label, bool) {
        match self.fused_probe_failure.take() {
            Some(label) => (label, true),
            None => (self.masm.new_label(), false),
        }
    }

    /// Binds `fail` to code that puts `value` into `output`, and `done`
    /// after it, unless the failure branches elsewhere (`fused`).
    pub(in crate::codegen) fn bind_probe_failure(
        &mut self,
        output: Gpr,
        value: u64,
        (fail, fused): (Label, bool),
        done: Label,
    ) {
        if fused {
            self.masm.bind(done);
            return;
        }
        self.masm.jump(done);
        self.masm.bind(fail);
        self.masm.move_imm64(output, value);
        self.masm.bind(done);
    }

    /// A `ProbeKeyedCache` node: an entry of the property lookup cache for
    /// an own data property or a missing property, or the VM's keyed lookup
    /// cache.
    pub(in crate::codegen) fn emit_probe_keyed_cache(
        &mut self,
        node: NodeId,
        executable: u32,
        cache: u32,
    ) -> Result<(), CompileFailure> {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let (base, key, output) = (self.input(node, 0), self.input(node, 1), self.output(node));
        let (object, shape, entry) = (self.temp(node, 0), self.temp(node, 1), self.temp(node, 2));
        let (failure, done) = (self.probe_failure_label(), self.masm.new_label());
        let fail = failure.0;
        self.unbox_object_or_branch(object, base, fail);
        let lookup_cache = self.masm.new_label();
        self.probe_keyed_cache_entry(
            object,
            key,
            shape,
            entry,
            executable,
            cache,
            EntryShape::Shape,
            lookup_cache,
        )?;
        let missing = self.masm.new_label();
        self.masm.branch32_memory_imm(
            Condition::Equal,
            &Address::new(entry, layout.property_lookup_cache_entry_type as i32),
            layout.property_lookup_cache_entry_type_get_missing_property as i32,
            missing,
        );
        let property = self.keyed_cache_property_address(object, entry, shape);
        self.masm.load64(entry, &property);
        self.branch_on_tag(Condition::Equal, entry, value::ACCESSOR_TAG, scratch, fail);
        self.masm.move64(output, entry);
        self.masm.jump(done);

        self.masm.bind(missing);
        self.branch_unless_property_absence_applies(object, shape, entry, fail);
        self.masm.move_imm64(output, value::UNDEFINED);
        self.masm.jump(done);

        self.masm.bind(lookup_cache);
        let registers = CacheRegisters { object, shape, entry };
        self.emit_keyed_lookup_cache_get(base, key, registers, output, fail, done);
        self.bind_probe_failure(output, value::EMPTY, failure, done);
        Ok(())
    }

    /// A `ProbeKeyedStore` node: an entry of the property lookup cache for
    /// a writable own data property or a property addition, or the VM's
    /// keyed store cache.
    pub(in crate::codegen) fn emit_probe_keyed_store(
        &mut self,
        node: NodeId,
        executable: u32,
        cache: u32,
    ) -> Result<(), CompileFailure> {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let (base, key, source) = (self.input(node, 0), self.input(node, 1), self.input(node, 2));
        let output = self.output(node);
        let (object, shape, entry, storage) = (
            self.temp(node, 0),
            self.temp(node, 1),
            self.temp(node, 2),
            self.temp(node, 3),
        );
        let (failure, stored, done) = (self.probe_failure_label(), self.masm.new_label(), self.masm.new_label());
        let fail = failure.0;
        self.unbox_object_or_branch(object, base, fail);
        let store_cache = self.masm.new_label();
        self.probe_keyed_cache_entry(
            object,
            key,
            shape,
            entry,
            executable,
            cache,
            EntryShape::ShapeOrFromShape,
            store_cache,
        )?;
        let add = self.masm.new_label();
        self.masm.branch32_memory_imm(
            Condition::Equal,
            &Address::new(entry, layout.property_lookup_cache_entry_type as i32),
            layout.property_lookup_cache_entry_type_add_own_property as i32,
            add,
        );
        self.masm.load8(
            scratch,
            &Address::new(entry, layout.property_lookup_cache_entry_writes_data_property as i32),
        );
        self.masm.branch_test32(Condition::Zero, scratch, 0xFF, fail);
        // NB: The address uses the scratch register, so the tag check must not.
        let property = self.keyed_cache_property_address(object, entry, shape);
        self.masm.load64(entry, &property);
        self.branch_on_tag(Condition::Equal, entry, value::ACCESSOR_TAG, entry, fail);
        self.masm.store64(&property, source);
        self.masm.jump(stored);

        self.masm.bind(add);
        self.masm
            .load64(shape, &Address::new(object, self.runtime.offsets.object_shape as i32));
        let registers = CacheRegisters { object, shape, entry };
        self.emit_add_own_property(node, &registers, source, storage, fail);
        self.masm.jump(stored);

        self.masm.bind(store_cache);
        self.emit_keyed_store_cache_put(base, key, source, registers, fail, stored);
        self.masm.bind(stored);
        self.masm.move_imm32(output, 1);
        self.bind_probe_failure(output, 0, failure, done);
        Ok(())
    }

    /// A `ProbeHasProperty` node: whether the object in input 0 has the
    /// property the key in input 1 names, as its own (if `own`) or at all,
    /// where the VM can tell without running code. Inline: own properties
    /// the VM's keyed lookup cache has for the shape and a string key,
    /// absent ones of plain objects (which have only the properties of
    /// their shape), and for `in`, elements of packed or holey storage.
    /// The runtime's helper answers out of line otherwise.
    pub(in crate::codegen) fn emit_probe_has_property(&mut self, node: NodeId, own: bool) {
        let (object, key, output) = (self.input(node, 0), self.input(node, 1), self.output(node));
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let registers = CacheRegisters {
            object: self.temp(node, 0),
            shape: self.temp(node, 1),
            entry: self.temp(node, 2),
        };
        let (failure, done) = (self.probe_failure_label(), self.masm.new_label());
        let probe = self.masm.new_label();
        self.deferred_cache_probes.push(DeferredCacheProbe {
            node,
            executable: 0,
            probe: CacheProbe::HasProperty { own },
            entry: probe,
            hit: done,
            miss: failure.0,
        });
        self.branch_on_tag(Condition::NotEqual, object, value::OBJECT_TAG, scratch, failure.0);
        if !own {
            self.emit_has_element(object, key, &registers, output, probe, done);
        }
        if self.emit_vm_keyed_cache_entry(object, key, &registers, layout.keyed_lookup_cache_entries, probe) {
            let (entry, absent) = (registers.object, self.masm.new_label());
            self.masm.branch32_memory_imm(
                Condition::NotEqual,
                &Address::new(entry, layout.keyed_lookup_cache_entry_type as i32),
                layout.property_lookup_cache_entry_type_get_own_property as i32,
                absent,
            );
            self.masm.move_imm64(output, value::TRUE);
            self.masm.jump(done);
            self.masm.bind(absent);
            // NB: Plain objects, which start with the virtual table pointer
            //     of their class like the template of new plain objects, have
            //     the own string-keyed properties of their shapes. Their
            //     prototypes may have it, which `in` leaves to the runtime.
            match self.runtime.object_allocation.template.first() {
                Some(&plain_object_vtable) if own => {
                    self.emit_unbox_cell(entry, object);
                    self.masm.move_imm64(scratch, plain_object_vtable);
                    self.masm
                        .branch64_memory(Condition::NotEqual, &Address::new(entry, 0), scratch, probe);
                    self.masm.move_imm64(output, value::FALSE);
                    self.masm.jump(done);
                }
                _ => self.masm.jump(probe),
            }
        }
        self.bind_probe_failure(output, value::EMPTY, failure, done);
    }

    /// For an int32 `key` that is not negative and the index of an element
    /// that the packed or holey storage of `object` has (no hole), puts true
    /// in `output` and jumps to `done`. Branches to `probe` for other int32
    /// keys, and continues for other keys. Uses the cache registers.
    fn emit_has_element(
        &mut self,
        object: Gpr,
        key: Gpr,
        registers: &CacheRegisters,
        output: Gpr,
        probe: Label,
        done: Label,
    ) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let (address, elements) = (registers.object, registers.shape);
        let not_index = self.masm.new_label();
        self.branch_on_tag(Condition::NotEqual, key, value::INT32_TAG, scratch, not_index);
        self.masm.branch_test32(Condition::NonZero, key, 0x8000_0000, probe);
        self.emit_unbox_cell(address, object);
        self.masm.load16(
            scratch,
            &Address::new(address, self.runtime.offsets.object_flags as i32),
        );
        self.masm.branch_test32(
            Condition::NonZero,
            scratch,
            u32::from(layout.object_flag_is_typed_array | layout.object_flag_may_interfere),
            probe,
        );
        self.masm.load8(
            scratch,
            &Address::new(address, layout.object_indexed_storage_kind as i32),
        );
        let checked_kind = self.masm.new_label();
        self.masm.branch32_imm(
            Condition::Equal,
            scratch,
            i32::from(layout.indexed_storage_kind_packed),
            checked_kind,
        );
        self.masm.branch32_imm(
            Condition::NotEqual,
            scratch,
            i32::from(layout.indexed_storage_kind_holey),
            probe,
        );
        self.masm.bind(checked_kind);
        self.masm.load32(
            scratch,
            &Address::new(address, layout.object_indexed_array_like_size as i32),
        );
        self.masm.branch32(Condition::AboveOrEqual, key, scratch, probe);
        self.masm
            .load64(elements, &Address::new(address, layout.object_indexed_elements as i32));
        self.masm.branch_test64(Condition::Zero, elements, u64::MAX, probe);
        self.masm
            .load32(scratch, &Address::new(elements, layout.indexed_elements_capacity));
        self.masm.branch32(Condition::AboveOrEqual, key, scratch, probe);
        self.masm.move32(scratch, key);
        self.masm
            .load64(scratch, &Address::indexed(elements, scratch, Scale::Eight, 0));
        // NB: Holes of holey storage are empty values, which the runtime
        //     looks up in the prototype chain.
        self.masm
            .branch64_imm(Condition::Equal, scratch, value::EMPTY as i64, probe);
        self.masm.move_imm64(output, value::TRUE);
        self.masm.jump(done);
        self.masm.bind(not_index);
    }
}
