/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Snapshots of executables for compile jobs, captured on the main thread straight from the runtime's structures.
//! A snapshot refers to cells only by address; the cells it captured are kept alive by whoever owns the job.

use libjs_jit::bytecode::{ExceptionHandler, FrameLayout};
use libjs_jit::snapshot::{
    CallFeedbackSnapshot, CellId, CompileOptions, ExecutableSnapshot, FeedbackSnapshot, InliningLimits,
    KeyedFeedbackSnapshot, PropertyCacheEntrySnapshot, PropertyCacheEntryType, PropertyCacheKind,
    PropertyCacheSnapshot, Snapshot, StressOptions,
};

use super::runtime_info::runtime_info;
use crate::bytecode::executable::{Executable, PropertyLookupCacheEntryData, PropertyLookupCacheTier};
use crate::bytecode::feedback::CallFeedback;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::layout::object::Object;
use crate::layout::property_lookup_cache::{
    PropertyLookupCache, PropertyLookupCacheEntry, PropertyLookupCacheEntryType,
};
use crate::layout::value::Value;
use crate::runtime::realm::Realm;

/// A snapshot with the cells it refers to, which must stay alive (and in place) until the job's code is installed or
/// the job is abandoned.
pub struct CapturedSnapshot {
    pub snapshot: Box<Snapshot>,
    pub cells: Vec<Gc<CellHeader>>,
}

struct SnapshotBuilder<'vm> {
    vm: &'vm Vm,
    cells: Vec<Gc<CellHeader>>,
    compiled_function: Gc<EcmascriptFunctionObject>,
}

/// The object the holder of a prototype chain lookup cache entry has in the property the entry describes, if the entry
/// is still valid and the property holds an object (such as a method).
fn prototype_property_object(entry: &PropertyLookupCacheEntryData) -> Option<Gc<Object>> {
    if entry.entry_type != PropertyLookupCacheEntryType::GetPropertyInPrototypeChain {
        return None;
    }
    let value = prototype_property_value(entry)?;
    value.is_object().then(|| value.as_object())
}

/// The value the prototype of a GetPropertyInPrototypeChain or ChangePropertyInPrototypeChain entry holds in the
/// property, if the entry still applies.
fn prototype_property_value(entry: &PropertyLookupCacheEntryData) -> Option<Value> {
    if !matches!(
        entry.entry_type,
        PropertyLookupCacheEntryType::GetPropertyInPrototypeChain
            | PropertyLookupCacheEntryType::ChangePropertyInPrototypeChain
    ) {
        return None;
    }
    let holder = entry.prototype?;
    if !entry.prototype_chain_validity?.is_valid() {
        return None;
    }
    if holder.shape().is_dictionary() || entry.property_offset >= holder.shape().property_count() {
        return None;
    }
    Some(holder.get_direct(entry.property_offset))
}

/// Whether the property of an entry held an accessor: the accessor the interpreter last called the getter of for a
/// GetOwnProperty entry, or the value its prototype holds in the property for entries of a prototype.
fn entry_holds_accessor(slot: &PropertyLookupCacheEntry) -> bool {
    let entry = slot.get();
    match entry.entry_type {
        PropertyLookupCacheEntryType::GetOwnProperty if entry.key == 0 => slot.accessor.get().is_some(),
        _ => prototype_property_value(&entry).is_some_and(|value| value.is_accessor()),
    }
}

