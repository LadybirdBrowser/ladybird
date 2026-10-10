/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Probes of the interpreter's property lookup caches, like its `GetById`
//! and `PutById` handlers do them: the most recently used cache entry (and
//! the entries of polymorphic and megamorphic caches) inline, the other
//! entries out of line by the runtime's cache probe.

use super::super::Codegen;
use super::super::checked_i32;
use super::super::slow_path_calls::slow_path_saved_registers;
use super::megamorphic::EntryShape;
use super::megamorphic::MegamorphicKey;
use crate::CompileFailure;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;
use crate::ir::NodeId;
use crate::ir::value;
use crate::regalloc::Location;
use crate::snapshot::PropertyCacheKind;

/// A probe of all entries of a property lookup cache by the runtime.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(in crate::codegen) enum CacheProbe {
    GetById {
        cache: u32,
    },
    PutById,
    /// The runtime's `hasOwnProperty` (if `own`) or `in` helper, which
    /// takes the object in input 0 and the key in input 1.
    HasProperty {
        own: bool,
    },
}

/// A runtime probe of a cache probe node, out of line: from `entry`, it
/// continues at `hit` if the runtime's probe did what the node does, and at
/// `miss` otherwise.
pub(in crate::codegen) struct DeferredCacheProbe {
    pub(in crate::codegen) node: NodeId,
    pub(in crate::codegen) executable: u32,
    pub(in crate::codegen) probe: CacheProbe,
    pub(in crate::codegen) entry: Label,
    pub(in crate::codegen) hit: Label,
    pub(in crate::codegen) miss: Label,
}

/// The bytes of the record the `PutById` cache probe takes: the base and
/// the value.
pub(in crate::codegen) const PUT_BY_ID_RECORD_BYTES: u32 = 16;

/// The registers of a cache fast path.
pub(in crate::codegen) struct CacheRegisters {
    pub(in crate::codegen) object: Gpr,
    pub(in crate::codegen) shape: Gpr,
    pub(in crate::codegen) entry: Gpr,
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    fn cache_registers(&self, node: NodeId) -> CacheRegisters {
        CacheRegisters {
            object: self.temp(node, 0),
            shape: self.temp(node, 1),
            entry: self.temp(node, 2),
        }
    }

    /// Puts the address of property lookup cache `cache_index` of
    /// executable `executable` (an index into `Snapshot::executables`) in
    /// `dst`. The compiled function's is the running frame's; inlined
    /// callees' are constants.
    pub(super) fn property_cache_address(
        &mut self,
        dst: Gpr,
        executable: u32,
        cache_index: u32,
    ) -> Result<(), CompileFailure> {
        if executable == 0 {
            let running = self.frame_field(self.runtime.offsets.execution_context_executable);
            self.masm.load64(dst, &running);
        } else {
            self.masm.move_imm64(dst, self.executables[executable as usize].cell.0);
        }
        self.masm.load64(
            dst,
            &Address::new(dst, self.runtime.layout.executable_property_lookup_caches as i32),
        );
        if cache_index != 0 {
            self.masm
                .add64_imm(dst, dst, i64::from(checked_i32(8 * u64::from(cache_index))?));
        }
        Ok(())
    }

