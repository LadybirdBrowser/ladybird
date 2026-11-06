/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Which environments the running code has, and what is known of the bindings in them.

use std::sync::Arc;
use std::sync::OnceLock;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;

use ak::Utf16FlyString;

use crate::bytecode::instruction::typed as op;
use crate::bytecode::operand::*;
use crate::fast_hash::HashMap;
use crate::u32_from_usize;

use super::context::Generator;
use super::registers::ScopedOperand;

/// What a function closes over of an environment, as of the moment that the function came up.
pub struct EnclosingEnvironmentScope {
    /// All bindings that the environment ends up with, for all that close over it, whenever that is.
    bindings: Arc<OnceLock<HashMap<ak::Utf16FlyString, u32>>>,
    /// The bindings of lower indices are the ones that existed at that moment.
    binding_count: u32,
    imports: HashMap<ak::Utf16FlyString, u32>,
    kind: EnvironmentCoordinateScopeKind,
    stops_unresolved_lookups: AtomicBool,
    parent: Option<Arc<EnclosingEnvironmentScope>>,
}

impl EnclosingEnvironmentScope {
    pub fn for_module_environment(
        binding_names: Vec<ak::Utf16FlyString>,
        import_names: Vec<ak::Utf16FlyString>,
    ) -> Arc<Self> {
        fn index_names(names: Vec<ak::Utf16FlyString>) -> HashMap<ak::Utf16FlyString, u32> {
            let mut indices = HashMap::with_capacity_and_hasher(names.len(), Default::default());
            for name in names {
                let index = u32_from_usize(indices.len());
                let previous_index = indices.insert(name, index);
                debug_assert!(previous_index.is_none());
            }
            indices
        }
        let bindings = index_names(binding_names);
        Arc::new(Self {
            binding_count: u32_from_usize(bindings.len()),
            bindings: Arc::new(OnceLock::from(bindings)),
            imports: index_names(import_names),
            kind: EnvironmentCoordinateScopeKind::Static,
            stops_unresolved_lookups: AtomicBool::new(false),
            parent: None,
        })
    }
}

#[derive(Clone, Copy)]
pub enum BindingLocation {
    Environment(EnvironmentCoordinate),
    Import(u32),
}

pub(super) enum ScopeLookup {
    Found(u32),
    NotFound,
    Unresolvable,
}

pub(super) fn look_up_in_scope(
    name: &ak::Utf16FlyString,
    bindings: &HashMap<ak::Utf16FlyString, u32>,
    binding_count: u32,
    kind: EnvironmentCoordinateScopeKind,
    stops_unresolved_lookups: bool,
) -> ScopeLookup {
    // Coordinates are only safe through fully-known declarative scopes. If
    // any dynamic scope is crossed, preserve the runtime lookup semantics.
    if kind == EnvironmentCoordinateScopeKind::Dynamic {
        return ScopeLookup::Unresolvable;
    }
    if let Some(&index) = bindings.get(name)
        && index < binding_count
    {
        return ScopeLookup::Found(index);
    }
    if stops_unresolved_lookups {
        return ScopeLookup::Unresolvable;
    }
    ScopeLookup::NotFound
}

#[derive(Clone, Copy, PartialEq)]
pub(super) enum EnvironmentCoordinateScopeKind {
    // A declarative environment whose bindings are created by bytecode we emit,
    // or by module linking in an order we mirror. The binding indexes are
    // therefore known while generating the instruction stream and can be
    // embedded in EnvironmentCoordinate operands.
    Static,
    // An object environment, such as `with`, can intercept any name. Once one
    // is between the current point and a binding, resolution must stay dynamic.
    Dynamic,
}

pub(super) struct EnvironmentCoordinateScope {
    bindings: HashMap<Utf16FlyString, u32>,
    next_binding_index: u32,
    kind: EnvironmentCoordinateScopeKind,
    stops_unresolved_lookups: bool,
    captured_scope: Option<Arc<EnclosingEnvironmentScope>>,
    /// Where the bindings go once they are all known, if anything closes over the scope.
    final_bindings: Option<Arc<OnceLock<HashMap<Utf16FlyString, u32>>>>,
}

impl Drop for EnvironmentCoordinateScope {
    fn drop(&mut self) {
        if let Some(final_bindings) = &self.final_bindings {
            final_bindings
                .set(std::mem::take(&mut self.bindings))
                .expect("a scope ends once");
        }
    }
}