impl SnapshotBuilder<'_> {
    fn cell<T>(&mut self, cell: Gc<T>) -> CellId {
        // SAFETY: Every cell starts with a cell header.
        let header = unsafe { Gc::<CellHeader>::from_non_null(cell.as_non_null().cast()) };
        self.cells.push(header);
        CellId(cell.as_ptr() as u64)
    }

    fn optional_cell<T>(&mut self, cell: Option<Gc<T>>) -> Option<CellId> {
        cell.map(|cell| self.cell(cell))
    }

    fn property_cache(&mut self, cache: &PropertyLookupCache, realm: Gc<Realm>) -> PropertyCacheSnapshot {
        let kind = match cache.tier() {
            PropertyLookupCacheTier::Empty => PropertyCacheKind::Empty,
            PropertyLookupCacheTier::Monomorphic => PropertyCacheKind::Monomorphic,
            PropertyLookupCacheTier::Polymorphic => PropertyCacheKind::Polymorphic,
            PropertyLookupCacheTier::Megamorphic => PropertyCacheKind::Megamorphic,
        };
        // NB: The interpreter caches the properties strings, numbers and booleans get from their prototypes as
        //     properties of those prototypes. JIT code speculating on such entries would check for an object and exit
        //     for every primitive, so caches with them are left to the fast paths, which look these up themselves.
        let primitive_prototype_shapes = [
            realm.string_prototype(self.vm).shape(),
            realm.number_prototype(self.vm).shape(),
            realm.boolean_prototype(self.vm).shape(),
        ];
        let mut sees_primitives = false;
        let mut entries = Vec::new();
        for slot in cache.entries() {
            let entry = slot.get();
            if entry
                .shape
                .is_some_and(|shape| primitive_prototype_shapes.contains(&shape))
            {
                sees_primitives = true;
                continue;
            }
            let entry_type = match entry.entry_type {
                // NB: Only the VM's keyed property lookup cache has entries for missing own properties.
                PropertyLookupCacheEntryType::Empty | PropertyLookupCacheEntryType::MissingOwnProperty => continue,
                PropertyLookupCacheEntryType::AddOwnProperty => PropertyCacheEntryType::AddOwnProperty,
                PropertyLookupCacheEntryType::ChangeOwnProperty => PropertyCacheEntryType::ChangeOwnProperty,
                PropertyLookupCacheEntryType::GetOwnProperty => PropertyCacheEntryType::GetOwnProperty,
                PropertyLookupCacheEntryType::ChangePropertyInPrototypeChain => {
                    PropertyCacheEntryType::ChangePropertyInPrototypeChain
                }
                PropertyLookupCacheEntryType::GetPropertyInPrototypeChain => {
                    PropertyCacheEntryType::GetPropertyInPrototypeChain
                }
                PropertyLookupCacheEntryType::GetMissingProperty => PropertyCacheEntryType::GetMissingProperty,
            };
            let prototype_property = prototype_property_object(&entry);
            entries.push(PropertyCacheEntrySnapshot {
                entry_type,
                property_offset: entry.property_offset,
                shape_dictionary_generation: entry.shape_dictionary_generation,
                shape_is_dictionary: entry.shape.is_some_and(|shape| shape.is_dictionary()),
                // NB: Nothing watches for objects leaving the shape, so no code depends on it staying stable.
                shape_is_stable: false,
                writes_data_property: entry.writes_data_property,
                from_shape: self.optional_cell(entry.from_shape),
                shape: self.optional_cell(entry.shape),
                prototype: self.optional_cell(entry.prototype),
                prototype_chain_validity: self.optional_cell(entry.prototype_chain_validity),
                // NB: Code checks the validity cell, since nothing invalidates code that relied on it.
                prototype_chain_valid: false,
                key: (entry.key != 0).then(|| self.cell(Value(entry.key).as_cell())),
                key_value: entry.key,
                prototype_property: self.optional_cell(prototype_property),
                prototype_property_intrinsic: None,
                accessor_function: None,
                holds_accessor: entry_holds_accessor(slot),
            });
        }
        if sees_primitives {
            entries.clear();
        }
        PropertyCacheSnapshot { kind, entries }
    }

    /// The first callee of a call site and how it was called. Calls are left to the runtime's call slow path.
    fn call_feedback(&mut self, call: &CallFeedback) -> CallFeedbackSnapshot {
        CallFeedbackSnapshot {
            target: self.optional_cell(call.target()),
            flags: call.flags.get(),
            ..CallFeedbackSnapshot::default()
        }
    }

    /// The snapshot of the compiled executable.
    fn executable_snapshot(&mut self, executable: Gc<Executable>, realm: Gc<Realm>) -> ExecutableSnapshot {
        let vm = self.vm;
        let bytecode = executable.bytecode();

        // NB: Value feedback is only up to date once the values the interpreter recorded since the last update are
        //     folded in.
        let feedback = executable.ensure_feedback();
        feedback.update_value_feedback();
        let call = feedback.call().iter().map(|call| self.call_feedback(call)).collect();
        let keyed = feedback
            .keyed()
            .iter()
            .map(|keyed| KeyedFeedbackSnapshot { bits: keyed.bits.get() })
            .collect();
        let feedback = FeedbackSnapshot {
            arith: feedback.arith().iter().map(core::cell::Cell::get).collect(),
            value: feedback.value.iter().map(core::cell::Cell::get).collect(),
            call,
            keyed,
        };

        let property_caches = executable
            .property_lookup_caches()
            .iter()
            .map(|cache| self.property_cache(cache, realm))
            .collect();

        ExecutableSnapshot {
            bytecode: bytecode.to_vec(),
            bytecode_address: bytecode.as_ptr() as u64,
            layout: FrameLayout {
                number_of_registers: executable.number_of_registers,
                registers_and_locals_count: executable.registers_and_locals_count(),
                number_of_constants: u32::try_from(executable.constants().len())
                    .expect("the constant count fits in u32"),
                number_of_arguments: executable.number_of_arguments,
            },
            constants: executable.constants().iter().map(|constant| constant.0).collect(),
            exception_handlers: executable
                .exception_handlers
                .iter()
                .map(|handler| ExceptionHandler {
                    start_offset: handler.start_offset,
                    end_offset: handler.end_offset,
                    handler_offset: handler.handler_offset,
                })
                .collect(),
            exit_sites: executable.jit_exit_sites(),
            builtin_exit_sites: Vec::new(),
            feedback,
            property_caches,
            cell: self.cell(executable),
            function: None,
            function_fields: None,
            environment: None,
            builtin: false,
            mapped_arguments_alias_parameters: !self.compiled_function.mapped_argument_names().is_empty(),
            new_object_shape: None,
            string_prototype: Some(self.cell(realm.string_prototype(vm))),
            number_prototype: Some(self.cell(realm.number_prototype(vm))),
            boolean_prototype: Some(self.cell(realm.boolean_prototype(vm))),
            object_shape_caches: Vec::new(),
            closure_templates: Vec::new(),
            environment_templates: Vec::new(),
            lexical_environment_templates: Vec::new(),
            // NB: Nothing watches the global variables, so compiled code leaves them to the interpreter's slow paths.
            globals: None,
            identifiers: executable
                .identifier_table
                .iter()
                .map(|identifier| identifier.raw_identity() as u64)
                .collect(),
        }
    }
}