    /// Loads the shape of the object in `registers.object` and the entry of
    /// property lookup cache `cache_index` for that shape into the entry
    /// register: the most recently used entry, or another entry of a
    /// polymorphic or megamorphic cache, matching `entry_shape` of the
    /// entries. Branches to `miss` unless there is one for the shape and its
    /// dictionary generation. Leaves the shape register free. Caches that
    /// were megamorphic when the snapshot was taken look in their hash
    /// tables first.
    fn probe_cache_entry(
        &mut self,
        registers: &CacheRegisters,
        executable: u32,
        cache_index: u32,
        entry_shape: EntryShape,
        miss: Label,
    ) -> Result<(), CompileFailure> {
        let layout = self.runtime.layout;
        let scratch = self.pinned.scratch;
        let CacheRegisters { object, shape, entry } = *registers;
        let found = self.masm.new_label();
        self.masm
            .load64(shape, &Address::new(object, self.runtime.offsets.object_shape as i32));
        self.property_cache_address(entry, executable, cache_index)?;
        self.masm.load64(entry, &Address::new(entry, 0));
        let was_megamorphic = self.executables[executable as usize]
            .property_caches
            .get(cache_index as usize)
            .is_some_and(|cache| cache.kind == PropertyCacheKind::Megamorphic);
        if was_megamorphic {
            // NB: The most recently used entry is also in the tables.
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
            self.emit_megamorphic_cache_lookup(shape, entry, MegamorphicKey::Named, entry_shape, found, miss);
            self.masm.bind(not_megamorphic);
        }
        self.masm.branch_test64(Condition::Zero, entry, u64::MAX, miss);
        self.masm.move64(scratch, entry);
        self.masm
            .and64_imm(entry, entry, layout.property_lookup_cache_data_pointer_mask);
        self.branch_if_entry_shape(entry, shape, entry_shape, found);
        // The other entries of a polymorphic cache follow the most recently used one.
        self.masm
            .and64_imm(scratch, scratch, !layout.property_lookup_cache_data_pointer_mask);
        let not_polymorphic = self.masm.new_label();
        self.masm.branch64_imm(
            Condition::NotEqual,
            scratch,
            layout.property_lookup_cache_polymorphic_tag as i64,
            not_polymorphic,
        );
        for _ in 1..layout.property_lookup_cache_polymorphic_entry_count {
            self.masm
                .add64_imm(entry, entry, i64::from(layout.property_lookup_cache_entry_size));
            self.branch_if_entry_shape(entry, shape, entry_shape, found);
        }
        self.masm.jump(miss);
        // Megamorphic caches keep the other shapes' entries in hash tables.
        self.masm.bind(not_polymorphic);
        self.masm.branch64_imm(
            Condition::NotEqual,
            scratch,
            layout.property_lookup_cache_megamorphic_tag as i64,
            miss,
        );
        self.emit_megamorphic_cache_lookup(shape, entry, MegamorphicKey::Named, entry_shape, found, miss);
        self.masm.bind(found);
        self.branch_unless_dictionary_generation_matches(shape, entry, miss);
        Ok(())
    }

    /// Branches to `miss` unless the dictionary generation of the shape in
    /// `shape` is the cache entry's. Clobbers `shape`.
    fn branch_unless_dictionary_generation_matches(&mut self, shape: Gpr, entry: Gpr, miss: Label) {
        let expected = Address::new(
            entry,
            self.runtime.layout.property_lookup_cache_entry_dictionary_generation as i32,
        );
        self.branch_unless_dictionary_generation_is(shape, expected, [shape, self.pinned.scratch], miss);
    }

    /// The address of the named property of `holder` the cache entry is for.
    /// Uses the shape register and the scratch register.
    fn cached_property_address(&mut self, registers: &CacheRegisters, holder: Gpr) -> Address {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        self.masm.load32(
            scratch,
            &Address::new(
                registers.entry,
                layout.property_lookup_cache_entry_property_offset as i32,
            ),
        );
        self.masm.load64(
            registers.shape,
            &Address::new(holder, self.runtime.offsets.object_named_properties as i32),
        );
        Address::indexed(registers.shape, scratch, Scale::Eight, 0)
    }

