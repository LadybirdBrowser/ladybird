/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Allocation in compiled code.
//!
//! `NewObject` becomes an `AllocateObject` node with the shape the
//! interpreter would give the object: the shape its object literal had the
//! last time (from the executable's object shape cache), or the realm's
//! empty object shape. An object literal with such a shape already has every
//! property it gets, so its `InitObjectLiteralProperty` instructions become
//! `InitializeNamed` stores to the offsets the cache recorded, and its
//! `CacheObjectShape` does nothing, as the cache already has a shape.
//!
//! Nothing but the literal's own instructions can see its object before the
//! literal is complete (the object is only in the literal's destination
//! register, and the methods it defines cannot run yet), so its shape stays
//! the one it was allocated with until then, whatever runs in between.

use super::GraphBuilder;
use crate::CompileFailure;
use crate::bytecode::Operand;
use crate::code::Repr;
use crate::ir::FrameField;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::snapshot::ObjectShapeCacheSnapshot;
use crate::snapshot::ShapeSnapshot;

/// `NewObject` without an object shape cache.
const NO_OBJECT_SHAPE_CACHE: u32 = u32::MAX;

impl GraphBuilder<'_> {
    fn object_shape_cache(&self, cache: u32) -> Option<&ObjectShapeCacheSnapshot> {
        self.function
            .executable
            .object_shape_caches
            .get(cache as usize)
            .and_then(Option::as_ref)
    }

    /// The `AllocateObject` node `object` is, with the shape of object shape
    /// cache `cache`, if it is one.
    fn literal_allocated_with_cache_shape(&self, object: NodeId, cache: u32) -> Option<&ObjectShapeCacheSnapshot> {
        let cached = self.object_shape_cache(cache)?;
        match self.graph.node(object).op {
            Op::AllocateObject { shape, .. } if shape == cached.shape.shape => Some(cached),
            _ => None,
        }
    }

    /// Builds `NewObject` as an `AllocateObject`. Returns false, having built
    /// nothing, if the snapshot does not say which shape the object gets.
    pub(super) fn try_build_new_object(&mut self, dst: Operand, cache: u32) -> Result<bool, CompileFailure> {
        let cached = (cache != NO_OBJECT_SHAPE_CACHE)
            .then(|| self.object_shape_cache(cache).map(|cached| cached.shape))
            .flatten();
        let Some(ShapeSnapshot { shape, property_count }) = cached.or(self.function.executable.new_object_shape) else {
            return Ok(false);
        };
        self.embed(shape);
        let object = self.emit(
            Op::AllocateObject {
                shape,
                property_count,
                reserve: property_count,
            },
            Vec::new(),
            Some(Repr::Tagged),
        );
        self.write(dst, object)?;
        Ok(true)
    }

    /// Builds `InitObjectLiteralProperty` on an object literal allocated
    /// with its cached shape as a store to the property's offset. Returns
    /// false, having built nothing, otherwise.
    pub(super) fn try_build_init_object_literal_property(
        &mut self,
        object: Operand,
        src: Operand,
        shape_cache_index: u32,
        property_slot: u32,
    ) -> Result<bool, CompileFailure> {
        let object = self.read(object)?;
        let Some(offset) = self
            .literal_allocated_with_cache_shape(object, shape_cache_index)
            .and_then(|cached| cached.property_offsets.get(property_slot as usize).copied())
        else {
            return Ok(false);
        };
        let value = self.read(src)?;
        self.emit(Op::InitializeNamed { offset }, vec![object, value], None);
        Ok(true)
    }

    /// Builds `CacheObjectShape` of an object literal allocated with its
    /// cached shape, which has nothing to do. Returns false, having built
    /// nothing, otherwise.
    pub(super) fn try_build_cache_object_shape(&mut self, object: Operand, cache: u32) -> Result<bool, CompileFailure> {
        let object = self.read(object)?;
        Ok(self.literal_allocated_with_cache_shape(object, cache).is_some())
    }
}

/// The most elements an array literal built as an `AllocateArray` has; the
/// storage of larger ones is not a cell.
const MAX_ARRAY_LITERAL_ELEMENTS: usize = 1024;

/// An element of an array literal.
pub(super) enum ArrayElement {
    Operand(Operand),
    Constant(u64),
}

