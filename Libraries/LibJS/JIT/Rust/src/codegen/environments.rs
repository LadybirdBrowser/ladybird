/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The probes of global variable caches, like the interpreter's
//! `GetGlobal` / `SetGlobal` handlers, the mutability test of bindings that
//! `BranchCondition::BindingMutable` shares with them, and the bindings
//! that environments of a shape get next, like its `CreateVariable`
//! handler.

use super::Codegen;
use super::checked_i32;
use crate::CompileFailure;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;
use crate::ir::NodeId;
use crate::ir::value;

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// Branches to `fail` unless binding `index` (a register) of the
    /// declarative environment `environment` is mutable, like the
    /// interpreter's `load_binding_flag`. Clobbers `temp` and the scratch
    /// register.
    pub(in crate::codegen) fn branch_unless_binding_mutable(
        &mut self,
        environment: Gpr,
        index: Gpr,
        temp: Gpr,
        fail: Label,
    ) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let (rare_at_index, rare_at_offset, check) =
            (self.masm.new_label(), self.masm.new_label(), self.masm.new_label());
        let rare_flags = |codegen: &mut Self| {
            codegen.masm.load64(
                scratch,
                &Address::new(environment, layout.declarative_environment_rare_data as i32),
            );
            codegen
                .masm
                .load64(scratch, &Address::new(scratch, layout.rare_data_binding_flags as i32));
        };

        self.masm.load64(
            scratch,
            &Address::new(environment, layout.declarative_environment_shape as i32),
        );
        self.masm
            .branch_test64(Condition::Zero, scratch, u64::MAX, rare_at_index);
        // Flags past the shape's are in the rare data, after the shape's.
        self.masm.load64(
            temp,
            &Address::new(scratch, layout.environment_shape_binding_flags_size as i32),
        );
        self.masm.branch64(Condition::BelowOrEqual, temp, index, rare_at_offset);
        self.masm.load64(
            scratch,
            &Address::new(scratch, layout.environment_shape_binding_flags as i32),
        );
        self.masm
            .load8(scratch, &Address::indexed(scratch, index, Scale::One, 0));
        self.masm.jump(check);

        self.masm.bind(rare_at_offset);
        self.masm.sub64(temp, index, temp);
        rare_flags(self);
        self.masm
            .load8(scratch, &Address::indexed(scratch, temp, Scale::One, 0));
        self.masm.jump(check);

        self.masm.bind(rare_at_index);
        rare_flags(self);
        self.masm
            .load8(scratch, &Address::indexed(scratch, index, Scale::One, 0));

        self.masm.bind(check);
        self.masm
            .branch_test32(Condition::Zero, scratch, u32::from(layout.binding_flag_mutable), fail);
    }

    /// Branches to `fail` unless the binding the shape of the environment
    /// `environment` has next is the binding `name` with `flags`, which
    /// `emit_append_environment_binding()` can append (see
    /// `BranchCondition::NextBindingOfShape`). Clobbers the temps of `node`
    /// and the scratch register.
    pub(in crate::codegen) fn emit_next_binding_of_shape_test(
        &mut self,
        node: NodeId,
        environment: Gpr,
        name: u64,
        flags: u8,
        fail: Label,
    ) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let (shape, index) = (self.temp(node, 0), self.temp(node, 1));

        // Only declarative environments have shapes, and module environments
        // find bindings their shapes do not have.
        self.masm.load8(
            scratch,
            &Address::new(environment, layout.environment_declarative as i32),
        );
        self.masm.branch_test32(Condition::Zero, scratch, 0xFF, fail);
        self.masm.load64(scratch, &Address::new(environment, 0));
        self.masm.move_imm64(shape, layout.module_environment_class);
        self.masm.branch64(Condition::Equal, scratch, shape, fail);

        self.masm.load64(
            shape,
            &Address::new(environment, layout.declarative_environment_shape as i32),
        );
        self.masm.branch_test64(Condition::Zero, shape, u64::MAX, fail);
        self.masm.load8(
            scratch,
            &Address::new(shape, layout.environment_shape_has_unique_binding_names as i32),
        );
        self.masm.branch_test32(Condition::Zero, scratch, 0xFF, fail);
        self.masm.load64(
            index,
            &Address::new(environment, layout.declarative_environment_binding_values_size as i32),
        );
        self.masm.load64(
            scratch,
            &Address::new(shape, layout.environment_shape_binding_flags_size as i32),
        );
        self.masm.branch64(Condition::BelowOrEqual, scratch, index, fail);
        self.masm.load64(
            scratch,
            &Address::new(shape, layout.environment_shape_binding_flags as i32),
        );
        self.masm
            .load8(scratch, &Address::indexed(scratch, index, Scale::One, 0));
        self.masm
            .branch32_imm(Condition::NotEqual, scratch, i32::from(flags), fail);
        self.masm.load64(
            scratch,
            &Address::new(shape, layout.environment_shape_binding_names as i32),
        );
        self.masm
            .load64(scratch, &Address::indexed(scratch, index, Scale::Eight, 0));
        self.masm.move_imm64(shape, name);
        self.masm.branch64(Condition::NotEqual, scratch, shape, fail);

        // Values that do not fit in their storage move first.
        self.masm.load64(
            scratch,
            &Address::new(
                environment,
                layout.declarative_environment_binding_values_capacity as i32,
            ),
        );
        self.masm.branch64(Condition::BelowOrEqual, scratch, index, fail);
    }

    /// An `AppendEnvironmentBinding` node of the declarative environment
    /// `environment`: an uninitialized binding after the others.
    pub(in crate::codegen) fn emit_append_environment_binding(&mut self, node: NodeId, environment: Gpr) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let index = self.temp(node, 0);
        self.masm.load64(
            index,
            &Address::new(environment, layout.declarative_environment_binding_values_size as i32),
        );
        self.masm.load64(
            scratch,
            &Address::new(environment, layout.declarative_environment_binding_values as i32),
        );
        self.masm
            .store_imm64(&Address::indexed(scratch, index, Scale::Eight, 0), value::EMPTY);
        self.masm.add64_imm(index, index, 1);
        self.masm.store64(
            &Address::new(environment, layout.declarative_environment_binding_values_size as i32),
            index,
        );
    }

    /// The common start of GetGlobal and SetGlobal, with the realm and the
    /// executable in the inputs after `context`: loads the global variable
    /// cache, the realm's global declarative environment and its global
    /// object, and checks the cache's environment serial number.
    /// Continues with the global object's named property at `property`, or
    /// branches to `environment_binding` if the cache entry does not match
    /// the global object's shape.
    fn emit_global_variable_prologue(
        &mut self,
        node: NodeId,
        context: usize,
        cache_index: u32,
        environment_binding: Label,
        slow: Label,
    ) -> Result<GlobalRegisters, CompileFailure> {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let registers = GlobalRegisters {
            cache: self.temp(node, 0),
            environment: self.temp(node, 1),
            object: self.temp(node, 2),
            temp: self.temp(node, 3),
        };
        let GlobalRegisters {
            cache,
            environment,
            object,
            temp,
        } = registers;

        let (realm, executable) = (self.input(node, context), self.input(node, context + 1));
        self.masm
            .load64(object, &Address::new(realm, layout.realm_global_object as i32));
        self.masm.load64(
            environment,
            &Address::new(realm, layout.realm_global_declarative_environment as i32),
        );
        self.masm.load64(
            cache,
            &Address::new(executable, layout.executable_global_variable_caches as i32),
        );
        let cache_offset = u64::from(cache_index) * u64::from(layout.global_variable_cache_size);
        self.masm.add64_imm(cache, cache, i64::from(checked_i32(cache_offset)?));

        self.masm.load64(
            scratch,
            &Address::new(environment, layout.declarative_environment_serial as i32),
        );
        self.masm.branch64_memory(
            Condition::NotEqual,
            &Address::new(cache, layout.global_variable_cache_environment_serial as i32),
            scratch,
            slow,
        );

        self.masm
            .load64(scratch, &Address::new(object, self.runtime.offsets.object_shape as i32));
        self.masm.branch64_memory(
            Condition::NotEqual,
            &Address::new(cache, layout.global_variable_cache_shape as i32),
            scratch,
            environment_binding,
        );
        let expected = Address::new(cache, layout.global_variable_cache_dictionary_generation as i32);
        self.branch_unless_dictionary_generation_is(scratch, expected, [temp, scratch], environment_binding);
        Ok(registers)
    }

    /// The address of the global object's named property the cache is for.
    /// Clobbers `temp` and the scratch register.
    fn global_property_address(&mut self, registers: &GlobalRegisters) -> Address {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        self.masm.load32(
            registers.temp,
            &Address::new(registers.cache, layout.global_variable_cache_property_offset as i32),
        );
        self.masm.load64(
            scratch,
            &Address::new(registers.object, self.runtime.offsets.object_named_properties as i32),
        );
        Address::indexed(scratch, registers.temp, Scale::Eight, 0)
    }

    /// Puts the index of the global declarative environment binding the
    /// cache is for in `temp`, or branches to `slow` if it is for none.
    fn load_global_binding_index(&mut self, registers: &GlobalRegisters, slow: Label) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        self.masm.load8(
            scratch,
            &Address::new(
                registers.cache,
                layout.global_variable_cache_has_environment_binding as i32,
            ),
        );
        self.masm.branch_test32(Condition::Zero, scratch, 0xFF, slow);
        self.masm.load32(
            registers.temp,
            &Address::new(
                registers.cache,
                layout.global_variable_cache_environment_binding_index as i32,
            ),
        );
    }

    /// The address of the global declarative environment binding whose
    /// index is in `temp`. Clobbers the scratch register.
    fn global_binding_address(&mut self, registers: &GlobalRegisters) -> Address {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        self.masm.load64(
            scratch,
            &Address::new(
                registers.environment,
                layout.declarative_environment_binding_values as i32,
            ),
        );
        Address::indexed(scratch, registers.temp, Scale::Eight, 0)
    }

    /// A `ProbeGlobalCache` node.
    pub(in crate::codegen) fn emit_probe_global_cache(
        &mut self,
        node: NodeId,
        cache_index: u32,
    ) -> Result<(), CompileFailure> {
        let output = self.output(node);
        let (failure, done) = (self.probe_failure_label(), self.masm.new_label());
        let slow = failure.0;
        let environment_binding = self.masm.new_label();
        let registers = self.emit_global_variable_prologue(node, 0, cache_index, environment_binding, slow)?;
        let (object, temp) = (registers.object, registers.temp);

        let property = self.global_property_address(&registers);
        self.masm.load64(object, &property);
        self.branch_on_tag(Condition::Equal, object, value::ACCESSOR_TAG, temp, slow);
        self.masm.move64(output, object);
        self.masm.jump(done);

        self.masm.bind(environment_binding);
        self.load_global_binding_index(&registers, slow);
        let binding = self.global_binding_address(&registers);
        self.masm.load64(object, &binding);
        self.masm
            .branch64_imm(Condition::Equal, object, value::EMPTY as i64, slow);
        self.masm.move64(output, object);
        self.bind_probe_failure(output, value::EMPTY, failure, done);
        Ok(())
    }

    /// A `ProbeGlobalStore` node.
    pub(in crate::codegen) fn emit_probe_global_store(
        &mut self,
        node: NodeId,
        cache_index: u32,
    ) -> Result<(), CompileFailure> {
        let (source, output) = (self.input(node, 0), self.output(node));
        let layout = self.runtime.layout;
        let (failure, stored, done) = (self.probe_failure_label(), self.masm.new_label(), self.masm.new_label());
        let slow = failure.0;
        let environment_binding = self.masm.new_label();
        let registers = self.emit_global_variable_prologue(node, 1, cache_index, environment_binding, slow)?;
        let (cache, environment, object, temp) =
            (registers.cache, registers.environment, registers.object, registers.temp);

        let property = self.global_property_address(&registers);
        // NB: The address uses `temp` and the scratch register; `object` is free.
        self.masm.load64(object, &property);
        self.branch_on_tag(Condition::Equal, object, value::ACCESSOR_TAG, object, slow);
        self.masm.load8(
            object,
            &Address::new(cache, layout.global_variable_cache_writes_data_property as i32),
        );
        self.masm.branch_test32(Condition::Zero, object, 0xFF, slow);
        self.masm.store64(&property, source);
        self.masm.jump(stored);

        self.masm.bind(environment_binding);
        self.load_global_binding_index(&registers, slow);
        self.branch_unless_binding_mutable(environment, temp, object, slow);
        let binding = self.global_binding_address(&registers);
        self.masm.load64(object, &binding);
        self.masm
            .branch64_imm(Condition::Equal, object, value::EMPTY as i64, slow);
        self.masm.store64(&binding, source);
        self.masm.bind(stored);
        self.masm.move_imm32(output, 1);
        self.bind_probe_failure(output, 0, failure, done);
        Ok(())
    }
}

/// The registers of the global variable fast paths.
struct GlobalRegisters {
    cache: Gpr,
    environment: Gpr,
    object: Gpr,
    temp: Gpr,
}
