/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Speculative `GetById` and `PutById`, from what the interpreter's property
//! lookup caches have seen, in their guard and action form (see `caches`).
//!
//! A monomorphic or polymorphic cache whose entries all describe a property
//! of the object or of a prototype (for gets) or an existing writable own data
//! property (for puts) becomes an object check, a shape check and a direct
//! load or store of the property's slot. When the shapes keep the property at
//! different places, a shape switch dispatches to one load or store per shape.
//! Properties that held an accessor when the snapshot was taken are left to
//! inlined accessor calls or to the instruction's handling, and loads and
//! stores still exit for properties that hold one at run time.
//!
//! Methods found on a prototype become constants: the load of the property
//! is checked against the object the prototype held at compile time (a
//! valid prototype chain does not rule out assignments to the property), and
//! uses of it, like calls, see that object as a constant.
//!
//! Every access emits all of its checks and loads; check elimination (see
//! `passes`) removes those that earlier ones make redundant.
//!
//! The caches of keyed accesses (`GetByValue`, `PutByValue`) have one entry
//! per (shape, key) pair. With a key known at compile time, or a single key
//! the cache saw (checked at run time), they are built like named accesses.

use super::Flow;
use super::GraphBuilder;
use super::caches::Action;
use super::caches::CacheEntry;
use super::caches::Holder;
use crate::CompileFailure;
use crate::bytecode::Operand;
use crate::code::Dependency;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::ir::AccessorPart;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::ShapeCheck;
use crate::snapshot::CellId;

/// A check that a key is the one a keyed access speculates on: `key` must
/// be `expected`, the encoded key cell `cell`.
#[derive(Debug, Clone, Copy)]
pub(super) struct KeyCheck {
    key: NodeId,
    expected: u64,
    cell: CellId,
}

/// Cache entries of a keyed access for one key, and the check of the key.
type KeyedEntries = (Vec<CacheEntry>, Option<KeyCheck>);