impl GraphBuilder<'_> {
    /// Builds `NewArray` or `NewPrimitiveArray` as an `AllocateArray` and
    /// the stores of its elements. Returns false, having built nothing, for
    /// arrays with too many elements.
    pub(super) fn try_build_new_array(
        &mut self,
        dst: Operand,
        elements: &[ArrayElement],
    ) -> Result<bool, CompileFailure> {
        // NB: The runtime always allocates arrays; the codegen tests of slow
        //     path conventions leave its allocation out to run NewArray's.
        if elements.len() > MAX_ARRAY_LITERAL_ELEMENTS || self.runtime.array_allocation.slow_path == 0 {
            return Ok(false);
        }
        let values = elements
            .iter()
            .map(|element| match element {
                ArrayElement::Operand(operand) => self.read(*operand),
                ArrayElement::Constant(bits) => Ok(self.constant(*bits)),
            })
            .collect::<Result<Vec<_>, _>>()?;
        // NB: Holes (the empty value) make the elements holey, not packed.
        if values
            .iter()
            .any(|value| self.graph.constant_value(*value) == Some(crate::ir::value::EMPTY))
        {
            return Ok(false);
        }
        let shape = self.runtime.array_allocation.shape;
        if shape.0 != 0 {
            self.embed(shape);
        }
        let count = u32::try_from(values.len()).expect("array literals have fewer than 2^32 elements");
        let array = self.emit(Op::AllocateArray { count }, Vec::new(), Some(Repr::Tagged));
        for (index, value) in (0u32..).zip(values) {
            self.emit(Op::InitializeElement { index }, vec![array, value], None);
        }
        self.write(dst, array)?;
        Ok(true)
    }
}

impl GraphBuilder<'_> {
    /// Builds `NewFunction` of the compiled function as an
    /// `AllocateFunction`, if it has a closure template and creates no
    /// method. Returns false, having built nothing, otherwise.
    pub(super) fn try_build_new_function(
        &mut self,
        dst: Operand,
        shared_function_data_index: u32,
        has_home_object: bool,
    ) -> Result<bool, CompileFailure> {
        if self.function.is_virtual() || has_home_object {
            return Ok(false);
        }
        let Some(sample) = self
            .function
            .executable
            .closure_templates
            .get(shared_function_data_index as usize)
            .and_then(Option::as_ref)
            .map(|closure| closure.sample)
        else {
            return Ok(false);
        };
        self.embed(sample);
        let environment = self.frame_field(FrameField::LexicalEnvironment)?;
        let private_environment = self.frame_field(FrameField::PrivateEnvironment)?;
        let function = self.emit(
            Op::AllocateFunction {
                shared_function_data_index,
            },
            vec![environment, private_environment],
            Some(Repr::Tagged),
        );
        self.write(dst, function)?;
        Ok(true)
    }
}