/// Captures the snapshot of a compile job for `executable`, whose frames run `function`. `osr_pc` is the loop back
/// edge whose tier-up budget ran out, if a loop triggered the compile.
pub fn capture_snapshot(
    vm: &Vm,
    executable: Gc<Executable>,
    function: Gc<EcmascriptFunctionObject>,
    osr_pc: Option<u32>,
) -> CapturedSnapshot {
    let realm = function.realm().expect("an ECMAScript function has a realm");
    let mut builder = SnapshotBuilder {
        vm,
        cells: Vec::new(),
        compiled_function: function,
    };
    let executables = vec![builder.executable_snapshot(executable, realm)];
    let runtime = runtime_info(vm, realm);
    // NB: Code that embeds the prototypes of the runtime info lists them as embedded cells.
    builder.cell(realm.array_prototype());
    builder.cell(realm.object_prototype());
    let options = &vm.jit.options;
    let snapshot = Snapshot {
        executables,
        runtime,
        options: CompileOptions {
            dump_ir: options.dump_ir,
            dump_passes: options.dump_passes,
            dump_asm: options.dump_asm,
            verify_ir: options.verify_ir,
            osr_pc,
            inlining: InliningLimits::default(),
            coverage: options.coverage.is_some(),
            stress: StressOptions {
                exit_countdown: vm.jit.stress_exit_countdown_address(),
                osr_at_every_loop: options.stress_osr,
                few_registers: options.stress_registers,
            },
        },
    };
    CapturedSnapshot {
        snapshot: Box::new(snapshot),
        cells: builder.cells,
    }
}