impl GraphBuilder<'_> {
    /// The speculation gate: whether the current instruction may speculate
    /// in a way that exits with `kind`. A speculation that already failed at
    /// the same place is never made again.
    pub(super) fn may_speculate(&self, kind: ExitKind) -> bool {
        let executable = self.function.executable;
        if executable.builtin {
            let site = (executable.cell, self.function.pc, kind);
            return !self.snapshot.executables[0].builtin_exit_sites.contains(&site);
        }
        !executable.exit_sites.contains(&(self.function.pc, kind))
    }

    pub(super) fn embed(&mut self, cell: CellId) {
        if !self.graph.embedded_cells.contains(&cell) {
            self.graph.embedded_cells.push(cell);
        }
    }

    /// Emits a node that may exit with the current instruction's eager frame
    /// state. The value of a check is the input it refines (see
    /// `Op::refined_input()`).
    pub(super) fn emit_checked(&mut self, op: Op, inputs: Vec<NodeId>, repr: Option<Repr>) -> NodeId {
        let repr = repr.or_else(|| self.graph.node(inputs[op.refined_input()?]).repr);
        let frame_state = self.eager_frame_state();
        let node = self.emit(op, inputs, repr);
        self.graph.nodes[node.index()].frame_state = Some(frame_state);
        node
    }

    /// `value`, refined: known to be an object, or the code exits.
    pub(super) fn check_object(&mut self, value: NodeId) -> NodeId {
        self.emit_checked(Op::CheckObject, vec![value], None)
    }

    /// `object`, refined: known to have one of `shapes`, or the code exits.
    pub(super) fn check_shapes(&mut self, object: NodeId, shapes: &[ShapeCheck]) -> NodeId {
        for shape in shapes {
            self.embed(shape.shape);
        }
        let address = self.cell_address(object);
        self.emit_checked(
            Op::CheckShape {
                shapes: shapes.to_vec(),
            },
            vec![object, address],
            None,
        )
    }

    /// Makes sure that the `PrototypeChainValidity` cell `validity` is still
    /// valid: the code depends on it staying valid if it was when the
    /// snapshot was taken (`valid`), and checks it otherwise.
    pub(super) fn check_prototype_chain(&mut self, validity: CellId, valid: bool) {
        self.embed(validity);
        if valid {
            self.assume(Dependency::PrototypeChainValid(validity));
            return;
        }
        self.emit_checked(Op::CheckPrototypeChainValid { validity }, Vec::new(), None);
    }

    /// Makes the code depend on no `[[IsHTMLDDA]]` object existing, if none
    /// existed when the snapshot was taken, for the current instruction,
    /// which tests the truthiness, type or loose equality with null of a
    /// value. Code depending on it treats every object as truthy, of type
    /// "object" or "function", and not loosely equal to null.
    pub(super) fn assume_no_htmldda_objects(&mut self) {
        if self.runtime.no_htmldda_objects {
            self.assume(Dependency::NoHtmlDdaObjects);
        }
    }

    /// Makes the code depend on `dependency` (see `Op::AssumeValid`) from
    /// the current instruction on.
    pub(super) fn assume(&mut self, dependency: Dependency) {
        self.graph.depend_on(dependency);
        self.emit_checked(Op::AssumeValid, Vec::new(), None);
    }

    pub(super) fn load_named(&mut self, object: NodeId, offset: u32) -> NodeId {
        let address = self.cell_address(object);
        self.emit_checked(Op::LoadNamed { offset }, vec![object, address], Some(Repr::Tagged))
    }

    pub(super) fn store_named(&mut self, object: NodeId, offset: u32, value: NodeId) {
        let address = self.cell_address(object);
        self.emit_checked(Op::StoreNamed { offset }, vec![object, value, address], None);
    }

    /// Loads the property a `Get` entry describes, from the object itself or
    /// from the prototype holding it.
    pub(super) fn load_entry(&mut self, object: NodeId, entry: &CacheEntry) -> NodeId {
        let Action::Get { holder, offset } = entry.action else {
            unreachable!("only gets load");
        };
        let Holder::Prototype {
            prototype,
            validity,
            valid,
        } = holder
        else {
            return self.load_named(object, offset);
        };
        self.check_prototype_chain(validity, valid);
        self.embed(prototype);
        let holder = self.boxed_object(prototype);
        let holder = self.constant(holder);
        match entry.prototype_property {
            Some((expected, intrinsic)) if self.may_speculate(ExitKind::UnexpectedValue) => {
                if let Some(intrinsic) = intrinsic {
                    let bits = self.boxed_object(expected);
                    self.intrinsics.insert(bits, intrinsic);
                }
                self.load_constant_named(holder, offset, expected)
            }
            _ => self.load_named(holder, offset),
        }
    }

    /// Loads property `offset` of `holder`, which held the object `expected`
    /// at compile time, as a constant: the load is checked against it.
    pub(super) fn load_constant_named(&mut self, holder: NodeId, offset: u32, expected: CellId) -> NodeId {
        self.embed(expected);
        let expected = self.boxed_object(expected);
        let constant = self.constant(expected);
        let loaded = self.load_named(holder, offset);
        self.emit_checked(
            Op::CheckValue {
                expected,
                kind: ExitKind::UnexpectedValue,
            },
            vec![loaded],
            None,
        );
        constant
    }

    /// Builds `dst = base.property` from the instruction's property cache.
    /// Returns false, having built nothing that matters, if it cannot speculate.
    pub(super) fn try_build_get_by_id(
        &mut self,
        dst: Operand,
        base: Operand,
        cache: u32,
    ) -> Result<bool, CompileFailure> {
        let Some(entries) = self.cache_entries(cache) else {
            return Ok(false);
        };
        self.build_get(dst, base, entries, None)
    }

    /// The entries of the instruction's property cache if they all call the
    /// `part` of the same accessor property with functions of the same
    /// executable (see `try_inline_getter()`), with the index of that
    /// executable in the snapshot.
    fn accessor_call_entries(&self, cache: u32, part: AccessorPart) -> Option<(Vec<CacheEntry>, u32)> {
        let entries = self.cache_entries(cache)?;
        let first = entries[0];
        let executable_of = |entry: &CacheEntry| {
            let index = entry.accessor_function?.inline_executable?;
            Some(self.snapshot.executables.get(index as usize)?.cell)
        };
        let index = first.accessor_function?.inline_executable?;
        let executable = executable_of(&first)?;
        let same_executable = entries.iter().all(|entry| {
            entry.calls_accessor(part) && entry.action == first.action && executable_of(entry) == Some(executable)
        });
        let gates = [ExitKind::NotObject, ExitKind::BadShape, ExitKind::BadCallTarget];
        if !same_executable || !gates.into_iter().all(|kind| self.may_speculate(kind)) {
            return None;
        }
        Some((entries, index))
    }

    /// Checks that `base` is an object of the shapes of `entries`, which all
    /// call the `part` of the accessor at their property offset, and that
    /// the accessor has the function the entries saw: in the object, or in
    /// the entries' prototype with the prototype chain unchanged. Returns
    /// the function as a value if the inlined call must check it and run it
    /// with its own environment, or nothing if it is the one the entries
    /// saw.
    ///
    /// NB: Accessors of objects of one shape can have other functions, such
    ///     as closures of one executable made for each object. Their calls
    ///     are inlined for the executable once a check of the function
    ///     itself failed at the access, or if the entries saw several.
    fn check_accessor_call(
        &mut self,
        base: Operand,
        entries: &[CacheEntry],
        part: AccessorPart,
    ) -> Result<Option<NodeId>, CompileFailure> {
        let first = entries[0];
        let object = self.read(base)?;
        let object = self.check_object(object);
        let shapes = entries.iter().map(|entry| self.shape_guard(entry)).collect::<Vec<_>>();
        let object = self.check_shapes(object, &shapes);
        let (Action::Get { holder, offset } | Action::Set { holder, offset }) = first.action else {
            unreachable!("accessor calls get or set");
        };
        let holder = match holder {
            Holder::Prototype {
                prototype,
                validity,
                valid,
            } => {
                self.check_prototype_chain(validity, valid);
                self.embed(prototype);
                let prototype = self.boxed_object(prototype);
                self.constant(prototype)
            }
            Holder::Receiver => object,
        };
        let address = self.cell_address(holder);
        let function = first.accessor_function.expect("the entries call a function").function;
        let one_function = entries.iter().all(|entry| {
            entry
                .accessor_function
                .is_some_and(|accessor| accessor.function == function)
        });
        if one_function && self.may_speculate(ExitKind::UnexpectedValue) {
            self.embed(function);
            self.emit_checked(
                Op::CheckAccessorFunction { offset, part, function },
                vec![holder, address],
                None,
            );
            return Ok(None);
        }
        Ok(Some(self.emit_checked(
            Op::LoadAccessorFunction { offset, part },
            vec![holder, address],
            Some(Repr::Tagged),
        )))
    }

    /// Builds `dst = base.property` for a property that holds an accessor,
    /// as an inlined call of its getter: the instruction's cache entries
    /// must all be for an own property at the same offset, or for the same
    /// property of the same prototype, whose accessor must have the getter
    /// it had when the snapshot was taken. Returns `None`, having built
    /// nothing that matters, if it cannot.
    pub(super) fn try_inline_getter(
        &mut self,
        dst: Operand,
        base: Operand,
        cache: u32,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Some((entries, index)) = self.accessor_call_entries(cache, AccessorPart::Getter) else {
            return Ok(None);
        };
        let getter = self.check_accessor_call(base, &entries, AccessorPart::Getter)?;
        self.try_inline_getter_call(index, getter, base, dst)
    }

    /// Builds `base.property = src` for a property that a prototype holds as
    /// an accessor, as an inlined call of its setter, like
    /// `try_inline_getter()`. The setter's result goes into a register
    /// nothing reads before writing it again, if there is one.
    pub(super) fn try_inline_setter(
        &mut self,
        base: Operand,
        src: Operand,
        cache: u32,
    ) -> Result<Option<Flow>, CompileFailure> {
        let Some((entries, index)) = self.accessor_call_entries(cache, AccessorPart::Setter) else {
            return Ok(None);
        };
        let Some(dst) = self.dead_register(&[base, src]) else {
            return Ok(None);
        };
        let setter = self.check_accessor_call(base, &entries, AccessorPart::Setter)?;
        self.try_inline_setter_call(index, setter, base, src, dst)
    }

    /// A register other than `operands` that is dead after the current
    /// instruction: nothing reads it before writing it again.
    fn dead_register(&self, operands: &[Operand]) -> Option<Operand> {
        let layout = self.function.layout;
        let live = self.function.liveness.live_out(self.function.instruction_index);
        (crate::bytecode::RESERVED_REGISTER_COUNT..layout.number_of_registers)
            .map(Operand::from_raw)
            .find(|register| {
                !operands.contains(register) && layout.tracked_index(*register).is_some_and(|slot| !live.contains(slot))
            })
    }

    /// Builds `dst = base[property]` for a string or symbol key from the
    /// instruction's keyed property cache, like `try_build_get_by_id()`.
    pub(super) fn try_build_get_by_value(
        &mut self,
        dst: Operand,
        base: Operand,
        property: Operand,
        cache: u32,
    ) -> Result<bool, CompileFailure> {
        let Some((entries, key_check)) = self.keyed_cache_entries(cache, property)? else {
            return Ok(false);
        };
        self.build_get(dst, base, entries, key_check)
    }

    /// Builds `base[property] = src` for a string or symbol key from the
    /// instruction's keyed property cache, like `try_build_put_by_id()`.
    pub(super) fn try_build_put_by_value(
        &mut self,
        base: Operand,
        property: Operand,
        src: Operand,
        cache: u32,
    ) -> Result<bool, CompileFailure> {
        let Some((entries, key_check)) = self.keyed_cache_entries(cache, property)? else {
            return Ok(false);
        };
        self.build_put(base, src, entries, key_check)
    }

    /// The entries of a keyed access's cache for the key in `key`: the key's
    /// own if it is a constant, or those of the only key the cache saw, with
    /// the key check to make (see `check_key()`). Returns nothing for other
    /// keys.
    fn keyed_cache_entries(&mut self, cache: u32, key: Operand) -> Result<Option<KeyedEntries>, CompileFailure> {
        let Some(entries) = self.cache_entries(cache) else {
            return Ok(None);
        };
        let Some(keys) = entries.iter().map(|entry| entry.key).collect::<Option<Vec<_>>>() else {
            return Ok(None);
        };
        let key = self.read(key)?;
        let key_value = match self.graph.constant_value(key) {
            Some(bits) => bits,
            None => {
                let (first, _) = keys[0];
                if !keys.iter().all(|(value, _)| *value == first) || !self.may_speculate(ExitKind::UnexpectedValue) {
                    return Ok(None);
                }
                first
            }
        };
        let entries = entries
            .into_iter()
            .filter(|entry| entry.key.is_some_and(|(value, _)| value == key_value))
            .collect::<Vec<_>>();
        let Some((_, cell)) = entries.first().and_then(|entry| entry.key) else {
            return Ok(None);
        };
        let key_check = self.graph.constant_value(key).is_none().then_some(KeyCheck {
            key,
            expected: key_value,
            cell,
        });
        Ok(Some((entries, key_check)))
    }

    /// Checks that a key that is not a constant is the one speculated on.
    fn check_key(&mut self, check: Option<KeyCheck>) {
        let Some(check) = check else {
            return;
        };
        self.embed(check.cell);
        self.emit_checked(
            Op::CheckValue {
                expected: check.expected,
                kind: ExitKind::UnexpectedValue,
            },
            vec![check.key],
            None,
        );
    }

    /// Builds `dst = base.property` from cache entries for the property.
    fn build_get(
        &mut self,
        dst: Operand,
        base: Operand,
        entries: Vec<CacheEntry>,
        key_check: Option<KeyCheck>,
    ) -> Result<bool, CompileFailure> {
        // NB: Loads of properties that hold accessors exit, so these are left to the instruction's handling, which
        //     calls their getters, unless the getter is inlined (see `try_inline_getter()`).
        let speculatable = entries
            .iter()
            .all(|entry| matches!(entry.action, Action::Get { .. }) && entry.is_data_access());
        if !speculatable || !self.may_speculate(ExitKind::NotObject) || !self.may_speculate(ExitKind::BadShape) {
            return Ok(false);
        }

        self.check_key(key_check);
        let object = self.read(base)?;
        let object = self.check_object(object);
        let result = self
            .translate_entries(object, &entries, |builder, entry, object| {
                Some(builder.load_entry(object, entry))
            })
            .expect("every case loads a value");
        self.write(dst, result)?;
        Ok(true)
    }

    /// Builds `base.property = src` from the instruction's property cache,
    /// for existing writable data properties.
    pub(super) fn try_build_put_by_id(
        &mut self,
        base: Operand,
        src: Operand,
        cache: u32,
    ) -> Result<bool, CompileFailure> {
        let Some(entries) = self.cache_entries(cache) else {
            return Ok(false);
        };
        self.build_put(base, src, entries, None)
    }

    /// Builds `base.property = src` from cache entries for the property.
    fn build_put(
        &mut self,
        base: Operand,
        src: Operand,
        entries: Vec<CacheEntry>,
        key_check: Option<KeyCheck>,
    ) -> Result<bool, CompileFailure> {
        let speculatable = entries
            .iter()
            .all(|entry| matches!(entry.action, Action::Set { .. }) && entry.is_data_access());
        if !speculatable || !self.may_speculate(ExitKind::NotObject) || !self.may_speculate(ExitKind::BadShape) {
            return Ok(false);
        }

        self.check_key(key_check);
        let object = self.read(base)?;
        let value = self.read(src)?;
        let object = self.check_object(object);
        self.translate_entries(object, &entries, |builder, entry, object| {
            let Action::Set { offset, .. } = entry.action else {
                unreachable!("only sets store");
            };
            builder.store_named(object, offset, value);
            None
        });
        Ok(true)
    }
}