impl GraphBuilder<'_> {
    /// Builds `CreateLexicalEnvironment` of the compiled function as an
    /// `AllocateEnvironment` that becomes the frame's lexical environment, if
    /// its shape cache has a template for environments with `capacity`
    /// bindings. Returns false, having built nothing, otherwise.
    pub(super) fn try_build_create_lexical_environment(
        &mut self,
        dst: Operand,
        parent: Operand,
        capacity: u32,
        shape_cache: u32,
    ) -> Result<bool, CompileFailure> {
        if self.function.is_virtual() {
            return Ok(false);
        }
        let Some(template) = self
            .function
            .executable
            .lexical_environment_templates
            .get(shape_cache as usize)
            .and_then(Option::as_ref)
        else {
            return Ok(false);
        };
        if template.binding_count != capacity {
            return Ok(false);
        }
        if let Some(shape) = template.shape {
            self.embed(shape);
        }
        let parent = self.read(parent)?;
        let environment = self.emit(
            Op::AllocateEnvironment { shape_cache, capacity },
            vec![parent],
            Some(Repr::Tagged),
        );
        let address = self.emit(Op::SetLexicalEnvironment, vec![environment], Some(Repr::Pointer));
        self.frame.set_field(FrameField::LexicalEnvironment, address);
        self.write(dst, environment)?;
        Ok(true)
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::snapshot_for;
    use super::*;
    use crate::bytecode::Instruction;
    use crate::bytecode::PropertyKeyTableIndex;
    use crate::bytecode::test_support::*;
    use crate::snapshot::CellId;

    const LITERAL_SHAPE: CellId = CellId(0x1000);
    const EMPTY_SHAPE: CellId = CellId(0x2000);

    /// `return { first: a0, second: 10 }` from object shape cache 0, then
    /// `{}` without a cache.
    fn literal_program() -> crate::bytecode::test_support::Program {
        assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::NewObject { dst: r(5), cache: 0 },
                Instruction::InitObjectLiteralProperty {
                    object: r(5),
                    property: PropertyKeyTableIndex(0),
                    src: a(0),
                    shape_cache_index: 0,
                    property_slot: 0,
                },
                Instruction::InitObjectLiteralProperty {
                    object: r(5),
                    property: PropertyKeyTableIndex(1),
                    src: c(1),
                    shape_cache_index: 0,
                    property_slot: 1,
                },
                Instruction::CacheObjectShape { object: r(5), cache: 0 },
                Instruction::NewObject {
                    dst: r(6),
                    cache: NO_OBJECT_SHAPE_CACHE,
                },
                Instruction::Return { value: r(5) },
            ]
        })
    }

    fn ops(graph: &crate::ir::Graph) -> Vec<Op> {
        graph
            .blocks
            .iter()
            .flat_map(|block| block.body.iter())
            .map(|node| graph.node(*node).op.clone())
            .filter(|op| {
                matches!(
                    op,
                    Op::AllocateObject { .. } | Op::InitializeNamed { .. } | Op::CallSlowPath { .. }
                )
            })
            .collect()
    }

    #[test]
    fn literals_with_a_cached_shape_are_allocated_and_initialized_in_place() {
        let program = literal_program();
        let mut snapshot = snapshot_for(&program, test_layout());
        snapshot.runtime.object_allocation.slow_path = 1;
        snapshot.executables[0].object_shape_caches = vec![Some(ObjectShapeCacheSnapshot {
            shape: ShapeSnapshot {
                shape: LITERAL_SHAPE,
                property_count: 2,
            },
            property_offsets: vec![1, 0],
        })];
        snapshot.executables[0].new_object_shape = Some(ShapeSnapshot {
            shape: EMPTY_SHAPE,
            property_count: 0,
        });
        let graph = crate::builder::build_graph(&snapshot).unwrap();
        assert_eq!(
            ops(&graph),
            vec![
                Op::AllocateObject {
                    shape: LITERAL_SHAPE,
                    property_count: 2,
                    reserve: 2
                },
                Op::InitializeNamed { offset: 1 },
                Op::InitializeNamed { offset: 0 },
                Op::AllocateObject {
                    shape: EMPTY_SHAPE,
                    property_count: 0,
                    reserve: 0
                },
            ]
        );
        assert!(graph.embedded_cells.contains(&LITERAL_SHAPE));
        assert!(graph.embedded_cells.contains(&EMPTY_SHAPE));
    }

    #[test]
    fn lexical_environments_with_a_template_are_allocated_in_place() {
        const SHAPE: CellId = CellId(0x3000);
        let program = assemble(|_| {
            vec![
                Instruction::Enter,
                Instruction::GetLexicalEnvironment { dst: r(5) },
                Instruction::CreateLexicalEnvironment {
                    dst: r(6),
                    parent: r(5),
                    capacity: 2,
                    shape_cache: 0,
                    is_catch_environment: false,
                },
                Instruction::CreateLexicalEnvironment {
                    dst: r(7),
                    parent: r(6),
                    capacity: 1,
                    shape_cache: 1,
                    is_catch_environment: false,
                },
                Instruction::Return { value: r(7) },
            ]
        });
        let mut snapshot = snapshot_for(&program, test_layout());
        snapshot.runtime.create_lexical_environment = 1;
        let template = crate::snapshot::LexicalEnvironmentTemplateSnapshot {
            allocator: 0x5000,
            cell_size: 88,
            words: vec![0; 9],
            binding_count: 2,
            inline_binding_values: true,
            binding_values_offset: 48,
            outer_offset: 16,
            shape: Some(SHAPE),
        };
        // NB: Cache 1 has no template.
        snapshot.executables[0].lexical_environment_templates = vec![Some(template), None];
        let graph = crate::builder::build_graph(&snapshot).unwrap();
        let ops = graph
            .blocks
            .iter()
            .flat_map(|block| block.body.iter())
            .map(|node| graph.node(*node).op.clone())
            .filter(|op| {
                matches!(
                    op,
                    Op::AllocateEnvironment { .. } | Op::SetLexicalEnvironment | Op::CallSlowPath { .. }
                )
            })
            .collect::<Vec<_>>();
        assert_eq!(
            ops,
            vec![
                Op::AllocateEnvironment {
                    shape_cache: 0,
                    capacity: 2
                },
                Op::SetLexicalEnvironment,
                Op::CallSlowPath {
                    opcode: crate::bytecode::OpCode::CreateLexicalEnvironment,
                    executable: 0,
                    pc: program.offsets[3],
                    saves_registers: false,
                },
            ]
        );
        assert!(graph.embedded_cells.contains(&SHAPE));
    }

    #[test]
    fn literals_without_a_cached_shape_initialize_through_the_slow_path() {
        let program = literal_program();
        let mut snapshot = snapshot_for(&program, test_layout());
        snapshot.runtime.object_allocation.slow_path = 1;
        snapshot.executables[0].object_shape_caches = vec![None];
        snapshot.executables[0].new_object_shape = Some(ShapeSnapshot {
            shape: EMPTY_SHAPE,
            property_count: 0,
        });
        let graph = crate::builder::build_graph(&snapshot).unwrap();
        let ops = ops(&graph);
        assert_eq!(
            ops.iter()
                .filter(|op| matches!(op, Op::AllocateObject { shape, .. } if *shape == EMPTY_SHAPE))
                .count(),
            2
        );
        assert_eq!(
            ops.iter()
                .filter(|op| matches!(op, Op::CallSlowPath { opcode, .. } if *opcode == crate::bytecode::OpCode::InitObjectLiteralProperty))
                .count(),
            2
        );
        assert!(!ops.iter().any(|op| matches!(op, Op::InitializeNamed { .. })));
    }
}
