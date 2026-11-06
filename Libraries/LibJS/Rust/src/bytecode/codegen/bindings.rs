/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Reading and writing bindings.

use crate::bytecode::instruction::typed as op;
use crate::bytecode::operand::*;

use super::context::Generator;
use super::environment::BindingLocation;
use super::registers::ScopedOperand;

pub(super) fn emit_get_binding(
    generator: &mut Generator<'_>,
    dst: Operand,
    identifier: IdentifierTableIndex,
    known_initialized: bool,
) {
    // Prefer an eagerly-computed coordinate when the generator can prove that
    // the binding lives in a known declarative environment. The dynamic forms
    // remain necessary for script and eval code, eval-poisoned scopes, and
    // `with`.
    match (generator.binding_location_for_identifier(identifier), known_initialized) {
        (Some(BindingLocation::Import(import_index)), _) => generator.emit(op::GetImport {
            dst,
            identifier,
            import_index,
        }),
        (Some(BindingLocation::Environment(cache)), true) => {
            let value_feedback = generator.next_value_feedback();
            generator.emit(op::GetInitializedBinding {
                value_feedback,
                dst,
                identifier,
                cache,
            });
        }
        (Some(BindingLocation::Environment(cache)), false) => {
            let value_feedback = generator.next_value_feedback();
            generator.emit(op::GetBinding {
                value_feedback,
                dst,
                identifier,
                cache,
            });
        }
        (None, true) => {
            let cache = generator.next_environment_coordinate_cache();
            let value_feedback = generator.next_value_feedback();
            generator.emit(op::DynamicGetInitializedBinding {
                value_feedback,
                dst,
                identifier,
                cache,
            });
        }
        (None, false) => {
            let cache = generator.next_environment_coordinate_cache();
            let value_feedback = generator.next_value_feedback();
            generator.emit(op::DynamicGetBinding {
                value_feedback,
                dst,
                identifier,
                cache,
            });
        }
    }
}

pub(super) fn emit_get_callee_and_this_from_environment(
    generator: &mut Generator<'_>,
    callee: Operand,
    this_value: Operand,
    identifier: IdentifierTableIndex,
) {
    match generator.binding_location_for_identifier(identifier) {
        Some(BindingLocation::Environment(cache)) => {
            let value_feedback = generator.next_value_feedback();
            generator.emit(op::GetCalleeAndThisFromEnvironment {
                value_feedback,
                callee,
                this_value,
                identifier,
                cache,
            });
        }
        Some(BindingLocation::Import(import_index)) => {
            generator.emit(op::GetImport {
                dst: callee,
                identifier,
                import_index,
            });
            let undefined = generator.add_constant_undefined();
            generator.emit(op::Mov {
                dst: this_value,
                src: undefined.operand(),
            });
        }
        None => {
            let cache = generator.next_environment_coordinate_cache();
            let value_feedback = generator.next_value_feedback();
            generator.emit(op::DynamicGetCalleeAndThisFromEnvironment {
                value_feedback,
                callee,
                this_value,
                identifier,
                cache,
            });
        }
    }
}

pub(super) fn emit_initialize_lexical_binding(
    generator: &mut Generator<'_>,
    identifier: IdentifierTableIndex,
    src: Operand,
) {
    if let Some(cache) = generator.environment_coordinate_for_identifier(identifier) {
        generator.emit(op::InitializeLexicalBinding { identifier, src, cache });
    } else {
        let cache = generator.next_environment_coordinate_cache();
        generator.emit(op::DynamicInitializeLexicalBinding { identifier, src, cache });
    }
}

pub(super) fn emit_initialize_variable_binding(
    generator: &mut Generator<'_>,
    identifier: IdentifierTableIndex,
    src: Operand,
) {
    // Var bindings are resolved from vm.variable_environment(), not from the
    // current lexical environment. Use the generator's variable-environment
    // anchor so nested lexical scopes do not affect the coordinate hops.
    if let Some(cache) = generator.variable_environment_coordinate_for_identifier(identifier) {
        generator.emit(op::InitializeVariableBinding { identifier, src, cache });
    } else {
        let cache = generator.next_environment_coordinate_cache();
        generator.emit(op::DynamicInitializeVariableBinding { identifier, src, cache });
    }
}

