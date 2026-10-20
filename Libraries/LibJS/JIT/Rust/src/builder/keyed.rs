/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Keyed accesses that speculation does not cover, built as IR for the
//! cases the interpreter's keyed feedback saw.
//!
//! A `GetByValue` with an int32 key branches on the elements kind of its
//! object, one kind its feedback saw after the other, and loads the element:
//! bounds checks against the element counts, the load, and for holey
//! elements a check for a hole, each a node of its own. Sites that saw
//! holes or indices past the end read undefined there, where the prototype
//! chain has no indexed properties. Strings give the
//! strings of their ASCII characters. Other keys probe the property lookup
//! caches. Everything else takes the instruction's slow path, and all paths
//! join after the instruction. `PutByValue` is built the same way, and
//! fills holes of arrays where that runs no code.

use super::Flow;
use super::GenericInfo;
use super::GraphBuilder;
use super::checks::SlowPaths;
use crate::CompileFailure;
use crate::bytecode::Instruction;
use crate::bytecode::Operand;
use crate::bytecode::PUT_KIND_NORMAL;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::ir::BinaryOp;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::Comparison;
use crate::ir::ElementsKind;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::TypedArrayElement;
use crate::ir::value;
use crate::snapshot::Forwarding;
use crate::snapshot::Intrinsic;
use crate::snapshot::keyed_feedback_bits::HOLEY;
use crate::snapshot::keyed_feedback_bits::INT32_INDEX;
use crate::snapshot::keyed_feedback_bits::KEY_KINDS_MASK;
use crate::snapshot::keyed_feedback_bits::OTHER_ELEMENTS;
use crate::snapshot::keyed_feedback_bits::OTHER_KEY;
use crate::snapshot::keyed_feedback_bits::OUT_OF_BOUNDS;
use crate::snapshot::keyed_feedback_bits::PACKED;
use crate::snapshot::keyed_feedback_bits::STRING_KEY;
use crate::snapshot::keyed_feedback_bits::SYMBOL_KEY;
use crate::snapshot::keyed_feedback_bits::TYPED_ARRAY_SHIFT;

/// What a site is built for without keyed feedback: every key and every
/// elements kind, like the interpreter handles them inline.
const UNKNOWN_KEYED_FEEDBACK: u32 =
    INT32_INDEX | PACKED | HOLEY | STRING_KEY | SYMBOL_KEY | OTHER_KEY | (0x1ff << TYPED_ARRAY_SHIFT);

/// Where a keyed access continues for each kind of key: the block for
/// int32 keys, with the index, and the block for other keys, with the key.
#[derive(Default)]
struct KeyPaths {
    indexed: Option<(BlockId, NodeId)>,
    named: Option<(BlockId, NodeId)>,
}