    /// Branches to `miss` unless the `GetMissingProperty` entry in `entry`
    /// for the shape of `object` applies to it, like
    /// `get_with_property_lookup_cache()`: only plain objects and ECMAScript
    /// functions (for the names their entries can be for) have no other
    /// properties than those of their shape (see
    /// `Object::is_cacheable_for_property_absence()`), and the property
    /// must also be missing from an unchanged prototype chain. Clobbers
    /// `shape`.
    pub(super) fn branch_unless_property_absence_applies(&mut self, object: Gpr, shape: Gpr, entry: Gpr, miss: Label) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let offsets = self.runtime.offsets;
        // NB: Plain objects start with the virtual table pointer of their
        //     class, like the template of new plain objects.
        let Some(&plain_object_vtable) = self.runtime.object_allocation.template.first() else {
            self.masm.jump(miss);
            return;
        };
        let cacheable = self.masm.new_label();
        self.masm.move_imm64(scratch, plain_object_vtable);
        self.masm
            .branch64_memory(Condition::Equal, &Address::new(object, 0), scratch, cacheable);
        let function_flag = self.runtime.dynamic_calls.object_flag_is_ecmascript_function;
        if function_flag == 0 {
            self.masm.jump(miss);
        } else {
            self.masm
                .load16(scratch, &Address::new(object, offsets.object_flags as i32));
            self.masm
                .branch_test32(Condition::Zero, scratch, u32::from(function_flag), miss);
        }
        self.masm.bind(cacheable);
        let (has_chain, done) = (self.masm.new_label(), self.masm.new_label());
        self.masm.load64(
            shape,
            &Address::new(
                entry,
                layout.property_lookup_cache_entry_prototype_chain_validity as i32,
            ),
        );
        self.masm.branch_test64(Condition::NonZero, shape, u64::MAX, has_chain);
        // Entries without a prototype chain validity are for shapes without
        // a prototype.
        self.masm
            .load64(shape, &Address::new(object, offsets.object_shape as i32));
        self.masm.branch64_memory_imm(
            Condition::NotEqual,
            &Address::new(shape, layout.shape_prototype as i32),
            0,
            miss,
        );
        self.masm.jump(done);
        self.masm.bind(has_chain);
        self.branch_unless_prototype_chain_valid(shape, miss);
        self.masm.bind(done);
    }

    /// A `ProbePropertyCache` node: the data property, or `undefined` for a
    /// missing property, that the entry of the cache for the shape of the
    /// object (or for strings, numbers and booleans, of their prototype)
    /// has, like the interpreter's `GetById` handler finds it. The runtime
    /// probes the entries out of line where the inline probe finds none.
    pub(in crate::codegen) fn emit_probe_property_cache(
        &mut self,
        node: NodeId,
        executable: u32,
        cache_index: u32,
    ) -> Result<(), CompileFailure> {
        let output = self.output(node);
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let registers = self.cache_registers(node);
        let CacheRegisters { object, shape, entry } = registers;
        let (failure, done) = (self.probe_failure_label(), self.masm.new_label());
        let fail = failure.0;
        let probe = self.masm.new_label();
        self.deferred_cache_probes.push(DeferredCacheProbe {
            node,
            executable,
            probe: CacheProbe::GetById { cache: cache_index },
            entry: probe,
            hit: done,
            miss: fail,
        });

        let base = self.input(node, 0);
        self.masm.shr64_imm(scratch, base, value::TAG_SHIFT);
        let snapshot = &self.executables[executable as usize];
        let prototypes = [
            (value::STRING_TAG, snapshot.string_prototype),
            (value::INT32_TAG, snapshot.number_prototype),
            (value::BOOLEAN_TAG, snapshot.boolean_prototype),
        ]
        .into_iter()
        .filter_map(|(tag, prototype)| Some((tag, prototype?, self.masm.new_label())))
        .collect::<Vec<_>>();
        let (is_object, have_object) = (self.masm.new_label(), self.masm.new_label());
        self.masm
            .branch32_imm(Condition::Equal, scratch, i32::from(value::OBJECT_TAG), is_object);
        for (tag, _, label) in &prototypes {
            self.masm
                .branch32_imm(Condition::Equal, scratch, i32::from(*tag), *label);
        }
        // Doubles are numbers too.
        if let Some((_, _, number)) = prototypes.iter().find(|(tag, ..)| *tag == value::INT32_TAG) {
            self.masm.and32_imm(scratch, scratch, u32::from(value::BASE_TAG));
            self.masm
                .branch32_imm(Condition::NotEqual, scratch, i32::from(value::BASE_TAG), *number);
        }
        self.masm.jump(fail);
        for (_, prototype, label) in prototypes {
            self.masm.bind(label);
            self.masm.move_imm64(object, prototype.0);
            self.masm.jump(have_object);
        }
        self.masm.bind(is_object);
        self.emit_unbox_cell(object, base);
        self.masm.bind(have_object);
        self.probe_cache_entry(&registers, executable, cache_index, EntryShape::Shape, probe)?;
        let present = self.masm.new_label();
        self.masm.branch32_memory_imm(
            Condition::NotEqual,
            &Address::new(entry, layout.property_lookup_cache_entry_type as i32),
            layout.property_lookup_cache_entry_type_get_missing_property as i32,
            present,
        );
        self.branch_unless_property_absence_applies(object, shape, entry, probe);
        self.masm.move_imm64(output, value::UNDEFINED);
        self.masm.jump(done);

        // Properties found in a prototype need its chain to be unchanged.
        self.masm.bind(present);
        let own = self.masm.new_label();
        self.masm.load64(
            scratch,
            &Address::new(entry, layout.property_lookup_cache_entry_prototype as i32),
        );
        self.masm.branch_test64(Condition::Zero, scratch, u64::MAX, own);
        self.masm.load64(
            shape,
            &Address::new(
                entry,
                layout.property_lookup_cache_entry_prototype_chain_validity as i32,
            ),
        );
        self.masm.branch_test64(Condition::Zero, shape, u64::MAX, probe);
        self.branch_unless_prototype_chain_valid(shape, probe);
        self.masm.move64(object, scratch);
        self.masm.bind(own);

        // NB: Accessors take the slow path, which calls them.
        let property = self.cached_property_address(&registers, object);
        self.masm.load64(entry, &property);
        self.branch_on_tag(Condition::Equal, entry, value::ACCESSOR_TAG, self.pinned.scratch, fail);
        self.masm.move64(output, entry);
        self.bind_probe_failure(output, value::EMPTY, failure, done);
        Ok(())
    }

    /// A `ProbePropertyStore` node: stores like the interpreter's `PutById`
    /// handler, and also adds properties through `AddOwnProperty` entries.
    /// The most recently used entry is checked first; the entry for the
    /// object's shape is found among the others (see `probe_cache_entry()`)
    /// otherwise. The runtime probes the entries out of line where the
    /// inline probe finds none, except in inlined callees, whose caches are
    /// not the running frame's.
    pub(in crate::codegen) fn emit_probe_property_store(
        &mut self,
        node: NodeId,
        executable: u32,
        cache_index: u32,
    ) -> Result<(), CompileFailure> {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let (base, source, output) = (self.input(node, 0), self.input(node, 1), self.output(node));
        let registers = self.cache_registers(node);
        let CacheRegisters { object, shape, entry } = registers;
        let (failure, stored, done) = (self.probe_failure_label(), self.masm.new_label(), self.masm.new_label());
        let (others, change, transition) = (self.masm.new_label(), self.masm.new_label(), self.masm.new_label());
        let probe = if executable == 0 {
            let probe = self.masm.new_label();
            self.deferred_cache_probes.push(DeferredCacheProbe {
                node,
                executable,
                probe: CacheProbe::PutById,
                entry: probe,
                hit: stored,
                miss: failure.0,
            });
            probe
        } else {
            failure.0
        };

        self.unbox_object_or_branch(object, base, failure.0);
        self.masm
            .load64(shape, &Address::new(object, self.runtime.offsets.object_shape as i32));
        self.property_cache_address(entry, executable, cache_index)?;
        self.masm.load64(entry, &Address::new(entry, 0));
        self.masm.branch_test64(Condition::Zero, entry, u64::MAX, probe);
        self.masm
            .and64_imm(entry, entry, layout.property_lookup_cache_data_pointer_mask);
        let most_recent_matches = self.masm.new_label();
        self.masm.branch64_memory(
            Condition::Equal,
            &Address::new(entry, layout.property_lookup_cache_entry_shape as i32),
            shape,
            most_recent_matches,
        );
        self.masm.branch64_memory(
            Condition::Equal,
            &Address::new(entry, layout.property_lookup_cache_entry_from_shape as i32),
            shape,
            transition,
        );
        self.masm.jump(others);
        self.masm.bind(most_recent_matches);
        self.branch_unless_dictionary_generation_matches(shape, entry, probe);
        // Own data properties.
        self.masm.bind(change);
        self.masm.branch64_memory_imm(
            Condition::NotEqual,
            &Address::new(entry, layout.property_lookup_cache_entry_prototype as i32),
            0,
            probe,
        );
        self.masm.load8(
            scratch,
            &Address::new(entry, layout.property_lookup_cache_entry_writes_data_property as i32),
        );
        self.masm.branch_test32(Condition::Zero, scratch, 0xFF, probe);
        // NB: The address uses the scratch register, so the tag check must not.
        let property = self.cached_property_address(&registers, object);
        self.masm.load64(entry, &property);
        self.branch_on_tag(Condition::Equal, entry, value::ACCESSOR_TAG, entry, probe);
        self.masm.store64(&property, source);
        self.masm.jump(stored);

        // The entry for the object's shape, or the addition to it.
        self.masm.bind(others);
        self.probe_cache_entry(&registers, executable, cache_index, EntryShape::ShapeOrFromShape, probe)?;
        self.masm
            .load64(shape, &Address::new(object, self.runtime.offsets.object_shape as i32));
        self.masm.branch32_memory_imm(
            Condition::NotEqual,
            &Address::new(entry, layout.property_lookup_cache_entry_type as i32),
            layout.property_lookup_cache_entry_type_add_own_property as i32,
            change,
        );
        self.masm.bind(transition);
        let storage = self.temp(node, 3);
        self.emit_add_own_property(node, &registers, source, storage, probe);
        self.masm.bind(stored);
        self.masm.move_imm32(output, 1);
        self.bind_probe_failure(output, 0, failure, done);
        Ok(())
    }

    /// Adds the property of the `AddOwnProperty` entry in `registers.entry`
    /// to the object in `registers.object`, whose shape is in
    /// `registers.shape`, like `asm_try_put_by_id_cache()` does: the object
    /// takes the entry's new shape, and `source` goes in the new property's
    /// slot. Storage that must grow grows out of line (see
    /// `named_storage`), or the addition is left to `miss`. Uses `storage`
    /// and clobbers the shape register.
    pub(super) fn emit_add_own_property(
        &mut self,
        node: NodeId,
        registers: &CacheRegisters,
        source: Gpr,
        storage: Gpr,
        miss: Label,
    ) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let offsets = self.runtime.offsets;
        let CacheRegisters { object, shape, entry } = *registers;

        self.masm.branch32_memory_imm(
            Condition::NotEqual,
            &Address::new(entry, layout.property_lookup_cache_entry_type as i32),
            layout.property_lookup_cache_entry_type_add_own_property as i32,
            miss,
        );
        self.masm.branch64_memory(
            Condition::NotEqual,
            &Address::new(entry, layout.property_lookup_cache_entry_from_shape as i32),
            shape,
            miss,
        );
        // Extensible objects that may cache property additions, without a
        // magical length (whose additions depend on the key).
        let flags = layout.object_flag_is_extensible
            | layout.object_flag_may_interfere
            | layout.object_flag_requires_slow_add_own_property
            | layout.object_flag_has_magical_length;
        self.masm
            .load16(scratch, &Address::new(object, offsets.object_flags as i32));
        self.masm.and32_imm(scratch, scratch, u32::from(flags));
        self.masm.branch32_imm(
            Condition::NotEqual,
            scratch,
            i32::from(layout.object_flag_is_extensible),
            miss,
        );
        // NB: Only additions that make dictionary shapes need the dictionary
        //     generations to match, but requiring it of all of them is safe.
        self.branch_unless_dictionary_generation_matches(shape, entry, miss);
        let valid_chain = self.masm.new_label();
        self.masm.load64(
            shape,
            &Address::new(
                entry,
                layout.property_lookup_cache_entry_prototype_chain_validity as i32,
            ),
        );
        self.masm.branch_test64(Condition::Zero, shape, u64::MAX, valid_chain);
        self.branch_unless_prototype_chain_valid(shape, miss);
        self.masm.bind(valid_chain);
        self.masm.load64(
            shape,
            &Address::new(entry, layout.property_lookup_cache_entry_shape as i32),
        );
        self.masm.branch_test64(Condition::Zero, shape, u64::MAX, miss);

        // The capacity of the inline or heap storage must hold every
        // property of the new shape.
        let heap_storage = self.masm.new_label();
        let check_capacity = self.masm.new_label();
        self.masm
            .load64(storage, &Address::new(object, offsets.object_named_properties as i32));
        self.masm.load_effective_address(
            scratch,
            &Address::new(object, self.runtime.object_allocation.inline_storage_offset as i32),
        );
        self.masm.branch64(Condition::NotEqual, storage, scratch, heap_storage);
        self.masm.load8(
            scratch,
            &Address::new(object, self.runtime.object_allocation.inline_capacity_offset as i32),
        );
        self.masm.jump(check_capacity);
        self.masm.bind(heap_storage);
        self.masm
            .load32(scratch, &Address::new(storage, layout.named_properties_capacity));
        self.masm.bind(check_capacity);
        self.masm
            .load32(storage, &Address::new(shape, layout.shape_property_count as i32));
        let has_room = self.masm.new_label();
        let grow = self.storage_growth(node, registers, storage, has_room, miss);
        self.masm.branch32(Condition::Above, storage, scratch, grow);

        self.masm.bind(has_room);
        self.masm
            .store64(&Address::new(object, offsets.object_shape as i32), shape);
        self.masm.load32(
            scratch,
            &Address::new(entry, layout.property_lookup_cache_entry_property_offset as i32),
        );
        self.masm
            .load64(storage, &Address::new(object, offsets.object_named_properties as i32));
        self.masm
            .store64(&Address::indexed(storage, scratch, Scale::Eight, 0), source);
    }

    /// The runtime probes of the cache probe nodes, out of line.
    pub(in crate::codegen) fn emit_deferred_cache_probes(&mut self) -> Result<(), CompileFailure> {
        for deferred in std::mem::take(&mut self.deferred_cache_probes) {
            self.masm.bind(deferred.entry);
            let pc = self.graph.node(deferred.node).pc;
            self.emit_cache_probe(
                deferred.node,
                deferred.executable,
                pc,
                deferred.probe,
                deferred.hit,
                deferred.miss,
            )?;
        }
        Ok(())
    }

    /// Probes every entry of the cache with the runtime's probe; continues
    /// at `resume` if it handled the instruction, and at `slow` otherwise.
    fn emit_cache_probe(
        &mut self,
        node: NodeId,
        executable: u32,
        pc: u32,
        probe: CacheProbe,
        resume: Label,
        slow: Label,
    ) -> Result<(), CompileFailure> {
        let arguments = M::ARGUMENT_GPRS;
        let control = M::RETURN_GPRS[0];
        let allocation = self.allocation.node(node);
        let output = match allocation.output {
            Some(Location::Register(register)) => Some(Gpr(register)),
            _ => None,
        };
        let mut saved = slow_path_saved_registers::<M>();
        if let Some(output) = output {
            saved = saved.without(output);
        }
        let saved = saved.iter().collect::<Vec<_>>();
        self.emit_save_registers(&saved);

        let hit = self.masm.new_label();
        let base = self.input(node, 0);
        match probe {
            CacheProbe::GetById { cache } => {
                if base != arguments[0] {
                    self.masm.move64(arguments[0], base);
                }
                self.property_cache_address(arguments[1], executable, cache)?;
                self.masm.call_absolute(self.runtime.layout.try_get_by_id_cache);
                let missed = self.masm.new_label();
                self.masm
                    .branch64_imm(Condition::Equal, control, value::EMPTY as i64, missed);
                self.masm.move64(output.expect("GetById has an output"), control);
                self.masm.jump(hit);
                self.masm.bind(missed);
            }
            CacheProbe::PutById => {
                let source = self.input(node, 1);
                let record = self.locals.record;
                self.masm.store64(&self.local_address(record), base);
                self.masm.store64(&self.local_address(record + 8), source);
                self.masm
                    .load_effective_address(arguments[3], &self.local_address(record));
                let instruction = self.executables[executable as usize]
                    .bytecode_address
                    .checked_add(u64::from(pc))
                    .ok_or(CompileFailure::InvalidBytecode {
                        pc,
                        reason: "instruction address overflows",
                    })?;
                self.masm.move_imm64(arguments[2], instruction);
                self.masm.move_imm32(arguments[1], pc);
                self.masm.move64(arguments[0], self.pinned.vm);
                self.masm.call_absolute(self.runtime.layout.try_put_by_id_cache);
                self.masm.branch64_imm(Condition::Equal, control, 0, hit);
            }
            CacheProbe::HasProperty { own } => {
                let helpers = self.runtime.intrinsic_helpers;
                let key = self.input(node, 1);
                // NB: hasOwnProperty takes the object first, `in` the key.
                let (address, first, second) = if own {
                    (helpers.has_own_property, base, key)
                } else {
                    (helpers.has_property, key, base)
                };
                // NB: Without the helper, the probe misses.
                if address != 0 {
                    let scratch = self.pinned.scratch;
                    self.masm.move64(scratch, second);
                    if first != arguments[1] {
                        self.masm.move64(arguments[1], first);
                    }
                    self.masm.move64(arguments[2], scratch);
                    self.masm.move64(arguments[0], self.pinned.vm);
                    self.masm.call_absolute(address);
                    let missed = self.masm.new_label();
                    self.masm
                        .branch64_imm(Condition::Equal, control, value::EMPTY as i64, missed);
                    self.masm.move64(output.expect("probes have an output"), control);
                    self.masm.jump(hit);
                    self.masm.bind(missed);
                }
            }
        }

        // A miss restores the registers and takes the slow path.
        self.emit_restore_registers(&saved, None);
        self.masm.jump(slow);
        self.masm.bind(hit);
        self.emit_restore_registers(&saved, None);
        self.masm.jump(resume);
        Ok(())
    }
}