pub(super) fn emit_set_lexical_binding(generator: &mut Generator<'_>, identifier: IdentifierTableIndex, src: Operand) {
    if let Some(cache) = generator.environment_coordinate_for_identifier(identifier) {
        generator.emit(op::SetLexicalBinding { identifier, src, cache });
    } else {
        let cache = generator.next_environment_coordinate_cache();
        generator.emit(op::DynamicSetLexicalBinding { identifier, src, cache });
    }
}

pub(super) fn emit_set_variable_binding(generator: &mut Generator<'_>, identifier: IdentifierTableIndex, src: Operand) {
    // See emit_initialize_variable_binding for why var coordinates use a
    // different lookup anchor than ordinary lexical operations.
    if let Some(cache) = generator.variable_environment_coordinate_for_identifier(identifier) {
        generator.emit(op::SetVariableBinding { identifier, src, cache });
    } else {
        let cache = generator.next_environment_coordinate_cache();
        generator.emit(op::DynamicSetVariableBinding { identifier, src, cache });
    }
}

pub(super) fn emit_typeof_binding(generator: &mut Generator<'_>, dst: Operand, identifier: IdentifierTableIndex) {
    match generator.binding_location_for_identifier(identifier) {
        Some(BindingLocation::Environment(cache)) => {
            generator.emit(op::TypeofBinding { dst, identifier, cache });
        }
        Some(BindingLocation::Import(import_index)) => {
            generator.emit(op::GetImport {
                dst,
                identifier,
                import_index,
            });
            generator.emit(op::Typeof { dst, src: dst });
        }
        None => {
            let cache = generator.next_environment_coordinate_cache();
            generator.emit(op::DynamicTypeofBinding { dst, identifier, cache });
        }
    }
}

pub(super) enum ResolvedBinding {
    /// The environment ResolveBinding found the binding in, or null if the reference is unresolvable.
    Environment {
        environment: ScopedOperand,
        identifier: IdentifierTableIndex,
    },
    /// Whether ResolveGlobalBinding found the binding of an identifier the compiler found no binding of before the
    /// global environment, in which case it is stored with SetGlobal.
    Global {
        is_resolvable: ScopedOperand,
        identifier: IdentifierTableIndex,
        cache: u32,
    },
}

pub(super) fn emit_set_resolved_binding(
    generator: &mut Generator<'_>,
    resolved_binding: &ResolvedBinding,
    value: &ScopedOperand,
) {
    match resolved_binding {
        ResolvedBinding::Environment {
            environment,
            identifier,
        } => {
            generator.emit(op::SetResolvedBinding {
                environment: environment.operand(),
                identifier: *identifier,
                src: value.operand(),
            });
        }
        ResolvedBinding::Global {
            is_resolvable,
            identifier,
            cache,
        } => {
            let set_block = generator.new_label();
            let unresolvable_block = generator.new_label();
            let end_block = generator.new_label();
            generator.emit_jump_if(is_resolvable, set_block, unresolvable_block);

            generator.bind_reachable_label(set_block);
            generator.emit(op::SetGlobal {
                identifier: *identifier,
                src: value.operand(),
                cache: *cache,
            });
            generator.emit(op::Jump { target: end_block });

            // NB: PutValue of an unresolvable reference throws a ReferenceError in strict mode code.
            generator.bind_reachable_label(unresolvable_block);
            let null = generator.add_constant_null();
            generator.emit(op::SetResolvedBinding {
                environment: null.operand(),
                identifier: *identifier,
                src: value.operand(),
            });
            generator.emit(op::Jump { target: end_block });

            generator.bind_reachable_label(end_block);
        }
    }
}
