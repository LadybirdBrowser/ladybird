/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Global variables, from what the interpreter's global variable caches
//! found (see `GlobalsSnapshot`).
//!
//! `GetGlobal` and `SetGlobal` access the binding of the global declarative
//! environment (a global `let`, `const` or `class`) or the property of the
//! global object their cache found directly, instead of going through the
//! cache like the interpreter does. Global object properties are checked
//! for the global object's shape, and the code depends on the global
//! declarative environment getting no bindings, such as those of a later
//! script, which may shadow them.
//!
//! An initialized immutable binding (`const`) keeps its value, which the
//! code uses as a constant, and so does a mutable one that was never
//! assigned, on which the code depends. So does a property of the global
//! object that holds an object (such as a function a script declares) and
//! was never assigned since it got that value, while the global object's
//! shape is a dictionary (see `Dependency::GlobalPropertyUnassigned`).
//! Other global variables that held an object (such as a function, a class
//! or a namespace object) are speculated to keep it, which is checked where
//! they are read, until a check like that fails. Those holding other values
//! are read as they are, since they tend to be counters, flags and other
//! state that changes.
//!
//! Where the cache found nothing to speculate on, `GetGlobal` and
//! `SetGlobal` probe the cache like the interpreter does, with
//! `ProbeGlobalCache` and `ProbeGlobalStore`, and take the slow path where
//! the probe does not apply.
//!
//! `instanceof` with such a function on its right-hand side, which inherits
//! the `@@hasInstance` of `%Function.prototype%` (which can never change),
//! walks the prototype chain of its left-hand side inline, looking for the
//! function's `prototype` property.

use super::Flow;
use super::GraphBuilder;
use crate::CompileFailure;
use crate::bytecode::Instruction;
use crate::bytecode::Operand;
use crate::code::Dependency;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::ir::BranchCondition;
use crate::ir::FrameField;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::ShapeCheck;
use crate::ir::value::OBJECT_TAG;
use crate::ir::value::tag;
use crate::snapshot::CellId;
use crate::snapshot::GlobalCacheSnapshot;
use crate::snapshot::GlobalValueSnapshot;

/// The global object and the global declarative environment of the realm
/// of the executable being built (see `GlobalsSnapshot`).
#[derive(Clone, Copy)]
struct Globals {
    object: CellId,
    declarative_environment: CellId,
    environment_serial: u64,
}