impl GraphBuilder<'_> {
    /// What the keyed feedback of `instruction` saw, or everything if it saw
    /// nothing.
    fn keyed_access_bits(&self, instruction: &Instruction) -> u32 {
        match self.keyed_feedback_bits(instruction) {
            Some(bits) if bits != 0 => bits,
            _ => UNKNOWN_KEYED_FEEDBACK,
        }
    }

    /// The elements kinds keyed feedback `bits` saw for int32 keys.
    fn elements_kinds_seen(&self, bits: u32) -> Vec<ElementsKind> {
        let mut kinds = Vec::new();
        if bits & INT32_INDEX != 0 && bits & PACKED != 0 {
            kinds.push(ElementsKind::Packed);
        }
        if bits & INT32_INDEX != 0 && bits & HOLEY != 0 {
            kinds.push(ElementsKind::Holey);
        }
        kinds.extend(
            self.typed_array_elements_seen(bits)
                .into_iter()
                .map(ElementsKind::TypedArray),
        );
        kinds
    }

    /// Splits the paths of a keyed access by its key: int32 keys (if
    /// `indexed`) continue with their index, and other keys (if `named`)
    /// with the key, each in a block of its own if a key may be either. Keys
    /// of kinds the feedback never saw take a slow path. Returns `None`,
    /// having built nothing, if keys like `key` take no path.
    fn split_key(&mut self, slow_paths: &mut SlowPaths, key: NodeId, indexed: bool, named: bool) -> Option<KeyPaths> {
        if !indexed && !named {
            return None;
        }
        let mut paths = KeyPaths::default();
        if let Some(index) = self.int32_value_of(key) {
            paths.indexed = Some((self.block, index));
            return indexed.then_some(paths);
        }
        let key = self.tagged(key);
        if let Some(index) = self.graph.constant_value(key).and_then(value::as_int32) {
            paths.indexed = Some((self.block, self.int32_constant(index)));
            return indexed.then_some(paths);
        }
        if self.graph.constant_value(key).is_some() {
            paths.named = Some((self.block, key));
            return named.then_some(paths);
        }
        let integer = if !named {
            self.branch_to_slow_path(slow_paths, BranchCondition::Int32Value, vec![key], true)
        } else if indexed {
            let (integer, other) = self.branch_both_ways(BranchCondition::Int32Value, vec![key]);
            paths.named = Some((other, key));
            self.block = integer;
            self.refine(BranchCondition::Int32Value, true, vec![key])
        } else {
            // NB: The cache lookup misses for int32 keys.
            paths.named = Some((self.block, key));
            return Some(paths);
        };
        let index = self.emit(Op::UnboxInt32, vec![integer], Some(Repr::Int32));
        paths.indexed = Some((self.block, index));
        Some(paths)
    }

    /// The storage of the elements of `kind` of the object at `address` (see
    /// `element_storage_and_bound()`), and `index`, refined to be in their
    /// bounds, taking a slow path where it is not.
    pub(super) fn element_index_in_bounds(
        &mut self,
        slow_paths: &mut SlowPaths,
        kind: ElementsKind,
        address: NodeId,
        index: NodeId,
    ) -> (NodeId, NodeId) {
        let (storage, bound) = self.element_storage_and_bound(kind, address);
        let index = self.branch_to_slow_path(slow_paths, BranchCondition::IndexInBounds, vec![index, bound], true);
        (storage, index)
    }

    /// `index`, refined to be in the bounds of the packed elements of the
    /// array `object` (at `address`), taking a slow path where it is not,
    /// except for the index of the next element: there, `value` becomes the
    /// next element like `Array.prototype.push` would append it, growing
    /// the elements where they have no room for it, and the access is done.
    pub(super) fn element_index_or_append(
        &mut self,
        slow_paths: &mut SlowPaths,
        object: NodeId,
        address: NodeId,
        index: NodeId,
        value: NodeId,
    ) -> NodeId {
        let length = self.emit(Op::LoadElementsLength, vec![address], Some(Repr::Int32));
        let (inside, outside) = self.branch_both_ways(BranchCondition::IndexInBounds, vec![index, length]);
        self.block = outside;
        let next = BranchCondition::Int32(Comparison::StrictlyEquals);
        let next_index = self.branch_to_slow_path(slow_paths, next, vec![index, length], true);
        let array = self.emit_checked(Op::CheckAppendableArray, vec![object], None);
        let array_address = self.cell_address(array);
        let capacity = self.emit(Op::LoadElementsCapacity, vec![array_address], Some(Repr::Int32));
        // NB: Lengths above i32::MAX are negative as int32 values, which an
        //     unsigned comparison keeps from passing as room for an element.
        //     Appends without room grow the elements through the runtime,
        //     which runs no code either, but at such lengths, whose keys are
        //     no indices, which the slow path stores.
        let room = BranchCondition::IndexInBounds;
        let (hot, cold) = self.branch_with_cold_side(room, vec![next_index, capacity], true);
        self.block = cold;
        let zero = self.int32_constant(0);
        let nonnegative = BranchCondition::Int32(Comparison::GreaterThanEquals);
        self.branch_to_slow_path(slow_paths, nonnegative, vec![next_index, zero], true);
        self.graph.blocks[self.block.index()].is_cold = true;
        self.emit(Op::CallArrayPush, vec![array, value], Some(Repr::Tagged));
        self.end_hot_path(slow_paths, Vec::new());
        self.block = hot;
        let next_index = self.refine(room, true, vec![next_index, capacity]);
        self.emit(
            Op::AppendElement,
            vec![array_address, next_index, value],
            Some(Repr::Int32),
        );
        self.end_hot_path(slow_paths, Vec::new());

        self.block = inside;
        let index = self.refine(BranchCondition::IndexInBounds, true, vec![index, length]);
        let capacity = self.emit(Op::LoadElementsCapacity, vec![address], Some(Repr::Int32));
        self.branch_to_slow_path(slow_paths, BranchCondition::IndexInBounds, vec![index, capacity], true)
    }

    /// Ends the current block, where the object at `address` has no element
    /// at a non-negative index, as a path that read undefined, if its
    /// prototype chain has no indexed properties. Takes the slow path
    /// otherwise.
    fn read_missing_element(&mut self, slow_paths: &mut SlowPaths, address: NodeId) {
        self.branch_to_slow_path(
            slow_paths,
            BranchCondition::PlainArrayPrototypeChain,
            vec![address],
            true,
        );
        let undefined = self.constant(value::UNDEFINED);
        self.end_hot_path(slow_paths, vec![undefined]);
    }

    /// Builds `GetByValue` (`Handling::Expanded`) for the keys and elements
    /// its feedback saw.
    pub(super) fn build_get_by_value(
        &mut self,
        instruction: &Instruction,
        info: GenericInfo,
    ) -> Result<Flow, CompileFailure> {
        let Instruction::GetByValue {
            dst,
            base,
            property,
            cache,
            ..
        } = *instruction
        else {
            unreachable!("only GetByValue is built here");
        };
        let bits = self.keyed_access_bits(instruction);
        let kinds = self.elements_kinds_seen(bits);
        let strings = bits & OTHER_KEY != 0 && self.runtime.layout.single_ascii_character_strings != 0;
        let indexed = !kinds.is_empty() || strings;
        let named = bits & (STRING_KEY | SYMBOL_KEY) != 0;
        // NB: Holes and indices past the end read undefined where the
        //     prototype chain is the realm's default one, without indexed
        //     properties. Sites that saw any are built to read them.
        let reads_missing_elements = bits & OUT_OF_BOUNDS != 0;
        if reads_missing_elements {
            self.embed(self.runtime.array_prototype);
            self.embed(self.runtime.object_prototype);
        }
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let object = self.read(base)?;
        let key = self.read(property)?;
        let Some(paths) = self.split_key(&mut slow_paths, key, indexed, named) else {
            return self.build_generic(instruction, info);
        };

        if let Some((block, index)) = paths.indexed {
            self.block = block;
            let conditions = kinds
                .iter()
                .copied()
                .map(BranchCondition::ElementsKind)
                .collect::<Vec<_>>();
            self.branch_on_cases(
                &mut slow_paths,
                object,
                &conditions,
                strings,
                |builder, slow_paths, case, object| {
                    let kind = kinds[case];
                    let address = builder.cell_address(object);
                    if !reads_missing_elements || matches!(kind, ElementsKind::TypedArray(_)) {
                        let (storage, index) = builder.element_index_in_bounds(slow_paths, kind, address, index);
                        let element = builder.load_element(kind, storage, index);
                        let mut element = builder.tagged(element);
                        if kind == ElementsKind::Holey {
                            element = builder.branch_if_empty(slow_paths, element);
                        }
                        builder.end_hot_path(slow_paths, vec![element]);
                        return;
                    }
                    let (storage, bound) = builder.element_storage_and_bound(kind, address);
                    let (inside, outside) =
                        builder.branch_both_ways(BranchCondition::IndexInBounds, vec![index, bound]);
                    builder.block = outside;
                    let zero = builder.int32_constant(0);
                    builder.branch_to_slow_path(
                        slow_paths,
                        BranchCondition::Int32(Comparison::GreaterThanEquals),
                        vec![index, zero],
                        true,
                    );
                    builder.read_missing_element(slow_paths, address);
                    builder.block = inside;
                    let index = builder.refine(BranchCondition::IndexInBounds, true, vec![index, bound]);
                    let element = builder.load_element(kind, storage, index);
                    let element = builder.tagged(element);
                    if kind == ElementsKind::Holey {
                        let empty = builder.constant(value::EMPTY);
                        let (hole, present) = builder
                            .branch_both_ways(BranchCondition::TaggedEquals { equal: true }, vec![element, empty]);
                        builder.block = hole;
                        builder.read_missing_element(slow_paths, address);
                        builder.block = present;
                        let element = builder.refine(
                            BranchCondition::TaggedEquals { equal: true },
                            false,
                            vec![element, empty],
                        );
                        builder.end_hot_path(slow_paths, vec![element]);
                    } else {
                        builder.end_hot_path(slow_paths, vec![element]);
                    }
                },
            );
            // NB: The feedback counts the characters of strings as other keys.
            if strings {
                let code_unit = self.string_code_unit(&mut slow_paths, object, index);
                let character = self.ascii_character_string(&mut slow_paths, code_unit);
                self.end_hot_path(&mut slow_paths, vec![character]);
            }
        }
        if let Some((block, key)) = paths.named {
            self.block = block;
            let executable = self.function.index;
            let value = self.emit(
                Op::ProbeKeyedCache { executable, cache },
                vec![object, key],
                Some(Repr::Tagged),
            );
            let value = self.branch_if_empty(&mut slow_paths, value);
            self.end_hot_path(&mut slow_paths, vec![value]);
        }
        let result = self.join_hot_paths(slow_paths);
        self.write(dst, result.expect("GetByValue has a value"))?;
        Ok(Flow::Continue)
    }

    /// Builds `PutByValue` (`Handling::Expanded`) for the keys and elements
    /// its feedback saw. Puts other than plain assignments run generically.
    pub(super) fn build_put_by_value(
        &mut self,
        instruction: &Instruction,
        info: GenericInfo,
    ) -> Result<Flow, CompileFailure> {
        let Instruction::PutByValue {
            base,
            property,
            src,
            kind: PUT_KIND_NORMAL,
            cache,
            ..
        } = *instruction
        else {
            return self.build_generic(instruction, info);
        };
        let bits = self.keyed_access_bits(instruction);
        let kinds = self.elements_kinds_seen(bits);
        let named = bits & (STRING_KEY | SYMBOL_KEY) != 0;
        let mut slow_paths = self.start_slow_paths(None)?;
        // NB: Stores of int32 indices into packed or holey elements fill holes and append inline, so their slow path
        //     only stores out of bounds otherwise, which is rare, as are stores out of the bounds of typed arrays
        //     where the feedback saw none. Exiting there instead keeps loops that store elements free of code that
        //     may change what they read. The exits are out of bounds ones, so that taking one keeps inline appends,
        //     which go once their check of the array exits with `ExitKind::SlowPath`. Where that happened, appends
        //     take the slow path, which they would otherwise exit at too.
        let appends = self.may_speculate(ExitKind::SlowPath);
        let out_of_bounds_typed_arrays = !self.typed_array_elements_seen(bits).is_empty() && bits & OUT_OF_BOUNDS != 0;
        if appends
            && bits & KEY_KINDS_MASK == INT32_INDEX
            && bits & OTHER_ELEMENTS == 0
            && !kinds.is_empty()
            && !out_of_bounds_typed_arrays
        {
            self.exit_with_instead_of_slow_path(&mut slow_paths, ExitKind::OutOfBounds);
        }
        let object = self.read(base)?;
        let key = self.read(property)?;
        let value = self.read(src)?;
        let Some(paths) = self.split_key(&mut slow_paths, key, !kinds.is_empty(), named) else {
            return self.build_generic(instruction, info);
        };

        // NB: Appends and hole fills check that the prototype chain is the
        //     realm's default one. Appends exit for arrays that push could
        //     not append to, and hole fills take the slow path.
        if appends || kinds.contains(&ElementsKind::Holey) {
            self.embed(self.runtime.array_prototype);
            self.embed(self.runtime.object_prototype);
        }
        if let Some((block, index)) = paths.indexed {
            self.block = block;
            let conditions = kinds
                .iter()
                .copied()
                .map(BranchCondition::ElementsKind)
                .collect::<Vec<_>>();
            self.branch_on_cases(
                &mut slow_paths,
                object,
                &conditions,
                false,
                |builder, slow_paths, case, object| {
                    let kind = kinds[case];
                    let stored = builder.stored_element(slow_paths, kind, value);
                    let address = builder.cell_address(object);
                    if kind == ElementsKind::Holey {
                        builder.store_holey_element(slow_paths, address, index, stored);
                        return;
                    }
                    let (storage, index) = if kind == ElementsKind::Packed && appends {
                        let index = builder.element_index_or_append(slow_paths, object, address, index, stored);
                        (address, index)
                    } else {
                        builder.element_index_in_bounds(slow_paths, kind, address, index)
                    };
                    builder.store_element(kind, storage, index, stored);
                    builder.end_hot_path(slow_paths, Vec::new());
                },
            );
        }
        if let Some((block, key)) = paths.named {
            self.block = block;
            let executable = self.function.index;
            let stored = self.emit(
                Op::ProbeKeyedStore { executable, cache },
                vec![object, key, value],
                Some(Repr::Bool),
            );
            self.branch_to_slow_path(&mut slow_paths, BranchCondition::Bool, vec![stored], true);
            self.end_hot_path(&mut slow_paths, Vec::new());
        }
        self.join_hot_paths(slow_paths);
        Ok(Flow::Continue)
    }

    /// Stores `value` at `index` of the holey elements of the object at
    /// `address`, taking a slow path unless the index is below their length.
    /// A hole takes a new element, and elements without room for it grow,
    /// which runs no code where the object is an array with a plain prototype
    /// chain (see `BranchCondition::HoleFillableArray`); other holes take a
    /// slow path.
    fn store_holey_element(&mut self, slow_paths: &mut SlowPaths, address: NodeId, index: NodeId, value: NodeId) {
        let kind = ElementsKind::Holey;
        let in_bounds = BranchCondition::IndexInBounds;
        let length = self.emit(Op::LoadElementsLength, vec![address], Some(Repr::Int32));
        let index = self.branch_to_slow_path(slow_paths, in_bounds, vec![index, length], true);
        // NB: Elements made with a length, like `new Array(length)`, have no room yet, and grow as they get filled.
        let capacity = self.emit(Op::LoadElementsCapacity, vec![address], Some(Repr::Int32));
        let (room, no_room) = self.branch_with_cold_side(in_bounds, vec![index, capacity], true);
        self.block = no_room;
        // NB: Lengths above i32::MAX let negative int32 keys, which are no indices, pass as below the length.
        let zero = self.int32_constant(0);
        let nonnegative = BranchCondition::Int32(Comparison::GreaterThanEquals);
        let element_index = self.branch_to_slow_path(slow_paths, nonnegative, vec![index, zero], true);
        self.graph.blocks[self.block.index()].is_cold = true;
        let array = self.branch_to_slow_path(slow_paths, BranchCondition::HoleFillableArray, vec![address], true);
        self.graph.blocks[self.block.index()].is_cold = true;
        self.emit(Op::CallStoreHoleyElement, vec![array, element_index, value], None);
        self.end_hot_path(slow_paths, Vec::new());

        self.block = room;
        let index = self.refine(in_bounds, true, vec![index, capacity]);
        let old = self.emit(Op::LoadElementAt { kind }, vec![address, index], Some(Repr::Tagged));
        let empty = self.constant(value::EMPTY);
        let (hole, element) = self.branch_both_ways(BranchCondition::TaggedEquals { equal: true }, vec![old, empty]);

        self.block = hole;
        let array = self.branch_to_slow_path(slow_paths, BranchCondition::HoleFillableArray, vec![address], true);
        // NB: A store to the last index may make the elements packed, which
        //     the runtime does. The index is below the length, so the length
        //     is not 0.
        let one = self.int32_constant(1);
        let last = self.emit(
            Op::Int32WrappingBinary { op: BinaryOp::Sub },
            vec![length, one],
            Some(Repr::Int32),
        );
        let (before_last, at_last) = self.branch_with_cold_side(in_bounds, vec![index, last], true);
        self.block = at_last;
        self.emit(Op::CallStoreHoleyElement, vec![array, index, value], None);
        self.end_hot_path(slow_paths, Vec::new());
        self.block = before_last;
        let index_before_last = self.refine(in_bounds, true, vec![index, last]);
        self.store_element(kind, address, index_before_last, value);
        self.end_hot_path(slow_paths, Vec::new());

        self.block = element;
        self.store_element(kind, address, index, value);
        self.end_hot_path(slow_paths, Vec::new());
    }

    /// `value` as elements of `kind` store it, taking a slow path for
    /// values of typed arrays that are no numbers.
    fn stored_element(&mut self, slow_paths: &mut SlowPaths, kind: ElementsKind, value: NodeId) -> NodeId {
        let int32 = self.int32_value_of(value);
        match (kind.stored_repr(), int32) {
            (Repr::Int32, Some(int32)) => int32,
            (Repr::Float64, Some(int32)) => self.int32_to_float64(int32),
            (Repr::Int32, None) => {
                let number = self.float64_of_number(slow_paths, value);
                let conversion = if kind == ElementsKind::TypedArray(TypedArrayElement::Uint8Clamped) {
                    Op::Float64ToUint8Clamped
                } else {
                    Op::Float64ToInt32
                };
                self.emit(conversion, vec![number], Some(Repr::Int32))
            }
            (Repr::Float64, None) => self.float64_of_number(slow_paths, value),
            _ => self.tagged(value),
        }
    }

    /// Builds `object.hasOwnProperty(key)` and `hasOwnProperty.call(object,
    /// key)` calls of `Object.prototype.hasOwnProperty` as a probe of
    /// whether the object has the property as its own, which exits where
    /// the probe cannot tell. Returns `None`, having built nothing, if the
    /// `Call` is not such a call.
    pub(super) fn try_build_has_own_property(
        &mut self,
        instruction: &Instruction,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Instruction::Call {
            call_feedback,
            dst,
            callee,
            this_value,
            ref arguments,
            ..
        } = *instruction
        else {
            return Ok(None);
        };
        if !self.may_speculate(ExitKind::BadCallTarget)
            || !self.may_speculate(ExitKind::SlowPath)
            || arguments
                .iter()
                .chain([&this_value])
                .any(|operand| self.holds_virtual_arguments(*operand))
        {
            return Ok(None);
        }
        let Some(feedback) = self.call_feedback(call_feedback) else {
            return Ok(None);
        };
        let (object, key) = match (arguments.as_slice(), feedback.monomorphic_intrinsic()) {
            ([key], Some((target, Intrinsic::ObjectPrototypeHasOwnProperty))) => {
                self.check_callee(callee, target)?;
                (this_value, *key)
            }
            // NB: `Function.prototype.call` forwarding to hasOwnProperty.
            ([object, key], Some((call, Intrinsic::FunctionPrototypeCall)))
                if feedback.forwarded.is_some_and(|forwarded| {
                    forwarded.forwarding == Forwarding::Call
                        && forwarded.target_intrinsic == Some(Intrinsic::ObjectPrototypeHasOwnProperty)
                }) =>
            {
                let forwarded = feedback.forwarded.expect("checked above");
                self.check_callee(callee, call)?;
                self.check_callee(this_value, forwarded.target)?;
                (*object, *key)
            }
            _ => return Ok(None),
        };
        // NB: The call's slow path is a call, which cold blocks cannot make,
        //     so the code exits where the probe cannot tell.
        let has = self.probe_has_property(true, object, key)?;
        let has = self.exit_if_empty(has, ExitKind::SlowPath);
        self.write(dst, has)?;
        Ok(Some(Flow::Continue))
    }

    /// Builds `dst = lhs in rhs` as a probe of whether the object has the
    /// property, and the instruction's slow path where the probe cannot
    /// tell. Returns `None`, having built nothing, for virtual arguments
    /// objects.
    pub(super) fn try_build_in(
        &mut self,
        dst: Operand,
        lhs: Operand,
        rhs: Operand,
    ) -> Result<Option<Flow>, CompileFailure> {
        if self.holds_virtual_arguments(lhs) || self.holds_virtual_arguments(rhs) {
            return Ok(None);
        }
        let mut slow_paths = self.start_slow_paths(Some(Repr::Tagged))?;
        let has = self.probe_has_property(false, rhs, lhs)?;
        let has = self.branch_if_empty(&mut slow_paths, has);
        let result = self.join_slow_paths(slow_paths, Some(has));
        self.write(dst, result.expect("In has a value"))?;
        Ok(Some(Flow::Continue))
    }

    /// Whether `object` has the property `key` names (as its own, if
    /// `own`), or the empty value where `ProbeHasProperty` cannot tell.
    fn probe_has_property(&mut self, own: bool, object: Operand, key: Operand) -> Result<NodeId, CompileFailure> {
        let object = self.read(object)?;
        let key = self.read(key)?;
        Ok(self.emit(Op::ProbeHasProperty { own }, vec![object, key], Some(Repr::Tagged)))
    }
}
