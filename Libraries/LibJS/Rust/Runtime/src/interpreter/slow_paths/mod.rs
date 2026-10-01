/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime's implementation of the slow paths the interpreter calls, as in Libraries/LibJS/Interpreter/SlowPaths.cpp.
//! Each group lives in its own module, and the methods here hand each call to it.

pub mod bindings;
pub mod calls;
pub mod control;
pub mod operators;
pub mod property_access;

use super::runtime_functions::{Runtime, RuntimeFunctions, SlowPathControl, handle_asm_exception};
use super::vm::Vm;
use crate::bytecode::op;
use crate::layout::value::Value;

impl RuntimeFunctions for Runtime {
    // Arithmetic, comparisons, conversions and the jumps on comparisons: operators.rs.

    // Property access and its inline caches: property_access.rs.

    // Bindings and environments: bindings.rs.

    fn create_mutable_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateMutableBinding,
        values: &mut op::CreateMutableBindingValues,
    ) -> SlowPathControl {
        bindings::create_mutable_binding(vm, pc, instruction, values)
    }

    fn create_immutable_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateImmutableBinding,
        values: &mut op::CreateImmutableBindingValues,
    ) -> SlowPathControl {
        bindings::create_immutable_binding(vm, pc, instruction, values)
    }

    fn create_lexical_environment(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateLexicalEnvironment,
        values: &mut op::CreateLexicalEnvironmentValues,
    ) -> SlowPathControl {
        bindings::create_lexical_environment_slow_path(vm, pc, instruction, values)
    }

    fn create_variable_environment(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateVariableEnvironment,
        values: &mut op::CreateVariableEnvironmentValues,
    ) -> SlowPathControl {
        bindings::create_variable_environment_slow_path(vm, pc, instruction, values)
    }

    fn create_variable(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateVariable,
        values: &mut op::CreateVariableValues,
    ) -> SlowPathControl {
        bindings::create_variable_slow_path(vm, pc, instruction, values)
    }

    fn create_private_environment(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreatePrivateEnvironment,
        values: &mut op::CreatePrivateEnvironmentValues,
    ) -> SlowPathControl {
        bindings::create_private_environment(vm, pc, instruction, values)
    }

    fn dynamic_get_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicGetBinding,
        values: &mut op::DynamicGetBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_get_binding(vm, pc, instruction, values)
    }

    fn dynamic_get_initialized_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicGetInitializedBinding,
        values: &mut op::DynamicGetInitializedBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_get_initialized_binding(vm, pc, instruction, values)
    }

    fn dynamic_get_callee_and_this(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicGetCalleeAndThisFromEnvironment,
        values: &mut op::DynamicGetCalleeAndThisFromEnvironmentValues,
    ) -> SlowPathControl {
        bindings::dynamic_get_callee_and_this(vm, pc, instruction, values)
    }

    fn dynamic_initialize_lexical_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicInitializeLexicalBinding,
        values: &mut op::DynamicInitializeLexicalBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_initialize_lexical_binding(vm, pc, instruction, values)
    }

    fn dynamic_initialize_variable_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicInitializeVariableBinding,
        values: &mut op::DynamicInitializeVariableBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_initialize_variable_binding(vm, pc, instruction, values)
    }

    fn dynamic_set_lexical_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicSetLexicalBinding,
        values: &mut op::DynamicSetLexicalBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_set_lexical_binding(vm, pc, instruction, values)
    }

    fn dynamic_set_variable_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicSetVariableBinding,
        values: &mut op::DynamicSetVariableBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_set_variable_binding(vm, pc, instruction, values)
    }

    fn dynamic_typeof_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::DynamicTypeofBinding,
        values: &mut op::DynamicTypeofBindingValues,
    ) -> SlowPathControl {
        bindings::dynamic_typeof_binding(vm, pc, instruction, values)
    }

    fn enter_object_environment(
        vm: &Vm,
        pc: u32,
        instruction: &op::EnterObjectEnvironment,
        values: &mut op::EnterObjectEnvironmentValues,
    ) -> SlowPathControl {
        bindings::enter_object_environment(vm, pc, instruction, values)
    }

    fn get_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetBinding,
        values: &mut op::GetBindingValues,
    ) -> SlowPathControl {
        bindings::get_binding(vm, pc, instruction, values)
    }

    fn get_callee_and_this(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetCalleeAndThisFromEnvironment,
        values: &mut op::GetCalleeAndThisFromEnvironmentValues,
    ) -> SlowPathControl {
        bindings::get_callee_and_this(vm, pc, instruction, values)
    }

    fn get_global(vm: &Vm, pc: u32, instruction: &op::GetGlobal, values: &mut op::GetGlobalValues) -> SlowPathControl {
        bindings::get_global(vm, pc, instruction, values)
    }

    fn set_global(vm: &Vm, pc: u32, instruction: &op::SetGlobal, values: &mut op::SetGlobalValues) -> SlowPathControl {
        bindings::set_global(vm, pc, instruction, values)
    }

    fn get_import(vm: &Vm, pc: u32, instruction: &op::GetImport, values: &mut op::GetImportValues) -> SlowPathControl {
        bindings::get_import(vm, pc, instruction, values)
    }

    fn get_import_meta(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetImportMeta,
        values: &mut op::GetImportMetaValues,
    ) -> SlowPathControl {
        bindings::get_import_meta(vm, pc, instruction, values)
    }

    fn import_call(
        vm: &Vm,
        pc: u32,
        instruction: &op::ImportCall,
        values: &mut op::ImportCallValues,
    ) -> SlowPathControl {
        bindings::import_call(vm, pc, instruction, values)
    }

    fn get_new_target(
        vm: &Vm,
        pc: u32,
        instruction: &op::GetNewTarget,
        values: &mut op::GetNewTargetValues,
    ) -> SlowPathControl {
        bindings::get_new_target(vm, pc, instruction, values)
    }

    fn resolve_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::ResolveBinding,
        values: &mut op::ResolveBindingValues,
    ) -> SlowPathControl {
        bindings::resolve_binding(vm, pc, instruction, values)
    }

    fn resolve_super_base(
        vm: &Vm,
        pc: u32,
        instruction: &op::ResolveSuperBase,
        values: &mut op::ResolveSuperBaseValues,
    ) -> SlowPathControl {
        bindings::resolve_super_base(vm, pc, instruction, values)
    }

    fn resolve_this_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::ResolveThisBinding,
        values: &mut op::ResolveThisBindingValues,
    ) -> SlowPathControl {
        bindings::resolve_this_binding(vm, pc, instruction, values)
    }

    fn set_lexical_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::SetLexicalBinding,
        values: &mut op::SetLexicalBindingValues,
    ) -> SlowPathControl {
        bindings::set_lexical_binding(vm, pc, instruction, values)
    }

    fn set_variable_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::SetVariableBinding,
        values: &mut op::SetVariableBindingValues,
    ) -> SlowPathControl {
        bindings::set_variable_binding(vm, pc, instruction, values)
    }

    fn set_resolved_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::SetResolvedBinding,
        values: &mut op::SetResolvedBindingValues,
    ) -> SlowPathControl {
        bindings::set_resolved_binding(vm, pc, instruction, values)
    }

    fn delete_variable(
        vm: &Vm,
        pc: u32,
        instruction: &op::DeleteVariable,
        values: &mut op::DeleteVariableValues,
    ) -> SlowPathControl {
        bindings::delete_variable(vm, pc, instruction, values)
    }

    fn typeof_binding(
        vm: &Vm,
        pc: u32,
        instruction: &op::TypeofBinding,
        values: &mut op::TypeofBindingValues,
    ) -> SlowPathControl {
        bindings::typeof_binding(vm, pc, instruction, values)
    }

    fn verify_environment_coordinate(
        vm: &Vm,
        pc: u32,
        instruction: &op::VerifyEnvironmentCoordinate,
        values: &mut op::VerifyEnvironmentCoordinateValues,
    ) -> SlowPathControl {
        bindings::verify_environment_coordinate(vm, pc, instruction, values)
    }

    // Calls, functions and classes: calls.rs.

    fn try_inline_call(
        vm: &Vm,
        pc: u32,
        instruction: &op::Call,
        values: &mut op::CallValues,
        arguments: &mut [Value],
    ) -> bool {
        calls::try_inline_call(vm, pc, instruction, values, arguments)
    }

    fn create_rest_params(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateRestParams,
        values: &mut op::CreateRestParamsValues,
    ) -> SlowPathControl {
        calls::create_rest_params(vm, pc, instruction, values)
    }

    fn create_arguments(
        vm: &Vm,
        pc: u32,
        instruction: &op::CreateArguments,
        values: &mut op::CreateArgumentsValues,
    ) -> SlowPathControl {
        calls::create_arguments(vm, pc, instruction, values)
    }

    fn helper_handle_raw_native_exception(vm: u64, encoded_exception: u64) -> u64 {
        // SAFETY: The interpreter passes its VM.
        let vm = unsafe { &*core::ptr::with_exposed_provenance::<Vm>(vm as usize) };
        calls::handle_raw_native_exception(vm, Value(encoded_exception)).0 as u64
    }

    // Literals, iterators, generators and control flow: control.rs.

    fn throw(vm: &Vm, pc: u32, _instruction: &op::Throw, values: &mut op::ThrowValues) -> SlowPathControl {
        handle_asm_exception(vm, pc, values.src)
    }

    fn throw_if_tdz(
        vm: &Vm,
        pc: u32,
        _instruction: &op::ThrowIfTDZ,
        values: &mut op::ThrowIfTDZValues,
    ) -> SlowPathControl {
        control::throw_if_tdz(vm, pc, values)
    }

    fn throw_if_not_object(
        vm: &Vm,
        pc: u32,
        _instruction: &op::ThrowIfNotObject,
        values: &mut op::ThrowIfNotObjectValues,
    ) -> SlowPathControl {
        control::throw_if_not_object(vm, pc, values)
    }

    fn throw_if_nullish(
        vm: &Vm,
        pc: u32,
        _instruction: &op::ThrowIfNullish,
        values: &mut op::ThrowIfNullishValues,
    ) -> SlowPathControl {
        control::throw_if_nullish(vm, pc, values)
    }

    fn throw_const_assignment(
        vm: &Vm,
        pc: u32,
        _instruction: &op::ThrowConstAssignment,
        _values: &mut op::ThrowConstAssignmentValues,
    ) -> SlowPathControl {
        control::throw_const_assignment(vm, pc)
    }
}