impl GraphBuilder<'_> {
    /// What global variable cache `cache` found, with the globals of the
    /// executable's realm, if code may access the variable directly.
    fn global_cache(&self, cache: u32) -> Option<(Globals, GlobalCacheSnapshot)> {
        let snapshot = self.function.executable.globals.as_ref()?;
        let global = (*snapshot.caches.get(cache as usize)?)?;
        let globals = Globals {
            object: snapshot.object,
            declarative_environment: snapshot.declarative_environment,
            environment_serial: snapshot.environment_serial,
        };
        Some((globals, global))
    }

    /// Builds `dst = GetGlobal` from the instruction's global variable
    /// cache. Returns false, having built nothing, if it cannot speculate.
    pub(super) fn try_build_get_global(&mut self, dst: Operand, cache: u32) -> Result<bool, CompileFailure> {
        let Some((globals, global)) = self.global_cache(cache) else {
            return Ok(false);
        };
        let value = match global {
            GlobalCacheSnapshot::Binding {
                mutable: false,
                value: Some(value),
                ..
            } => self.global_constant(value),
            GlobalCacheSnapshot::Binding {
                index,
                assigned: false,
                value: Some(value),
                ..
            } => {
                let environment = globals.declarative_environment;
                self.embed(environment);
                self.assume(Dependency::GlobalBindingUnassigned { environment, index });
                self.global_constant(value)
            }
            GlobalCacheSnapshot::Binding { index, value, .. } => {
                if !self.may_speculate(ExitKind::BadShape) {
                    return Ok(false);
                }
                let environment = globals.declarative_environment;
                self.embed(environment);
                let loaded = self.emit_checked(
                    Op::LoadGlobalBinding { environment, index },
                    Vec::new(),
                    Some(Repr::Tagged),
                );
                match value {
                    Some(value) => self.speculate_global_value(loaded, value),
                    None => loaded,
                }
            }
            GlobalCacheSnapshot::Property {
                dictionary_generation: Some(_),
                offset,
                assigned: false,
                value,
                ..
            } if tag(value.bits) == OBJECT_TAG => {
                self.assume_global_declarations(globals);
                self.assume(Dependency::GlobalPropertyUnassigned {
                    object: globals.object,
                    offset,
                });
                self.global_constant(value)
            }
            GlobalCacheSnapshot::Property {
                shape,
                dictionary_generation,
                offset,
                value,
                ..
            } => {
                if !self.may_speculate(ExitKind::BadShape) {
                    return Ok(false);
                }
                let object = self.global_object(globals);
                let shape = ShapeCheck {
                    shape,
                    dictionary_generation,
                };
                let object = self.check_shapes(object, &[shape]);
                let loaded = self.load_named(object, offset);
                self.speculate_global_value(loaded, value)
            }
        };
        self.write(dst, value)?;
        Ok(true)
    }

    /// Builds `SetGlobal` from the instruction's global variable cache.
    /// Returns false, having built nothing, if it cannot speculate.
    pub(super) fn try_build_set_global(&mut self, src: Operand, cache: u32) -> Result<bool, CompileFailure> {
        let Some((globals, global)) = self.global_cache(cache) else {
            return Ok(false);
        };
        if !self.may_speculate(ExitKind::BadShape) {
            return Ok(false);
        }
        match global {
            GlobalCacheSnapshot::Binding {
                index, mutable: true, ..
            } => {
                let environment = globals.declarative_environment;
                self.embed(environment);
                let value = self.read(src)?;
                self.emit_checked(Op::StoreGlobalBinding { environment, index }, vec![value], None);
            }
            GlobalCacheSnapshot::Property {
                shape,
                dictionary_generation,
                offset,
                writes_data_property: true,
                ..
            } => {
                let object = self.global_object(globals);
                let shape = ShapeCheck {
                    shape,
                    dictionary_generation,
                };
                let object = self.check_shapes(object, &[shape]);
                let value = self.read(src)?;
                self.store_named(object, offset, value);
            }
            _ => return Ok(false),
        }
        Ok(true)
    }

    /// Builds `GetGlobal` (`Handling::Expanded`): the global variable that
    /// the instruction's global variable cache finds, like the interpreter.
    pub(super) fn build_get_global(&mut self, instruction: &Instruction) -> Result<Flow, CompileFailure> {
        let Instruction::GetGlobal { dst, cache, .. } = *instruction else {
            unreachable!("only GetGlobal is built here");
        };
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let realm = self.frame_field(FrameField::Realm)?;
        let executable = self.frame_field(FrameField::Executable)?;
        let value = self.emit(
            Op::ProbeGlobalCache { cache },
            vec![realm, executable],
            Some(Repr::Tagged),
        );
        let value = self.branch_if_empty(&mut slow_paths, value);
        let result = self.join_slow_paths(slow_paths, Some(value));
        self.write(dst, result.expect("GetGlobal has a value"))?;
        Ok(Flow::Continue)
    }

    /// Builds `SetGlobal` (`Handling::Expanded`): stores to the global
    /// variable that the instruction's global variable cache finds, like
    /// the interpreter.
    pub(super) fn build_set_global(&mut self, instruction: &Instruction) -> Result<Flow, CompileFailure> {
        let Instruction::SetGlobal { src, cache, .. } = *instruction else {
            unreachable!("only SetGlobal is built here");
        };
        let mut slow_paths = self.start_slow_paths(None)?;
        let value = self.read(src)?;
        let realm = self.frame_field(FrameField::Realm)?;
        let executable = self.frame_field(FrameField::Executable)?;
        let stored = self.emit(
            Op::ProbeGlobalStore { cache },
            vec![value, realm, executable],
            Some(Repr::Bool),
        );
        self.branch_to_slow_path(&mut slow_paths, BranchCondition::Bool, vec![stored], true);
        self.join_slow_paths(slow_paths, None);
        Ok(Flow::Continue)
    }

    /// Builds `dst = lhs instanceof rhs` for an `rhs` that is a function the
    /// code uses as a constant, whose `@@hasInstance` is the one of
    /// `%Function.prototype%` while it keeps its shape. Returns false, having
    /// built nothing, if it cannot speculate.
    pub(super) fn try_build_instance_of(
        &mut self,
        dst: Operand,
        lhs: Operand,
        rhs: Operand,
    ) -> Result<bool, CompileFailure> {
        let function = self.read(rhs)?;
        let Some(has_instance) = self
            .graph
            .constant_value(function)
            .and_then(|bits| self.has_instance.get(&bits).copied())
        else {
            return Ok(false);
        };
        if !self.may_speculate(ExitKind::BadShape) || !self.may_speculate(ExitKind::BadType) {
            return Ok(false);
        }
        let function = self.check_shapes(
            function,
            &[ShapeCheck {
                shape: has_instance.shape,
                dictionary_generation: has_instance.dictionary_generation,
            }],
        );
        let prototype = self.load_named(function, has_instance.prototype_offset);
        let value = self.read(lhs)?;
        let result = self.emit_checked(Op::HasInPrototypeChain, vec![value, prototype], Some(Repr::Bool));
        let boxed = self.emit(Op::BoxBool, vec![result], Some(Repr::Tagged));
        self.write(dst, boxed)?;
        Ok(true)
    }

    /// The global object, while no binding of the global declarative
    /// environment shadows its properties.
    fn global_object(&mut self, globals: Globals) -> NodeId {
        self.assume_global_declarations(globals);
        let object = self.boxed_object(globals.object);
        self.constant(object)
    }

    /// Depends on the global declarative environment getting no bindings,
    /// which may shadow properties of the global object, and embeds the
    /// global object, which properties the code depends on belong to.
    fn assume_global_declarations(&mut self, globals: Globals) {
        let environment = globals.declarative_environment;
        self.embed(environment);
        self.assume(Dependency::GlobalDeclarations {
            environment,
            serial: globals.environment_serial,
        });
        self.embed(globals.object);
    }

    /// The value of a global variable that keeps it, as a constant.
    fn global_constant(&mut self, value: GlobalValueSnapshot) -> NodeId {
        if let Some(cell) = value.cell {
            self.embed(cell);
        }
        if let Some(intrinsic) = value.intrinsic {
            self.intrinsics.insert(value.bits, intrinsic);
        }
        if let Some(has_instance) = value.has_instance {
            self.has_instance.insert(value.bits, has_instance);
        }
        self.constant(value.bits)
    }

    /// `loaded`, the value of a global variable, as the constant `value` it
    /// held at compile time if that was an object: the load is checked
    /// against it.
    fn speculate_global_value(&mut self, loaded: NodeId, value: GlobalValueSnapshot) -> NodeId {
        if tag(value.bits) != OBJECT_TAG || !self.may_speculate(ExitKind::UnexpectedValue) {
            return loaded;
        }
        self.emit_checked(
            Op::CheckValue {
                expected: value.bits,
                kind: ExitKind::UnexpectedValue,
            },
            vec![loaded],
            None,
        );
        self.global_constant(value)
    }
}
