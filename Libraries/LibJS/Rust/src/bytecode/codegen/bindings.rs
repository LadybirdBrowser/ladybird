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
use super::environment::location_for_instructions;

pub(super) fn emit_get_binding(
    generator: &mut Generator<'_>,
    dst: Operand,
    identifier: IdentifierTableIndex,
    known_initialized: bool,
) {
    let location = generator.binding_location_for_identifier(identifier);
    emit_get_binding_at(generator, dst, identifier, location, known_initialized);
}

/// Reads the binding at `location`, as seen from the running lexical environment.
pub(super) fn emit_get_binding_at(
    generator: &mut Generator<'_>,
    dst: Operand,
    identifier: IdentifierTableIndex,
    location: Option<BindingLocation>,
    known_initialized: bool,
) {
    // Prefer an eagerly-computed coordinate when the generator can prove that
    // the binding lives in a known declarative environment. The dynamic forms
    // remain necessary for script and eval code, eval-poisoned scopes, and
    // `with`.
    match (location_for_instructions(location), known_initialized) {
        (Some(BindingLocation::Import(import_index)), _) => generator.put(op::GetImport {
            dst,
            identifier,
            import_index,
        }),
        (Some(BindingLocation::Environment(cache)), true) => {
            let value_feedback = generator.next_value_feedback();
            generator.put(op::GetInitializedBinding {
                value_feedback,
                dst,
                identifier,
                cache,
            });
        }
        (Some(BindingLocation::Environment(cache)), false) => {
            let value_feedback = generator.next_value_feedback();
            generator.put(op::GetBinding {
                value_feedback,
                dst,
                identifier,
                cache,
            });
        }
        (None, true) => {
            let cache = generator.next_environment_coordinate_cache();
            let value_feedback = generator.next_value_feedback();
            generator.put(op::DynamicGetInitializedBinding {
                value_feedback,
                dst,
                identifier,
                cache,
            });
        }
        (None, false) => {
            let cache = generator.next_environment_coordinate_cache();
            let value_feedback = generator.next_value_feedback();
            generator.put(op::DynamicGetBinding {
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
    match location_for_instructions(generator.binding_location_for_identifier(identifier)) {
        Some(BindingLocation::Environment(cache)) => {
            let value_feedback = generator.next_value_feedback();
            generator.put(op::GetCalleeAndThisFromEnvironment {
                value_feedback,
                callee,
                this_value,
                identifier,
                cache,
            });
        }
        Some(BindingLocation::Import(import_index)) => {
            generator.put(op::GetImport {
                dst: callee,
                identifier,
                import_index,
            });
            let undefined = generator.add_constant_undefined();
            generator.emit_mov(this_value, undefined);
        }
        None => {
            let cache = generator.next_environment_coordinate_cache();
            let value_feedback = generator.next_value_feedback();
            generator.put(op::DynamicGetCalleeAndThisFromEnvironment {
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
        generator.put(op::InitializeLexicalBinding { identifier, src, cache });
    } else {
        let cache = generator.next_environment_coordinate_cache();
        generator.put(op::DynamicInitializeLexicalBinding { identifier, src, cache });
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
        generator.put(op::InitializeVariableBinding { identifier, src, cache });
    } else {
        let cache = generator.next_environment_coordinate_cache();
        generator.put(op::DynamicInitializeVariableBinding { identifier, src, cache });
    }
}

pub(super) fn emit_set_lexical_binding(generator: &mut Generator<'_>, identifier: IdentifierTableIndex, src: Operand) {
    if let Some(cache) = generator.environment_coordinate_for_identifier(identifier) {
        generator.put(op::SetLexicalBinding { identifier, src, cache });
    } else {
        let cache = generator.next_environment_coordinate_cache();
        generator.put(op::DynamicSetLexicalBinding { identifier, src, cache });
    }
}

pub(super) fn emit_set_variable_binding(generator: &mut Generator<'_>, identifier: IdentifierTableIndex, src: Operand) {
    // See emit_initialize_variable_binding for why var coordinates use a
    // different lookup anchor than ordinary lexical operations.
    if let Some(cache) = generator.variable_environment_coordinate_for_identifier(identifier) {
        generator.put(op::SetVariableBinding { identifier, src, cache });
    } else {
        let cache = generator.next_environment_coordinate_cache();
        generator.put(op::DynamicSetVariableBinding { identifier, src, cache });
    }
}

pub(super) fn emit_typeof_binding(generator: &mut Generator<'_>, dst: Operand, identifier: IdentifierTableIndex) {
    match location_for_instructions(generator.binding_location_for_identifier(identifier)) {
        Some(BindingLocation::Environment(cache)) => {
            generator.put(op::TypeofBinding { dst, identifier, cache });
        }
        Some(BindingLocation::Import(import_index)) => {
            generator.put(op::GetImport {
                dst,
                identifier,
                import_index,
            });
            generator.put(op::Typeof { dst, src: dst });
        }
        None => {
            let cache = generator.next_environment_coordinate_cache();
            generator.put(op::DynamicTypeofBinding { dst, identifier, cache });
        }
    }
}

pub(super) enum ResolvedBinding {
    /// The environment ResolveBinding found the binding in, or null if the reference is unresolvable.
    Environment {
        environment: Operand,
        identifier: IdentifierTableIndex,
    },
    /// Whether ResolveGlobalBinding found the binding of an identifier the compiler found no binding of before the
    /// global environment, in which case it is stored with SetGlobal.
    Global {
        is_resolvable: Operand,
        identifier: IdentifierTableIndex,
        cache: u32,
    },
}

pub(super) fn emit_set_resolved_binding(
    generator: &mut Generator<'_>,
    resolved_binding: &ResolvedBinding,
    value: Operand,
) {
    match resolved_binding {
        ResolvedBinding::Environment {
            environment,
            identifier,
        } => {
            generator.put(op::SetResolvedBinding {
                environment: *environment,
                identifier: *identifier,
                src: value,
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
            generator.emit_jump_if(*is_resolvable, set_block, unresolvable_block);

            generator.bind_reachable_label(set_block);
            generator.put(op::SetGlobal {
                identifier: *identifier,
                src: value,
                cache: *cache,
            });
            generator.put(op::Jump { target: end_block });

            // NB: PutValue of an unresolvable reference throws a ReferenceError in strict mode code.
            generator.bind_reachable_label(unresolvable_block);
            let null = generator.add_constant_null();
            generator.put(op::SetResolvedBinding {
                environment: null,
                identifier: *identifier,
                src: value,
            });
            generator.put(op::Jump { target: end_block });

            generator.bind_reachable_label(end_block);
        }
    }
}