impl Generator<'_> {
    pub(super) fn current_lexical_environment(&mut self) -> ScopedOperand {
        self.context
            .lexical_environment_register_stack
            .last()
            .cloned()
            .unwrap_or_else(|| self.scoped_operand(Operand::register(Register::SAVED_LEXICAL_ENVIRONMENT)))
    }

    pub(super) fn capture_saved_lexical_environment_with_coordinates(&mut self, has_function_environment: bool) {
        let env_reg = self.scoped_operand(Operand::register(Register::SAVED_LEXICAL_ENVIRONMENT));
        if self.creates_environment {
            self.emit(op::GetLexicalEnvironment { dst: env_reg.operand() });
        }
        if !has_function_environment {
            self.push_untracked_lexical_environment(env_reg);
            return;
        }
        self.push_static_variable_environment(env_reg);
    }

    /// Makes `environment` the running one again, on the way out of an environment or at the start of a handler, where
    /// exception unwinding left the running environment as it was. Code that never changes it has nothing to restore.
    #[inline(always)]
    pub(super) fn restore_lexical_environment(&mut self, environment: &ScopedOperand) {
        if self.creates_environment {
            self.emit(op::SetLexicalEnvironment {
                environment: environment.operand(),
            });
        }
    }

    pub(super) fn push_new_lexical_environment(&mut self, capacity: u32) -> ScopedOperand {
        self.push_new_lexical_environment_impl(capacity, false)
    }

    fn push_new_lexical_environment_impl(&mut self, capacity: u32, is_catch_environment: bool) -> ScopedOperand {
        let parent = self.current_lexical_environment();
        let new_env = self.allocate_register();
        let shape_cache = self.next_environment_shape_cache();
        self.emit(op::CreateLexicalEnvironment {
            dst: new_env.operand(),
            parent: parent.operand(),
            capacity,
            shape_cache,
            is_catch_environment,
        });
        self.push_static_lexical_environment(new_env.clone());
        new_env
    }

    pub(super) fn push_untracked_lexical_environment(&mut self, environment: ScopedOperand) {
        self.context.lexical_environment_register_stack.push(environment);
    }

    pub(super) fn push_static_lexical_environment(&mut self, environment: ScopedOperand) {
        self.push_untracked_lexical_environment(environment);
        self.push_environment_coordinate_scope(EnvironmentCoordinateScopeKind::Static);
    }

    pub(super) fn push_dynamic_lexical_environment(&mut self, environment: ScopedOperand) {
        self.push_untracked_lexical_environment(environment);
        self.push_environment_coordinate_scope(EnvironmentCoordinateScopeKind::Dynamic);
    }

    pub(super) fn push_static_variable_environment(&mut self, environment: ScopedOperand) {
        self.push_static_lexical_environment(environment);
        let scope_index = self.context.environment_coordinate_scope_stack.len() - 1;
        self.context.environment_coordinate_scope_stack[scope_index].stops_unresolved_lookups =
            self.contains_direct_call_to_eval_in_non_strict_mode;
        self.variable_environment_coordinate_scope_index = Some(scope_index);
    }

    pub(super) fn pop_untracked_lexical_environment(&mut self) -> Option<ScopedOperand> {
        self.context.lexical_environment_register_stack.pop()
    }

    pub(super) fn pop_tracked_lexical_environment(&mut self) -> Option<ScopedOperand> {
        self.pop_environment_coordinate_scope();
        self.pop_untracked_lexical_environment()
    }

    fn push_environment_coordinate_scope(&mut self, kind: EnvironmentCoordinateScopeKind) {
        self.context.environment_generation = self.context.environment_generation.wrapping_add(1);
        self.context
            .environment_coordinate_scope_stack
            .push(EnvironmentCoordinateScope {
                bindings: HashMap::default(),
                next_binding_index: 0,
                kind,
                stops_unresolved_lookups: false,
                captured_scope: None,
                final_bindings: None,
            });
    }

    pub(super) fn capture_environment_coordinate_scopes(&mut self) -> Option<Arc<EnclosingEnvironmentScope>> {
        let mut parent = self.enclosing_environment_scope.clone();
        let mut parent_was_captured_again = false;
        for scope in &mut self.context.environment_coordinate_scope_stack {
            if parent_was_captured_again || scope.captured_scope.is_none() {
                scope.captured_scope = Some(Arc::new(EnclosingEnvironmentScope {
                    bindings: scope.final_bindings.get_or_insert_default().clone(),
                    binding_count: scope.next_binding_index,
                    imports: HashMap::default(),
                    kind: scope.kind,
                    stops_unresolved_lookups: AtomicBool::new(scope.stops_unresolved_lookups),
                    parent,
                }));
                parent_was_captured_again = true;
            }
            parent = scope.captured_scope.clone();
        }
        parent
    }

    /// Nothing gets declared in the scopes around the end of the code, and what closes over them can have their bindings.
    pub(super) fn end_environment_coordinate_scopes(&mut self) {
        self.context.environment_coordinate_scope_stack.clear();
    }

    fn pop_environment_coordinate_scope(&mut self) {
        self.context.environment_generation = self.context.environment_generation.wrapping_add(1);
        self.context.environment_coordinate_scope_stack.pop();
    }

    /// Records a binding of the innermost environment that an instruction other than CreateVariable creates.
    pub(super) fn record_environment_binding_created_elsewhere(&mut self, identifier: IdentifierTableIndex) {
        let name = self.context.names.identifier_fly_string(self.tree, identifier);
        self.record_environment_binding(name);
    }

    pub(super) fn record_environment_binding(&mut self, name: Utf16FlyString) {
        let Some(scope_index) = self.context.environment_coordinate_scope_stack.len().checked_sub(1) else {
            return;
        };
        self.record_environment_binding_at_scope_index(name, scope_index);
    }

    pub(super) fn record_variable_environment_binding(&mut self, name: Utf16FlyString) {
        let Some(scope_index) = self.variable_environment_coordinate_scope_index else {
            return;
        };
        self.record_environment_binding_at_scope_index(name, scope_index);
    }

    fn record_environment_binding_at_scope_index(&mut self, name: Utf16FlyString, scope_index: usize) {
        self.context.environment_generation = self.context.environment_generation.wrapping_add(1);
        let Some(scope) = self.context.environment_coordinate_scope_stack.get_mut(scope_index) else {
            return;
        };
        if scope.kind == EnvironmentCoordinateScopeKind::Dynamic {
            return;
        }
        debug_assert!(!scope.bindings.contains_key(&name));
        scope.bindings.insert(name, scope.next_binding_index);
        scope.next_binding_index += 1;
        if let Some(captured_scope) = scope.captured_scope.take() {
            captured_scope.stops_unresolved_lookups.store(true, Ordering::Relaxed);
        }
    }

    fn environment_coordinate_for_from_scope_index(
        &self,
        name: &Utf16FlyString,
        scope_index: Option<usize>,
    ) -> Option<EnvironmentCoordinate> {
        match self.binding_location_for_from_scope_index(name, scope_index)? {
            BindingLocation::Environment(coordinate) => Some(coordinate),
            BindingLocation::Import(_) => None,
        }
    }

    fn binding_location_for_from_scope_index(
        &self,
        name: &Utf16FlyString,
        scope_index: Option<usize>,
    ) -> Option<BindingLocation> {
        let local_scopes = match scope_index {
            Some(scope_index) => &self.context.environment_coordinate_scope_stack[..=scope_index],
            None => &[],
        };
        let mut hops = 0;
        for scope in local_scopes.iter().rev() {
            match look_up_in_scope(
                name,
                &scope.bindings,
                scope.next_binding_index,
                scope.kind,
                scope.stops_unresolved_lookups,
            ) {
                ScopeLookup::Found(index) => {
                    return Some(BindingLocation::Environment(EnvironmentCoordinate { hops, index }));
                }
                ScopeLookup::NotFound => hops += 1,
                ScopeLookup::Unresolvable => return None,
            }
        }
        let mut enclosing_scope = self.enclosing_environment_scope.as_deref();
        while let Some(scope) = enclosing_scope {
            let stops_unresolved_lookups = scope.stops_unresolved_lookups.load(Ordering::Relaxed);
            let bindings = scope
                .bindings
                .get()
                .expect("the code around a function ended before it");
            match look_up_in_scope(
                name,
                bindings,
                scope.binding_count,
                scope.kind,
                stops_unresolved_lookups,
            ) {
                ScopeLookup::Found(index) => {
                    return Some(BindingLocation::Environment(EnvironmentCoordinate { hops, index }));
                }
                ScopeLookup::NotFound => hops += 1,
                ScopeLookup::Unresolvable => return None,
            }
            if let Some(import_index) = scope.imports.get(name) {
                return Some(BindingLocation::Import(*import_index));
            }
            enclosing_scope = scope.parent.as_deref();
        }
        None
    }

    pub(super) fn environment_coordinate_for_identifier(
        &mut self,
        identifier: IdentifierTableIndex,
    ) -> Option<EnvironmentCoordinate> {
        match self.binding_location_for_identifier(identifier)? {
            BindingLocation::Environment(coordinate) => Some(coordinate),
            BindingLocation::Import(_) => None,
        }
    }

    #[inline(always)]
    pub(super) fn binding_location_for_identifier(
        &mut self,
        identifier: IdentifierTableIndex,
    ) -> Option<BindingLocation> {
        let generation = self.context.environment_generation;
        if let Some((memo_generation, location)) = self.context.binding_locations.get(identifier.0 as usize)
            && *memo_generation == generation
        {
            return *location;
        }
        self.look_up_binding_location(identifier)
    }

    fn look_up_binding_location(&mut self, identifier: IdentifierTableIndex) -> Option<BindingLocation> {
        let name = &self.context.names.identifier_fly_string(self.tree, identifier);
        let location = self.binding_location_for_from_scope_index(
            name,
            self.context.environment_coordinate_scope_stack.len().checked_sub(1),
        );
        let index = identifier.0 as usize;
        let generation = self.context.environment_generation;
        let memo = &mut self.context.binding_locations;
        if index >= memo.len() {
            memo.resize(index + 1, (generation.wrapping_sub(1), None));
        }
        memo[index] = (generation, location);
        location
    }

    pub(super) fn variable_environment_coordinate_for_identifier(
        &mut self,
        identifier: IdentifierTableIndex,
    ) -> Option<EnvironmentCoordinate> {
        let name = &self.context.names.identifier_fly_string(self.tree, identifier);
        self.environment_coordinate_for_from_scope_index(name, Some(self.variable_environment_coordinate_scope_index?))
    }
}
