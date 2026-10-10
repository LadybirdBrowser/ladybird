/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Bindings at static environment coordinates, built as IR.
//!
//! The environment a coordinate starts at is a frame field (see
//! `GraphBuilder::frame_field()`), and `LoadOuterEnvironment` walks the
//! chain, whose links never change. Bindings are read and written in place;
//! where one is uninitialized or immutable, the instruction's slow path runs
//! in a cold block, and throws or does what the interpreter does.

use super::Flow;
use super::GraphBuilder;
use crate::CompileFailure;
use crate::bytecode::EnvironmentCoordinate;
use crate::bytecode::Instruction;
use crate::bytecode::Operand;
use crate::code::Repr;
use crate::ir::BranchCondition;
use crate::ir::FrameField;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;

/// The coordinate of an instruction whose binding is not known statically.
const INVALID_HOPS: u32 = 0xFFFF_FFFE;

/// `JS::EnvironmentMode::Lexical`.
const ENVIRONMENT_MODE_LEXICAL: u32 = 0;

impl GraphBuilder<'_> {
    /// `field` of the running execution context. Inlined callees need no
    /// function environment and never change their environments, so their
    /// execution contexts are constants.
    pub(super) fn frame_field(&mut self, field: FrameField) -> Result<NodeId, CompileFailure> {
        // NB: The compiled function loads a field once per path, until an
        //     instruction may replace it.
        if !self.function.is_virtual() {
            if let Some(value) = self.frame.field(field) {
                return Ok(value);
            }
            let value = self.emit(Op::LoadFrameField { field }, Vec::new(), Some(Repr::Pointer));
            self.frame.set_field(field, value);
            return Ok(value);
        }
        // NB: Frames of inlined closures have the environments the closure
        //     was created with.
        if let Some(closure) = self.function.inline.as_ref().and_then(|call| call.closure) {
            let private = match field {
                FrameField::LexicalEnvironment | FrameField::VariableEnvironment => Some(false),
                FrameField::PrivateEnvironment => Some(true),
                FrameField::Realm | FrameField::Executable => None,
            };
            if let Some(private) = private {
                // NB: A closure the code creates itself has the environments
                //     it was created with as inputs.
                if let Op::AllocateFunction { .. } = self.graph.node(closure).op {
                    return Ok(self.graph.node(closure).inputs[usize::from(private)]);
                }
                let address = self.cell_address(closure);
                return Ok(self.emit(
                    Op::LoadFunctionEnvironment { private },
                    vec![address],
                    Some(Repr::Pointer),
                ));
            }
        }
        let executable = self.function.executable;
        let missing = CompileFailure::InvalidBytecode {
            pc: self.function.pc,
            reason: "inlined executable without a function",
        };
        let cell = match field {
            FrameField::LexicalEnvironment | FrameField::VariableEnvironment => executable.environment,
            FrameField::Realm => executable.function.map(|function| function.realm),
            FrameField::Executable => Some(executable.cell),
            // NB: Inlined callees never read it.
            FrameField::PrivateEnvironment => None,
        }
        .ok_or(missing)?;
        self.embed(cell);
        Ok(self.typed_constant(cell.0, Repr::Pointer))
    }

    /// Builds the binding instructions that have a static environment
    /// coordinate as IR, or returns `None`, having built nothing, for those
    /// that do not.
    pub(super) fn try_build_binding(&mut self, instruction: &Instruction) -> Result<Option<Flow>, CompileFailure> {
        match *instruction {
            Instruction::GetBinding { dst, cache, .. } => self.build_get_binding(&[dst], cache, false),
            Instruction::GetInitializedBinding { dst, cache, .. } => self.build_get_binding(&[dst], cache, true),
            Instruction::GetCalleeAndThisFromEnvironment {
                callee,
                this_value,
                cache,
                ..
            } => self.build_get_binding(&[callee, this_value], cache, false),
            Instruction::SetLexicalBinding { src, cache, .. } => {
                self.build_set_binding(FrameField::LexicalEnvironment, src, cache)
            }
            Instruction::SetVariableBinding { src, cache, .. } => {
                self.build_set_binding(FrameField::VariableEnvironment, src, cache)
            }
            Instruction::InitializeLexicalBinding { src, cache, .. } => {
                self.build_initialize_binding(FrameField::LexicalEnvironment, src, cache)
            }
            Instruction::InitializeVariableBinding { src, cache, .. } => {
                self.build_initialize_binding(FrameField::VariableEnvironment, src, cache)
            }
            Instruction::CreateVariable {
                identifier,
                mode,
                is_immutable,
                is_global: false,
                is_strict,
            } if !self.function.is_virtual() => {
                let field = if mode == ENVIRONMENT_MODE_LEXICAL {
                    FrameField::LexicalEnvironment
                } else {
                    FrameField::VariableEnvironment
                };
                self.build_create_variable(field, identifier.0, is_immutable, is_strict)
            }
            _ => Ok(None),
        }
    }

    /// The declarative environment `coordinate` names, from the environment
    /// in `field`, or `None` if the coordinate is not static.
    fn coordinate_environment(
        &mut self,
        field: FrameField,
        coordinate: EnvironmentCoordinate,
    ) -> Result<Option<NodeId>, CompileFailure> {
        if coordinate.hops == INVALID_HOPS {
            return Ok(None);
        }
        let mut environment = self.frame_field(field)?;
        for _ in 0..coordinate.hops {
            environment = self.emit(Op::LoadOuterEnvironment, vec![environment], Some(Repr::Pointer));
        }
        Ok(Some(environment))
    }

    /// GetBinding, GetInitializedBinding and GetCalleeAndThisFromEnvironment:
    /// the binding at `coordinate` from the lexical environment in the first
    /// of `outputs`, and `undefined` in the second one (the `this` value of
    /// GetCalleeAndThisFromEnvironment). Where the binding is uninitialized
    /// (unless `initialized` says it never is), the slow path throws.
    fn build_get_binding(
        &mut self,
        outputs: &[Operand],
        coordinate: EnvironmentCoordinate,
        initialized: bool,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Some(environment) = self.coordinate_environment(FrameField::LexicalEnvironment, coordinate)? else {
            return Ok(None);
        };
        let binding = self.emit(
            Op::LoadEnvironmentBinding {
                index: coordinate.index,
            },
            vec![environment],
            Some(Repr::Tagged),
        );
        // NB: Reading an uninitialized binding of a declarative environment
        //     throws a ReferenceError, so the instruction continues with the
        //     binding's value only.
        if !initialized {
            let empty = self.constant(value::EMPTY);
            self.build_throwing_check(
                BranchCondition::TaggedEquals { equal: true },
                vec![binding, empty],
                true,
            )?;
        }
        self.write(outputs[0], binding)?;
        if let Some(this_value) = outputs.get(1) {
            let undefined = self.constant(value::UNDEFINED);
            self.write(*this_value, undefined)?;
        }
        Ok(Some(Flow::Continue))
    }

    /// SetLexicalBinding and SetVariableBinding: `src` into the initialized
    /// mutable binding at `coordinate` from the environment in `field`. The
    /// slow path sets the others (or throws).
    fn build_set_binding(
        &mut self,
        field: FrameField,
        src: Operand,
        coordinate: EnvironmentCoordinate,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Some(environment) = self.coordinate_environment(field, coordinate)? else {
            return Ok(None);
        };
        let value = self.read(src)?;
        let mut slow_paths = self.start_slow_paths(None)?;
        self.branch_to_slow_path(
            &mut slow_paths,
            BranchCondition::BindingMutable {
                index: coordinate.index,
            },
            vec![environment],
            true,
        );
        let binding = self.emit(
            Op::LoadEnvironmentBinding {
                index: coordinate.index,
            },
            vec![environment],
            Some(Repr::Tagged),
        );
        let empty = self.constant(value::EMPTY);
        self.branch_to_slow_path(
            &mut slow_paths,
            BranchCondition::TaggedEquals { equal: false },
            vec![binding, empty],
            true,
        );
        self.emit(
            Op::StoreEnvironmentBinding {
                index: coordinate.index,
            },
            vec![environment, value],
            None,
        );
        self.join_slow_paths(slow_paths, None);
        Ok(Some(Flow::Continue))
    }

    /// CreateVariable in a declarative environment, the lexical or variable
    /// one (`field`): appends the binding the environment's shape has next,
    /// uninitialized, where it is the identifier's binding with the flags
    /// the instruction creates (like
    /// `DeclarativeEnvironment::create_next_binding_of_shape()`), and the
    /// slow path creates the binding otherwise.
    fn build_create_variable(
        &mut self,
        field: FrameField,
        identifier: u32,
        is_immutable: bool,
        is_strict: bool,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Some(name) = self.function.executable.identifiers.get(identifier as usize).copied() else {
            return Err(CompileFailure::InvalidBytecode {
                pc: self.function.pc,
                reason: "identifier out of range",
            });
        };
        let layout = self.runtime.layout;
        // NB: The flags of CreateImmutableBinding(name, is_strict) or
        //     CreateMutableBinding(name, is_strict).
        let flags = match (is_immutable, is_strict) {
            (true, true) => layout.binding_flag_strict,
            (true, false) => 0,
            (false, true) => layout.binding_flag_mutable | layout.binding_flag_can_be_deleted,
            (false, false) => layout.binding_flag_mutable,
        };
        let environment = self.frame_field(field)?;
        let mut slow_paths = self.start_slow_paths(None)?;
        self.branch_to_slow_path(
            &mut slow_paths,
            BranchCondition::NextBindingOfShape { name, flags },
            vec![environment],
            true,
        );
        self.emit(Op::AppendEnvironmentBinding, vec![environment], None);
        self.join_slow_paths(slow_paths, None);
        Ok(Some(Flow::Continue))
    }

    /// InitializeLexicalBinding and InitializeVariableBinding: `src` into
    /// the binding at `coordinate` from the environment in `field`, which
    /// never fails.
    fn build_initialize_binding(
        &mut self,
        field: FrameField,
        src: Operand,
        coordinate: EnvironmentCoordinate,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Some(environment) = self.coordinate_environment(field, coordinate)? else {
            return Ok(None);
        };
        let value = self.read(src)?;
        self.emit(
            Op::StoreEnvironmentBinding {
                index: coordinate.index,
            },
            vec![environment, value],
            None,
        );
        Ok(Some(Flow::Continue))
    }
}
