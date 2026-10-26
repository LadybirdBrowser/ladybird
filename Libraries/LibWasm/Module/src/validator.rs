/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Module validation. Function bodies are checked in one pass over their bytes, which also builds the side table the
//! in-place interpreter branches with.
//!
//! # Side tables
//!
//! The interpreter executes a function's original bytes, so it cannot find branch targets in the code itself. Every
//! instruction that may transfer control to somewhere other than the next instruction owns entries of the function's
//! side table, in the order the instructions appear. The interpreter keeps a pointer to the next entry (`stp`) next
//! to the instruction pointer and advances it as it executes those instructions, so a branch finds its entry without
//! a search:
//!
//! - `br`, `br_if`, `br_on_null`, `br_on_non_null`, `br_on_cast`, and `br_on_cast_fail` own one entry, for the taken
//!   branch. Not taking a conditional branch steps over it.
//! - `br_table` owns one entry per label, the default last.
//! - `if` owns one entry, for jumping to its `else` arm, or past its `end` if it has none.
//! - `else` owns one entry, for jumping past the `end` of the `if` when its first arm completes.
//! - The legacy `catch` and `catch_all` own one entry each, for jumping past the `end` of their `try` when the body or
//!   handler before them completes, as `else` does.
//! - The legacy `rethrow` owns one entry, whose `keep` is the index of the local that holds the exception to rethrow.
//!
//! An entry holds the distance from the branching instruction to its target, the distance from the entry itself to
//! the entry the target continues with, and how to reshape the operand stack: keep the `keep` slots at its top and
//! drop the `drop` slots below them. Branches to blocks target the instruction after their `end`; branches to loops
//! target the `loop` instruction, which is where back edges are counted; branches to the function's label target its
//! final `end`, which returns.
//!
//! Exceptions are not linear, so `try_table` handlers live in a separate table of the regions they protect.
//!
//! # Legacy exception handling
//!
//! The body of a legacy `try` is a region whose clauses are its `catch` and `catch_all` handlers, in order. A handler
//! starts after its instruction, with the operand stack at the height of the `try`, and the runtime also stores the
//! exception it caught in a local of its own past the function's declared ones, for `rethrow`. A handler nested in
//! the handlers of `n` other tries gets the `n`th such local. The body of a `try` that ends with `delegate` is a region
//! without clauses that names where the search for a handler goes on: only regions that start at or before its
//! `delegate_bound`, which leaves out those inside the block the `delegate` names.

use crate::error::{Error, Result};
use crate::instruction_tables::{AtomicKind, SimdKind, atomic_kind, simd_kind};
use crate::module::{ConstantExpression, DataMode, ElementItems, ElementMode, ExternalKind, ImportDescription, Module};
use crate::opcode;
use crate::parser::{parse_heap_type, parse_value_type};
use crate::quickening::Rewrite;
use crate::reader::Reader;
use crate::types::{
    AddressType, CompositeType, FieldType, FuncType, HeapType, Limits, RefType, StorageType, SubType,
    TypeCanonicalizer, ValType,
};
use std::collections::HashSet;

/// One side-table entry. See the module documentation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(C)]
pub struct SideTableEntry {
    /// The target's offset from the branching instruction, in bytes.
    pub pc_delta: i32,
    /// The target's next entry's offset from this entry, in entries.
    pub stp_delta: i32,
    /// The number of slots at the top of the operand stack the branch keeps.
    pub keep: u32,
    /// The number of slots below them the branch drops.
    pub drop: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CatchKind {
    Catch,
    CatchRef,
    CatchAll,
    CatchAllRef,
}

/// One catch clause of a `try_table` or a legacy `try`: which exceptions it catches and where the handler continues.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CatchClause {
    pub kind: CatchKind,
    /// The tag index of `catch` and `catch_ref` clauses.
    pub tag: u32,
    /// The module offset of the instruction the handler continues at.
    pub target_pc: u32,
    /// The index of the side-table entry the handler continues with.
    pub target_stp: u32,
    /// The height of the operand stack, in slots, below the values the handler pushes.
    pub height: u32,
    /// The local a legacy handler keeps the exception in, for `rethrow`, or NO_EXCEPTION_LOCAL.
    pub exception_local: u32,
}

pub const NO_EXCEPTION_LOCAL: u32 = u32::MAX;

/// The instructions of one `try_table` or legacy `try` body, by module offset, and the range of its clauses in the
/// catch table.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TryRegion {
    pub start: u32,
    pub end: u32,
    pub first_clause: u32,
    pub clause_count: u32,
    /// For the body of a legacy `try` that ends with `delegate`, the latest start of the regions the search for a
    /// handler goes on with, or NOT_DELEGATED.
    pub delegate_bound: u32,
}

pub const NOT_DELEGATED: u32 = u32::MAX;

/// What validation learned about a defined function that running it needs.
#[derive(Debug, Clone, Default)]
pub struct FunctionInfo {
    pub side_table: Box<[SideTableEntry]>,
    /// `try_table` regions in the order their instructions appear, so that inner regions come after outer ones.
    pub try_regions: Box<[TryRegion]>,
    pub catch_clauses: Box<[CatchClause]>,
    /// The number of locals, parameters and the locals of legacy handlers included. The types of the others are in
    /// the function's body.
    pub local_count: u32,
    /// The number of locals legacy handlers keep their exceptions in, which come after the declared ones.
    pub exception_local_count: u32,
    /// The most slots the operand stack ever holds.
    pub max_stack_slots: u32,
}

impl FunctionInfo {
    /// The bytes this function's validation results take beyond the module's own bytes.
    pub fn metadata_size(&self) -> usize {
        size_of::<Self>()
            + self.side_table.len() * size_of::<SideTableEntry>()
            + self.try_regions.len() * size_of::<TryRegion>()
            + self.catch_clauses.len() * size_of::<CatchClause>()
    }
}

/// The largest number of pages a memory may declare.
const MEMORY32_MAX_PAGES: u64 = 1 << 16;
const MEMORY64_MAX_PAGES: u64 = 1 << 48;

// https://webassembly.github.io/spec/js-api/#limits

// The maximum depth of a defined subtype hierarchy is 63 (where a type defined with no supertype has depth 0).
const MAX_SUBTYPING_DEPTH: u32 = 63;

// The maximum number of locals declared in a function, including implicitly declared as parameters, is 50,000.
const MAX_FUNCTION_LOCALS: usize = 50_000;

fn invalid<T>(message: impl Into<String>) -> Result<T> {
    Err(Error::invalid(message))
}

fn type_mismatch<T>() -> Result<T> {
    invalid("type mismatch")
}

/// The type-level view of a module that validation checks against.
pub struct TypeContext<'a> {
    pub types: &'a [SubType],
    /// The identity of each type, or none where the types are identities themselves, as in a store.
    pub identities: Option<&'a [u32]>,
}

impl TypeContext<'_> {
    fn sub_type(&self, index: u32) -> &SubType {
        &self.types[index as usize]
    }

    fn is_func(&self, index: u32) -> bool {
        matches!(self.sub_type(index).composite, CompositeType::Func(_))
    }

    fn top(&self, heap: HeapType) -> HeapType {
        match heap {
            HeapType::Func | HeapType::NoFunc => HeapType::Func,
            HeapType::Extern | HeapType::NoExtern => HeapType::Extern,
            HeapType::Exn | HeapType::NoExn => HeapType::Exn,
            HeapType::Any | HeapType::Eq | HeapType::I31 | HeapType::Struct | HeapType::Array | HeapType::None => {
                HeapType::Any
            }
            HeapType::Concrete(index) => match self.sub_type(index).composite {
                CompositeType::Func(_) => HeapType::Func,
                _ => HeapType::Any,
            },
            HeapType::Bottom => HeapType::Bottom,
        }
    }

    pub fn heap_subtype(&self, sub: HeapType, sup: HeapType) -> bool {
        use HeapType::*;
        if sub == sup || sub == Bottom {
            return true;
        }
        match (sub, sup) {
            (Concrete(a), Concrete(b)) => {
                if self
                    .identities
                    .is_some_and(|identities| identities[a as usize] == identities[b as usize])
                {
                    return true;
                }
                self.sub_type(a)
                    .supertypes
                    .iter()
                    .any(|&supertype| self.heap_subtype(Concrete(supertype), sup))
            }
            (Concrete(a), _) => match self.sub_type(a).composite {
                CompositeType::Func(_) => sup == Func,
                CompositeType::Struct(_) => matches!(sup, Struct | Eq | Any),
                CompositeType::Array(_) => matches!(sup, Array | Eq | Any),
            },
            (None, Concrete(b)) => !self.is_func(b),
            (NoFunc, Concrete(b)) => self.is_func(b),
            (_, Concrete(_)) => false,
            (None, Any | Eq | I31 | Struct | Array) => true,
            (I31 | Struct | Array, Eq | Any) => true,
            (Eq, Any) => true,
            (NoFunc, Func) | (NoExtern, Extern) | (NoExn, Exn) => true,
            _ => false,
        }
    }

    pub fn ref_subtype(&self, sub: RefType, sup: RefType) -> bool {
        (!sub.nullable || sup.nullable) && self.heap_subtype(sub.heap, sup.heap)
    }

    pub fn value_subtype(&self, sub: ValType, sup: ValType) -> bool {
        match (sub, sup) {
            (ValType::Ref(sub), ValType::Ref(sup)) => self.ref_subtype(sub, sup),
            _ => sub == sup,
        }
    }

    fn storage_subtype(&self, sub: StorageType, sup: StorageType) -> bool {
        match (sub, sup) {
            (StorageType::Val(sub), StorageType::Val(sup)) => self.value_subtype(sub, sup),
            _ => sub == sup,
        }
    }

    fn field_subtype(&self, sub: FieldType, sup: FieldType) -> bool {
        if sub.mutable != sup.mutable {
            return false;
        }
        if sub.mutable {
            self.storage_subtype(sub.storage, sup.storage) && self.storage_subtype(sup.storage, sub.storage)
        } else {
            self.storage_subtype(sub.storage, sup.storage)
        }
    }

    fn values_subtype(&self, sub: &[ValType], sup: &[ValType]) -> bool {
        sub.len() == sup.len() && sub.iter().zip(sup).all(|(&sub, &sup)| self.value_subtype(sub, sup))
    }

    fn function_subtype(&self, sub: &FuncType, sup: &FuncType) -> bool {
        self.values_subtype(&sup.params, &sub.params) && self.values_subtype(&sub.results, &sup.results)
    }

    fn composite_subtype(&self, sub: &CompositeType, sup: &CompositeType) -> bool {
        match (sub, sup) {
            (CompositeType::Func(sub), CompositeType::Func(sup)) => self.function_subtype(sub, sup),
            (CompositeType::Struct(sub), CompositeType::Struct(sup)) => {
                sub.len() >= sup.len() && sub.iter().zip(sup).all(|(&sub, &sup)| self.field_subtype(sub, sup))
            }
            (CompositeType::Array(sub), CompositeType::Array(sup)) => self.field_subtype(*sub, *sup),
            _ => false,
        }
    }
}

/// An operand stack entry. `None` is a value of unknown type, which only unreachable code produces.
type Operand = Option<ValType>;

fn operand_slots(operand: Operand) -> u32 {
    operand.map_or(1, ValType::slot_count)
}

fn slots_of(types: &[ValType]) -> u32 {
    types.iter().map(|ty| ty.slot_count()).sum()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FrameKind {
    Block,
    Loop,
    If,
    Else,
    TryTable,
    /// The body of a legacy `try`.
    Try,
    /// A legacy `catch` handler.
    Catch,
    /// A legacy `catch_all` handler.
    CatchAll,
    Function,
}

/// A side-table entry or catch clause whose target is the end of a frame that has not been reached yet.
#[derive(Debug, Clone, Copy)]
enum Fixup {
    Entry { entry: usize, branch_pc: u32 },
    Clause(usize),
}

struct Frame {
    kind: FrameKind,
    params: Vec<ValType>,
    results: Vec<ValType>,
    height: usize,
    slot_height: u32,
    unreachable: bool,
    initialization_height: usize,
    /// The module offset of the frame's first instruction: the `loop` instruction of a loop, which back edges target.
    start_pc: u32,
    /// The side-table index a branch to a loop continues with.
    start_stp: u32,
    fixups: Vec<Fixup>,
    /// The entry of an `if` that jumps to its `else` arm.
    if_entry: Option<(usize, u32)>,
    /// The index of the `try_table` region this frame protects.
    try_region: Option<usize>,
    /// The index of the region of a legacy `try`'s body, for the try and its handlers.
    legacy_region: Option<usize>,
    /// The clauses of a legacy `try`'s handlers so far, which go into the catch table together at its `end`.
    legacy_clauses: Vec<CatchClause>,
    /// The local in which a legacy handler keeps its exception.
    exception_local: Option<u32>,
    /// How many legacy handlers this frame is in, itself included: the position among the exception locals, which
    /// come after the declared locals, of the one a handler directly inside it takes. Kept per frame so that a handler
    /// need not count them in every frame around it, which made deeply nested handlers take quadratic time.
    catch_depth: u32,
}

impl Frame {
    fn label_types(&self) -> &[ValType] {
        if self.kind == FrameKind::Loop {
            &self.params
        } else {
            &self.results
        }
    }
}

struct FunctionValidator<'a> {
    module: &'a Module,
    types: TypeContext<'a>,
    declared_functions: &'a HashSet<u32>,
    locals: Vec<ValType>,
    local_initialized: Vec<bool>,
    initialization_log: Vec<u32>,
    operands: Vec<Operand>,
    slot_height: u32,
    max_slot_height: u32,
    frames: Vec<Frame>,
    side_table: Vec<SideTableEntry>,
    try_regions: Vec<TryRegion>,
    catch_clauses: Vec<CatchClause>,
    exception_local_count: u32,
    results: Vec<ValType>,
    /// Where the instruction being validated starts.
    instruction_start: u32,
    /// The quickened forms of this function's instructions (see quickening.rs).
    rewrites: Vec<Rewrite>,
    /// The memory index and offset immediates of the last memory argument read, if its access needs bounds checks.
    checked_access: Option<(Option<std::ops::Range<usize>>, std::ops::Range<usize>)>,
}

impl FunctionValidator<'_> {
    /// Quicken the instruction being validated into its v128 form if its value is a v128.
    fn quicken_for_v128(&mut self, value: Operand) {
        if value != Some(ValType::V128) {
            return;
        }
        let opcode = self.module.bytes[self.instruction_start as usize];
        let quickened = crate::quickening::v128_form(opcode).expect("only instructions with a v128 form get here");
        self.rewrites.push(Rewrite {
            position: self.instruction_start,
            bytes: vec![quickened],
        });
    }

    fn push(&mut self, operand: Operand) {
        self.slot_height += operand_slots(operand);
        self.max_slot_height = self.max_slot_height.max(self.slot_height);
        self.operands.push(operand);
    }

    fn push_value(&mut self, value: ValType) {
        self.push(Some(value));
    }

    fn push_values(&mut self, values: &[ValType]) {
        for &value in values {
            self.push_value(value);
        }
    }

    fn pop(&mut self) -> Result<Operand> {
        let frame = self.frames.last().expect("validation always has a frame");
        if self.operands.len() == frame.height {
            if frame.unreachable {
                return Ok(None);
            }
            return type_mismatch();
        }
        let operand = self.operands.pop().expect("the stack is above the frame's height");
        self.slot_height -= operand_slots(operand);
        Ok(operand)
    }

    fn pop_expecting(&mut self, expected: ValType) -> Result<Operand> {
        let actual = self.pop()?;
        if let Some(actual) = actual
            && !self.types.value_subtype(actual, expected)
        {
            return type_mismatch();
        }
        Ok(actual.or(Some(expected)))
    }

    fn pop_values(&mut self, values: &[ValType]) -> Result<()> {
        for &value in values.iter().rev() {
            self.pop_expecting(value)?;
        }
        Ok(())
    }

    /// Pop a reference of any type, and return it unless it is of unknown type.
    fn pop_reference(&mut self) -> Result<Option<RefType>> {
        match self.pop()? {
            None => Ok(None),
            Some(ValType::Ref(reference)) => Ok(Some(reference)),
            Some(_) => type_mismatch(),
        }
    }

    fn set_unreachable(&mut self) {
        let frame = self.frames.last_mut().expect("validation always has a frame");
        let height = frame.height;
        frame.unreachable = true;
        while self.operands.len() > height {
            let operand = self.operands.pop().expect("the stack is above the frame's height");
            self.slot_height -= operand_slots(operand);
        }
    }

    fn push_frame(&mut self, kind: FrameKind, params: Vec<ValType>, results: Vec<ValType>, start_pc: u32) {
        let height = self.operands.len();
        let slot_height = self.slot_height;
        let catch_depth = self.frames.last().map_or(0, |frame| frame.catch_depth)
            + u32::from(matches!(kind, FrameKind::Catch | FrameKind::CatchAll));
        self.frames.push(Frame {
            kind,
            params: params.clone(),
            results,
            height,
            slot_height,
            unreachable: false,
            initialization_height: self.initialization_log.len(),
            start_pc,
            start_stp: self.side_table.len() as u32,
            fixups: Vec::new(),
            if_entry: None,
            try_region: None,
            legacy_region: None,
            legacy_clauses: Vec::new(),
            exception_local: None,
            catch_depth,
        });
        self.push_values(&params);
    }

    fn pop_frame(&mut self) -> Result<Frame> {
        let frame = self.frames.last().expect("validation always has a frame");
        let results = frame.results.clone();
        self.pop_values(&results)?;
        let frame = self.frames.last().expect("validation always has a frame");
        if self.operands.len() != frame.height {
            return type_mismatch();
        }
        let frame = self.frames.pop().expect("validation always has a frame");
        for local in self.initialization_log.drain(frame.initialization_height..) {
            self.local_initialized[local as usize] = false;
        }
        Ok(frame)
    }

    fn label(&self, depth: u32) -> Result<&Frame> {
        let count = self.frames.len();
        if depth as usize >= count {
            return invalid("unknown label");
        }
        Ok(&self.frames[count - 1 - depth as usize])
    }

    fn label_types(&self, depth: u32) -> Result<Vec<ValType>> {
        Ok(self.label(depth)?.label_types().to_vec())
    }

    /// Add the side-table entry of a branch at `branch_pc` to the label at `depth`, taken with the current operand
    /// stack.
    fn add_branch_entry(&mut self, depth: u32, branch_pc: u32) {
        let entry_index = self.side_table.len();
        let frame_index = self.frames.len() - 1 - depth as usize;
        let frame = &self.frames[frame_index];
        let keep = slots_of(frame.label_types());
        let drop = self.slot_height.saturating_sub(frame.slot_height + keep);
        let reachable = !self.frames.last().expect("validation always has a frame").unreachable;
        let mut entry = SideTableEntry {
            pc_delta: 0,
            stp_delta: 0,
            keep,
            drop: if reachable { drop } else { 0 },
        };
        if frame.kind == FrameKind::Loop {
            entry.pc_delta = frame.start_pc as i32 - branch_pc as i32;
            entry.stp_delta = frame.start_stp as i32 - entry_index as i32;
        } else {
            self.frames[frame_index].fixups.push(Fixup::Entry {
                entry: entry_index,
                branch_pc,
            });
        }
        self.side_table.push(entry);
    }

    /// Point the pending branches of a frame at `target_pc`, continuing with the next side-table entry.
    fn resolve_fixups(&mut self, fixups: &[Fixup], target_pc: u32) {
        let target_stp = self.side_table.len();
        for fixup in fixups {
            match *fixup {
                Fixup::Entry { entry, branch_pc } => {
                    self.side_table[entry].pc_delta = target_pc as i32 - branch_pc as i32;
                    self.side_table[entry].stp_delta = target_stp as i32 - entry as i32;
                }
                Fixup::Clause(clause) => {
                    self.catch_clauses[clause].target_pc = target_pc;
                    self.catch_clauses[clause].target_stp = target_stp as u32;
                }
            }
        }
    }

    fn local(&self, index: u32) -> Result<ValType> {
        self.locals
            .get(index as usize)
            .copied()
            .ok_or_else(|| Error::invalid("unknown local"))
    }

    fn initialize_local(&mut self, index: u32) {
        if !self.local_initialized[index as usize] {
            self.local_initialized[index as usize] = true;
            self.initialization_log.push(index);
        }
    }

    fn global(&self, index: u32) -> Result<crate::types::GlobalType> {
        self.module
            .global_space
            .get(index as usize)
            .copied()
            .ok_or_else(|| Error::invalid("unknown global"))
    }

    fn table(&self, index: u32) -> Result<crate::types::TableType> {
        self.module
            .table_space
            .get(index as usize)
            .copied()
            .ok_or_else(|| Error::invalid("unknown table"))
    }

    fn memory_address_type(&self, index: u32) -> Result<AddressType> {
        self.module
            .memory_space
            .get(index as usize)
            .map(|memory| memory.limits.address_type)
            .ok_or_else(|| Error::invalid("unknown memory"))
    }

    fn function_type(&self, index: u32) -> Result<FuncType> {
        let type_index = *self
            .module
            .function_space
            .get(index as usize)
            .ok_or_else(|| Error::invalid("unknown function"))?;
        Ok(self.module.types[type_index as usize]
            .as_func()
            .expect("function types are checked first")
            .clone())
    }

    fn func_type_at(&self, type_index: u32) -> Result<FuncType> {
        match self.module.types.get(type_index as usize) {
            None => invalid("unknown type"),
            Some(sub_type) => sub_type
                .as_func()
                .cloned()
                .ok_or_else(|| Error::invalid("type mismatch")),
        }
    }

    fn struct_type(&self, type_index: u32) -> Result<Vec<FieldType>> {
        match self
            .module
            .types
            .get(type_index as usize)
            .map(|sub_type| &sub_type.composite)
        {
            None => invalid("unknown type"),
            Some(CompositeType::Struct(fields)) => Ok(fields.clone()),
            Some(_) => type_mismatch(),
        }
    }

    fn array_type(&self, type_index: u32) -> Result<FieldType> {
        match self
            .module
            .types
            .get(type_index as usize)
            .map(|sub_type| &sub_type.composite)
        {
            None => invalid("unknown type"),
            Some(CompositeType::Array(field)) => Ok(*field),
            Some(_) => type_mismatch(),
        }
    }

    fn tag_params(&self, index: u32) -> Result<Vec<ValType>> {
        let tag = self
            .module
            .tag_space
            .get(index as usize)
            .ok_or_else(|| Error::invalid("unknown tag"))?;
        Ok(self.func_type_at(tag.type_index)?.params)
    }

    fn check_data_index(&self, index: u32) -> Result<()> {
        let Some(count) = self.module.data_count else {
            return invalid("data count section required");
        };
        if index >= count {
            return invalid("unknown data segment");
        }
        Ok(())
    }

    fn element_type(&self, index: u32) -> Result<RefType> {
        self.module
            .elements
            .get(index as usize)
            .map(|element| element.ty)
            .ok_or_else(|| Error::invalid("unknown elem segment"))
    }

    fn check_heap_type(&self, heap: HeapType) -> Result<()> {
        check_heap_type(self.module, heap)
    }

    fn check_value_type(&self, value: ValType) -> Result<()> {
        check_value_type(self.module, value)
    }

    fn read_block_type(&self, reader: &mut Reader<'_>) -> Result<(Vec<ValType>, Vec<ValType>)> {
        match reader.peek_u8()? {
            0x40 => {
                reader.read_u8()?;
                Ok((Vec::new(), Vec::new()))
            }
            0x7f | 0x7e | 0x7d | 0x7c | 0x7b | 0x63 | 0x64 | 0x70 | 0x6f | 0x6e | 0x6d | 0x6c | 0x6b | 0x6a | 0x69
            | 0x71 | 0x72 | 0x73 | 0x74 => {
                let value = parse_value_type(reader)?;
                self.check_value_type(value)?;
                Ok((Vec::new(), vec![value]))
            }
            _ => {
                let index = reader.read_s33()?;
                if index < 0 {
                    return Err(Error::malformed("malformed block type"));
                }
                let function = self.func_type_at(u32::try_from(index).map_err(|_| Error::invalid("unknown type"))?)?;
                Ok((function.params, function.results))
            }
        }
    }

    fn read_memory_argument(&mut self, reader: &mut Reader<'_>, natural_size: u32) -> Result<AddressType> {
        let alignment_start = reader.position();
        let mut align = reader.read_u32()?;
        let alignment_end = reader.position();
        let mut memory = 0;
        let mut memory_bytes = None;
        if align & 0x40 != 0 {
            align &= !0x40;
            let start = reader.position();
            memory = reader.read_u32()?;
            memory_bytes = Some(start..reader.position());
        }
        let address_type = self.memory_address_type(memory)?;
        let offset_start = reader.position();
        let offset = reader.read_u64()?;
        let opcode = self.module.bytes[self.instruction_start as usize];
        let is_core_access = (crate::opcode::I32_LOAD..=crate::opcode::I64_STORE32).contains(&opcode);
        self.checked_access = None;
        // The plain handlers only decode `align offset` with a one-byte alignment, for memory 0 as a memory32 (see
        // rule 1 in quickening.rs). Every other encoding, even of the same access, becomes a checked access.
        let is_plain =
            memory_bytes.is_none() && alignment_end - alignment_start == 1 && address_type == AddressType::I32;
        if !is_plain {
            if is_core_access {
                self.rewrites.push(crate::quickening::checked_access(
                    &self.module.bytes,
                    self.instruction_start as usize,
                    alignment_start..alignment_end,
                    memory_bytes,
                    offset_start..reader.position(),
                ));
            } else {
                self.checked_access = Some((memory_bytes, offset_start..reader.position()));
            }
        }
        if align >= 64 {
            return Err(Error::malformed("malformed memop flags"));
        }
        if address_type == AddressType::I32 && offset > u64::from(u32::MAX) {
            return invalid("offset out of range");
        }
        if 1u64 << align > u64::from(natural_size) {
            return invalid("alignment must not be larger than natural");
        }
        Ok(address_type)
    }

    fn read_atomic_memory_argument(&self, reader: &mut Reader<'_>, natural_size: u32) -> Result<AddressType> {
        let mut align = reader.read_u32()?;
        let mut memory = 0;
        if align & 0x40 != 0 {
            align &= !0x40;
            memory = reader.read_u32()?;
        }
        let address_type = self.memory_address_type(memory)?;
        let offset = reader.read_u64()?;
        if address_type == AddressType::I32 && offset > u64::from(u32::MAX) {
            return invalid("offset out of range");
        }
        if align >= 64 || 1u64 << align != u64::from(natural_size) {
            return invalid("atomic memory op alignment must be equal to natural");
        }
        Ok(address_type)
    }

    fn load(&mut self, reader: &mut Reader<'_>, natural_size: u32, result: ValType) -> Result<()> {
        let address_type = self.read_memory_argument(reader, natural_size)?;
        self.pop_expecting(address_type.value_type())?;
        self.push_value(result);
        Ok(())
    }

    fn store(&mut self, reader: &mut Reader<'_>, natural_size: u32, value: ValType) -> Result<()> {
        let address_type = self.read_memory_argument(reader, natural_size)?;
        self.pop_expecting(value)?;
        self.pop_expecting(address_type.value_type())?;
        Ok(())
    }

    fn unary(&mut self, operand: ValType, result: ValType) -> Result<()> {
        self.pop_expecting(operand)?;
        self.push_value(result);
        Ok(())
    }

    fn binary(&mut self, operand: ValType, result: ValType) -> Result<()> {
        self.pop_expecting(operand)?;
        self.pop_expecting(operand)?;
        self.push_value(result);
        Ok(())
    }

    fn check_call_results(&self, results: &[ValType]) -> Result<()> {
        if !self.types.values_subtype(results, &self.results) {
            return type_mismatch();
        }
        Ok(())
    }

    fn validate_body(&mut self, reader: &mut Reader<'_>) -> Result<()> {
        while !self.frames.is_empty() {
            if reader.is_at_end() {
                return Err(Error::malformed("unexpected end of section or function"));
            }
            self.validate_instruction(reader)?;
        }
        if !reader.is_at_end() {
            return Err(Error::malformed("section size mismatch"));
        }
        Ok(())
    }

    fn validate_instruction(&mut self, reader: &mut Reader<'_>) -> Result<()> {
        use ValType::{F32, F64, I32, I64, V128};

        let pc = reader.position() as u32;
        self.instruction_start = pc;
        let opcode = reader.read_u8()?;
        match opcode {
            opcode::UNREACHABLE => self.set_unreachable(),
            opcode::NOP => {}
            opcode::BLOCK | opcode::LOOP => {
                let (params, results) = self.read_block_type(reader)?;
                self.pop_values(&params)?;
                let kind = if opcode == opcode::BLOCK {
                    FrameKind::Block
                } else {
                    FrameKind::Loop
                };
                self.push_frame(kind, params, results, pc);
            }
            opcode::IF => {
                let (params, results) = self.read_block_type(reader)?;
                self.pop_expecting(I32)?;
                self.pop_values(&params)?;
                let entry = self.side_table.len();
                self.side_table.push(SideTableEntry {
                    keep: slots_of(&params),
                    ..SideTableEntry::default()
                });
                self.push_frame(FrameKind::If, params, results, pc);
                self.frames.last_mut().expect("the frame was just pushed").if_entry = Some((entry, pc));
            }
            opcode::ELSE => {
                if self.frames.last().is_none_or(|frame| frame.kind != FrameKind::If) {
                    return invalid("else without if");
                }
                let frame = self.pop_frame()?;
                // The first arm jumps past the `end`.
                let else_entry = self.side_table.len();
                self.side_table.push(SideTableEntry {
                    keep: slots_of(&frame.results),
                    ..SideTableEntry::default()
                });
                // The condition jumps to the second arm, which starts after this instruction and its entry.
                let (if_entry, if_pc) = frame.if_entry.expect("an if frame has an if entry");
                self.side_table[if_entry].pc_delta = (pc + 1) as i32 - if_pc as i32;
                self.side_table[if_entry].stp_delta = self.side_table.len() as i32 - if_entry as i32;
                let mut fixups = frame.fixups;
                fixups.push(Fixup::Entry {
                    entry: else_entry,
                    branch_pc: pc,
                });
                self.push_frame(FrameKind::Else, frame.params, frame.results, frame.start_pc);
                self.frames.last_mut().expect("the frame was just pushed").fixups = fixups;
            }
            opcode::END => {
                let frame = self.pop_frame()?;
                if frame.kind == FrameKind::If && !self.types.values_subtype(&frame.params, &frame.results) {
                    return type_mismatch();
                }
                let target_pc = if frame.kind == FrameKind::Function { pc } else { pc + 1 };
                if let Some((if_entry, if_pc)) = frame.if_entry {
                    self.side_table[if_entry].pc_delta = target_pc as i32 - if_pc as i32;
                    self.side_table[if_entry].stp_delta = self.side_table.len() as i32 - if_entry as i32;
                }
                if let Some(region) = frame.try_region {
                    self.try_regions[region].end = pc;
                }
                if let Some(region) = frame.legacy_region {
                    if frame.kind == FrameKind::Try {
                        self.try_regions[region].end = pc;
                    }
                    self.try_regions[region].first_clause = self.catch_clauses.len() as u32;
                    self.try_regions[region].clause_count = frame.legacy_clauses.len() as u32;
                    self.catch_clauses.extend(frame.legacy_clauses);
                }
                self.resolve_fixups(&frame.fixups, target_pc);
                if frame.kind != FrameKind::Function {
                    self.push_values(&frame.results);
                }
            }
            opcode::BR => {
                let depth = reader.read_u32()?;
                let label = self.label_types(depth)?;
                self.pop_values(&label)?;
                self.push_values(&label);
                self.add_branch_entry(depth, pc);
                self.set_unreachable();
            }
            opcode::BR_IF => {
                let depth = reader.read_u32()?;
                self.pop_expecting(I32)?;
                let label = self.label_types(depth)?;
                self.pop_values(&label)?;
                self.push_values(&label);
                self.add_branch_entry(depth, pc);
            }
            opcode::BR_TABLE => {
                let count = reader.read_vector_length()?;
                let mut depths = Vec::with_capacity(count as usize + 1);
                for _ in 0..=count {
                    depths.push(reader.read_u32()?);
                }
                self.pop_expecting(I32)?;
                let default_depth = *depths.last().expect("there is a default label");
                let arity = self.label_types(default_depth)?.len();
                for &depth in &depths {
                    let label = self.label_types(depth)?;
                    if label.len() != arity {
                        return type_mismatch();
                    }
                    // Check the operands against each label without committing to one label's types.
                    let mut popped = Vec::with_capacity(arity);
                    for &value in label.iter().rev() {
                        let operand = self.pop()?;
                        if let Some(operand) = operand
                            && !self.types.value_subtype(operand, value)
                        {
                            return type_mismatch();
                        }
                        popped.push(operand);
                    }
                    for operand in popped.into_iter().rev() {
                        self.push(operand);
                    }
                }
                for &depth in &depths {
                    self.add_branch_entry(depth, pc);
                }
                self.set_unreachable();
            }
            opcode::RETURN => {
                let results = self.results.clone();
                self.pop_values(&results)?;
                self.set_unreachable();
            }
            opcode::CALL => {
                let function = self.function_type(reader.read_u32()?)?;
                self.pop_values(&function.params)?;
                self.push_values(&function.results);
            }
            opcode::RETURN_CALL => {
                let function = self.function_type(reader.read_u32()?)?;
                self.check_call_results(&function.results)?;
                self.pop_values(&function.params)?;
                self.set_unreachable();
            }
            opcode::CALL_INDIRECT | opcode::RETURN_CALL_INDIRECT => {
                let type_index = reader.read_u32()?;
                let table_index = reader.read_u32()?;
                let table = self.table(table_index)?;
                let function = self.func_type_at(type_index)?;
                if !self.types.ref_subtype(table.element, RefType::FUNCREF) {
                    return type_mismatch();
                }
                self.pop_expecting(table.limits.address_type.value_type())?;
                if opcode == opcode::RETURN_CALL_INDIRECT {
                    self.check_call_results(&function.results)?;
                    self.pop_values(&function.params)?;
                    self.set_unreachable();
                } else {
                    self.pop_values(&function.params)?;
                    self.push_values(&function.results);
                }
            }
            opcode::CALL_REF | opcode::RETURN_CALL_REF => {
                let type_index = reader.read_u32()?;
                let function = self.func_type_at(type_index)?;
                self.pop_expecting(ValType::Ref(RefType::new(true, HeapType::Concrete(type_index))))?;
                if opcode == opcode::RETURN_CALL_REF {
                    self.check_call_results(&function.results)?;
                    self.pop_values(&function.params)?;
                    self.set_unreachable();
                } else {
                    self.pop_values(&function.params)?;
                    self.push_values(&function.results);
                }
            }
            opcode::THROW => {
                let params = self.tag_params(reader.read_u32()?)?;
                self.pop_values(&params)?;
                self.set_unreachable();
            }
            opcode::THROW_REF => {
                self.pop_expecting(ValType::Ref(RefType::EXNREF))?;
                self.set_unreachable();
            }
            opcode::TRY => {
                let (params, results) = self.read_block_type(reader)?;
                self.pop_values(&params)?;
                let region = self.try_regions.len();
                self.try_regions.push(TryRegion {
                    start: reader.position() as u32,
                    end: 0,
                    first_clause: 0,
                    clause_count: 0,
                    delegate_bound: NOT_DELEGATED,
                });
                self.push_frame(FrameKind::Try, params, results, pc);
                self.frames.last_mut().expect("the frame was just pushed").legacy_region = Some(region);
            }
            opcode::CATCH | opcode::CATCH_ALL => {
                match self.frames.last().map(|frame| frame.kind) {
                    Some(FrameKind::Try | FrameKind::Catch) => {}
                    Some(FrameKind::CatchAll) => return invalid("catch after catch_all"),
                    _ => return invalid("catch without try"),
                }
                let (kind, tag, pushed) = if opcode == opcode::CATCH {
                    let tag = reader.read_u32()?;
                    (CatchKind::Catch, tag, self.tag_params(tag)?)
                } else {
                    (CatchKind::CatchAll, 0, Vec::new())
                };
                let frame = self.pop_frame()?;
                let region = frame.legacy_region.expect("a legacy try frame has a region");
                if frame.kind == FrameKind::Try {
                    self.try_regions[region].end = pc;
                }
                // The body or the handler before this one jumps past the `end`.
                let entry = self.side_table.len();
                self.side_table.push(SideTableEntry {
                    keep: slots_of(&frame.results),
                    ..SideTableEntry::default()
                });
                let mut fixups = frame.fixups;
                fixups.push(Fixup::Entry { entry, branch_pc: pc });
                let nesting = self.frames.last().map_or(0, |frame| frame.catch_depth);
                self.exception_local_count = self.exception_local_count.max(nesting + 1);
                let exception_local = self.locals.len() as u32 + nesting;
                let mut clauses = frame.legacy_clauses;
                clauses.push(CatchClause {
                    kind,
                    tag,
                    target_pc: reader.position() as u32,
                    target_stp: self.side_table.len() as u32,
                    height: frame.slot_height,
                    exception_local,
                });
                let handler_kind = if opcode == opcode::CATCH {
                    FrameKind::Catch
                } else {
                    FrameKind::CatchAll
                };
                self.push_frame(handler_kind, pushed, frame.results, frame.start_pc);
                let handler = self.frames.last_mut().expect("the frame was just pushed");
                handler.fixups = fixups;
                handler.legacy_region = Some(region);
                handler.legacy_clauses = clauses;
                handler.exception_local = Some(exception_local);
            }
            opcode::DELEGATE => {
                if self.frames.last().is_none_or(|frame| frame.kind != FrameKind::Try) {
                    return invalid("delegate without try");
                }
                let depth = reader.read_u32()?;
                let frame = self.pop_frame()?;
                let target = self.label(depth)?;
                // Regions inside the block the label names start after its body does.
                let bound = match target.kind {
                    FrameKind::Try => {
                        self.try_regions[target.legacy_region.expect("a legacy try frame has a region")].start
                    }
                    FrameKind::Function => 0,
                    _ => target.start_pc,
                };
                let region = frame.legacy_region.expect("a legacy try frame has a region");
                self.try_regions[region].end = pc;
                self.try_regions[region].delegate_bound = bound;
                self.try_regions[region].first_clause = self.catch_clauses.len() as u32;
                self.resolve_fixups(&frame.fixups, reader.position() as u32);
                self.push_values(&frame.results);
            }
            opcode::RETHROW => {
                let depth = reader.read_u32()?;
                let Some(local) = self.label(depth)?.exception_local else {
                    return invalid("invalid rethrow label");
                };
                self.side_table.push(SideTableEntry {
                    keep: local,
                    ..SideTableEntry::default()
                });
                self.set_unreachable();
            }
            opcode::TRY_TABLE => {
                let (params, results) = self.read_block_type(reader)?;
                let count = reader.read_vector_length()?;
                let first_clause = self.catch_clauses.len();
                let mut clause_labels = Vec::with_capacity(count as usize);
                for _ in 0..count {
                    let kind = match reader.read_u8()? {
                        0x00 => CatchKind::Catch,
                        0x01 => CatchKind::CatchRef,
                        0x02 => CatchKind::CatchAll,
                        0x03 => CatchKind::CatchAllRef,
                        _ => return Err(Error::malformed("malformed catch clause")),
                    };
                    let tag = if matches!(kind, CatchKind::Catch | CatchKind::CatchRef) {
                        reader.read_u32()?
                    } else {
                        0
                    };
                    let depth = reader.read_u32()?;
                    let mut pushed = if matches!(kind, CatchKind::Catch | CatchKind::CatchRef) {
                        self.tag_params(tag)?
                    } else {
                        Vec::new()
                    };
                    if matches!(kind, CatchKind::CatchRef | CatchKind::CatchAllRef) {
                        pushed.push(ValType::Ref(RefType::new(false, HeapType::Exn)));
                    }
                    let label = self.label_types(depth)?;
                    if !self.types.values_subtype(&pushed, &label) {
                        return type_mismatch();
                    }
                    clause_labels.push((kind, tag, depth));
                }
                self.pop_values(&params)?;
                for (kind, tag, depth) in clause_labels {
                    let clause_index = self.catch_clauses.len();
                    let frame_index = self.frames.len() - 1 - depth as usize;
                    let frame = &self.frames[frame_index];
                    let mut clause = CatchClause {
                        kind,
                        tag,
                        target_pc: 0,
                        target_stp: 0,
                        height: frame.slot_height,
                        exception_local: NO_EXCEPTION_LOCAL,
                    };
                    if frame.kind == FrameKind::Loop {
                        clause.target_pc = frame.start_pc;
                        clause.target_stp = frame.start_stp;
                    } else {
                        self.frames[frame_index].fixups.push(Fixup::Clause(clause_index));
                    }
                    self.catch_clauses.push(clause);
                }
                let region = self.try_regions.len();
                self.try_regions.push(TryRegion {
                    start: reader.position() as u32,
                    end: 0,
                    first_clause: first_clause as u32,
                    clause_count: count,
                    delegate_bound: NOT_DELEGATED,
                });
                self.push_frame(FrameKind::TryTable, params, results, pc);
                self.frames.last_mut().expect("the frame was just pushed").try_region = Some(region);
            }
            opcode::DROP => {
                let value = self.pop()?;
                self.quicken_for_v128(value);
            }
            opcode::SELECT => {
                self.pop_expecting(I32)?;
                let first = self.pop()?;
                let second = self.pop()?;
                let is_numeric_or_vector =
                    |operand: Operand| operand.is_none_or(|value| value.is_numeric() || value == V128);
                if !is_numeric_or_vector(first) || !is_numeric_or_vector(second) {
                    return type_mismatch();
                }
                match (first, second) {
                    (Some(first), Some(second)) if first != second => return type_mismatch(),
                    _ => {}
                }
                self.quicken_for_v128(first.or(second));
                self.push(first.or(second));
            }
            opcode::SELECT_TYPED => {
                let count = reader.read_u32()?;
                if count != 1 {
                    return invalid("invalid result arity");
                }
                let value = parse_value_type(reader)?;
                self.check_value_type(value)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(value)?;
                self.pop_expecting(value)?;
                self.push_value(value);
            }
            opcode::LOCAL_GET => {
                let index = reader.read_u32()?;
                let value = self.local(index)?;
                if !self.local_initialized[index as usize] {
                    return invalid("uninitialized local");
                }
                self.quicken_for_v128(Some(value));
                self.push_value(value);
            }
            opcode::LOCAL_SET => {
                let index = reader.read_u32()?;
                let value = self.local(index)?;
                self.pop_expecting(value)?;
                self.initialize_local(index);
                self.quicken_for_v128(Some(value));
            }
            opcode::LOCAL_TEE => {
                let index = reader.read_u32()?;
                let value = self.local(index)?;
                self.pop_expecting(value)?;
                self.initialize_local(index);
                self.quicken_for_v128(Some(value));
                self.push_value(value);
            }
            opcode::GLOBAL_GET => {
                let global = self.global(reader.read_u32()?)?;
                self.quicken_for_v128(Some(global.value));
                self.push_value(global.value);
            }
            opcode::GLOBAL_SET => {
                let global = self.global(reader.read_u32()?)?;
                if !global.mutable {
                    return invalid("global is immutable");
                }
                self.pop_expecting(global.value)?;
                self.quicken_for_v128(Some(global.value));
            }
            opcode::TABLE_GET => {
                let table = self.table(reader.read_u32()?)?;
                self.pop_expecting(table.limits.address_type.value_type())?;
                self.push_value(ValType::Ref(table.element));
            }
            opcode::TABLE_SET => {
                let table = self.table(reader.read_u32()?)?;
                self.pop_expecting(ValType::Ref(table.element))?;
                self.pop_expecting(table.limits.address_type.value_type())?;
            }
            opcode::I32_LOAD => self.load(reader, 4, I32)?,
            opcode::I64_LOAD => self.load(reader, 8, I64)?,
            opcode::F32_LOAD => self.load(reader, 4, F32)?,
            opcode::F64_LOAD => self.load(reader, 8, F64)?,
            opcode::I32_LOAD8_S | opcode::I32_LOAD8_U => self.load(reader, 1, I32)?,
            opcode::I32_LOAD16_S | opcode::I32_LOAD16_U => self.load(reader, 2, I32)?,
            opcode::I64_LOAD8_S | opcode::I64_LOAD8_U => self.load(reader, 1, I64)?,
            opcode::I64_LOAD16_S | opcode::I64_LOAD16_U => self.load(reader, 2, I64)?,
            opcode::I64_LOAD32_S | opcode::I64_LOAD32_U => self.load(reader, 4, I64)?,
            opcode::I32_STORE => self.store(reader, 4, I32)?,
            opcode::I64_STORE => self.store(reader, 8, I64)?,
            opcode::F32_STORE => self.store(reader, 4, F32)?,
            opcode::F64_STORE => self.store(reader, 8, F64)?,
            opcode::I32_STORE8 => self.store(reader, 1, I32)?,
            opcode::I32_STORE16 => self.store(reader, 2, I32)?,
            opcode::I64_STORE8 => self.store(reader, 1, I64)?,
            opcode::I64_STORE16 => self.store(reader, 2, I64)?,
            opcode::I64_STORE32 => self.store(reader, 4, I64)?,
            opcode::MEMORY_SIZE => {
                let address_type = self.memory_address_type(reader.read_u32()?)?;
                self.push_value(address_type.value_type());
            }
            opcode::MEMORY_GROW => {
                let address_type = self.memory_address_type(reader.read_u32()?)?;
                self.unary(address_type.value_type(), address_type.value_type())?;
            }
            opcode::I32_CONST | opcode::I64_CONST => {
                let start = reader.position();
                let is_i64 = opcode == opcode::I64_CONST;
                let value = if is_i64 {
                    reader.read_i64()?
                } else {
                    i64::from(reader.read_i32()?)
                };
                if let Some(rewrite) = crate::quickening::fixed_constant(
                    self.instruction_start as usize,
                    reader.position() - start,
                    value,
                    is_i64,
                ) {
                    self.rewrites.push(rewrite);
                }
                self.push_value(if is_i64 { I64 } else { I32 });
            }
            opcode::F32_CONST => {
                reader.read_bytes(4)?;
                self.push_value(F32);
            }
            opcode::F64_CONST => {
                reader.read_bytes(8)?;
                self.push_value(F64);
            }
            opcode::I32_EQZ => self.unary(I32, I32)?,
            0x46..=0x4f => self.binary(I32, I32)?,
            opcode::I64_EQZ => self.unary(I64, I32)?,
            0x51..=0x5a => self.binary(I64, I32)?,
            0x5b..=0x60 => self.binary(F32, I32)?,
            0x61..=0x66 => self.binary(F64, I32)?,
            0x67..=0x69 => self.unary(I32, I32)?,
            0x6a..=0x78 => self.binary(I32, I32)?,
            0x79..=0x7b => self.unary(I64, I64)?,
            0x7c..=0x8a => self.binary(I64, I64)?,
            0x8b..=0x91 => self.unary(F32, F32)?,
            0x92..=0x98 => self.binary(F32, F32)?,
            0x99..=0x9f => self.unary(F64, F64)?,
            0xa0..=0xa6 => self.binary(F64, F64)?,
            opcode::I32_WRAP_I64 => self.unary(I64, I32)?,
            opcode::I32_TRUNC_F32_S | opcode::I32_TRUNC_F32_U => self.unary(F32, I32)?,
            opcode::I32_TRUNC_F64_S | opcode::I32_TRUNC_F64_U => self.unary(F64, I32)?,
            opcode::I64_EXTEND_I32_S | opcode::I64_EXTEND_I32_U => self.unary(I32, I64)?,
            opcode::I64_TRUNC_F32_S | opcode::I64_TRUNC_F32_U => self.unary(F32, I64)?,
            opcode::I64_TRUNC_F64_S | opcode::I64_TRUNC_F64_U => self.unary(F64, I64)?,
            opcode::F32_CONVERT_I32_S | opcode::F32_CONVERT_I32_U => self.unary(I32, F32)?,
            opcode::F32_CONVERT_I64_S | opcode::F32_CONVERT_I64_U => self.unary(I64, F32)?,
            opcode::F32_DEMOTE_F64 => self.unary(F64, F32)?,
            opcode::F64_CONVERT_I32_S | opcode::F64_CONVERT_I32_U => self.unary(I32, F64)?,
            opcode::F64_CONVERT_I64_S | opcode::F64_CONVERT_I64_U => self.unary(I64, F64)?,
            opcode::F64_PROMOTE_F32 => self.unary(F32, F64)?,
            opcode::I32_REINTERPRET_F32 => self.unary(F32, I32)?,
            opcode::I64_REINTERPRET_F64 => self.unary(F64, I64)?,
            opcode::F32_REINTERPRET_I32 => self.unary(I32, F32)?,
            opcode::F64_REINTERPRET_I64 => self.unary(I64, F64)?,
            opcode::I32_EXTEND8_S | opcode::I32_EXTEND16_S => self.unary(I32, I32)?,
            opcode::I64_EXTEND8_S | opcode::I64_EXTEND16_S | opcode::I64_EXTEND32_S => self.unary(I64, I64)?,
            opcode::REF_NULL => {
                let heap = parse_heap_type(reader)?;
                self.check_heap_type(heap)?;
                self.push_value(ValType::Ref(RefType::new(true, heap)));
            }
            opcode::REF_IS_NULL => {
                self.pop_reference()?;
                self.push_value(I32);
            }
            opcode::REF_FUNC => {
                let index = reader.read_u32()?;
                let type_index = *self
                    .module
                    .function_space
                    .get(index as usize)
                    .ok_or_else(|| Error::invalid("unknown function"))?;
                if !self.declared_functions.contains(&index) {
                    return invalid("undeclared function reference");
                }
                self.push_value(ValType::Ref(RefType::new(false, HeapType::Concrete(type_index))));
            }
            opcode::REF_EQ => {
                let eqref = ValType::Ref(RefType::new(true, HeapType::Eq));
                self.pop_expecting(eqref)?;
                self.pop_expecting(eqref)?;
                self.push_value(I32);
            }
            opcode::REF_AS_NON_NULL => {
                let reference = self.pop_reference()?;
                self.push(reference.map(|reference| ValType::Ref(reference.as_non_null())));
            }
            opcode::BR_ON_NULL => {
                let depth = reader.read_u32()?;
                let reference = self.pop_reference()?;
                let label = self.label_types(depth)?;
                self.pop_values(&label)?;
                self.push_values(&label);
                self.add_branch_entry(depth, pc);
                self.push(reference.map(|reference| ValType::Ref(reference.as_non_null())));
            }
            opcode::BR_ON_NON_NULL => {
                let depth = reader.read_u32()?;
                let mut label = self.label_types(depth)?;
                let Some(ValType::Ref(label_reference)) = label.pop() else {
                    return type_mismatch();
                };
                let reference = self.pop_reference()?;
                if let Some(reference) = reference
                    && !self.types.ref_subtype(reference.as_non_null(), label_reference)
                {
                    return type_mismatch();
                }
                self.pop_values(&label)?;
                self.push_values(&label);
                // The taken branch keeps the label's values and the reference.
                self.push_value(ValType::Ref(label_reference));
                self.add_branch_entry(depth, pc);
                self.pop()?;
            }
            opcode::PREFIX_MISC => self.validate_misc(reader)?,
            opcode::PREFIX_GC => self.validate_gc(reader, pc)?,
            opcode::PREFIX_SIMD => self.validate_simd(reader)?,
            opcode::PREFIX_ATOMIC => self.validate_atomic(reader)?,
            _ => return Err(Error::malformed(format!("illegal opcode {opcode:#x}"))),
        }
        Ok(())
    }

    fn validate_misc(&mut self, reader: &mut Reader<'_>) -> Result<()> {
        use ValType::{F32, F64, I32, I64};
        let sub_opcode = reader.read_u32()?;
        match sub_opcode {
            opcode::misc::I32_TRUNC_SAT_F32_S | opcode::misc::I32_TRUNC_SAT_F32_U => self.unary(F32, I32)?,
            opcode::misc::I32_TRUNC_SAT_F64_S | opcode::misc::I32_TRUNC_SAT_F64_U => self.unary(F64, I32)?,
            opcode::misc::I64_TRUNC_SAT_F32_S | opcode::misc::I64_TRUNC_SAT_F32_U => self.unary(F32, I64)?,
            opcode::misc::I64_TRUNC_SAT_F64_S | opcode::misc::I64_TRUNC_SAT_F64_U => self.unary(F64, I64)?,
            opcode::misc::MEMORY_INIT => {
                let data = reader.read_u32()?;
                let address_type = self.memory_address_type(reader.read_u32()?)?;
                self.check_data_index(data)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(address_type.value_type())?;
            }
            opcode::misc::DATA_DROP => self.check_data_index(reader.read_u32()?)?,
            opcode::misc::MEMORY_COPY => {
                let destination = self.memory_address_type(reader.read_u32()?)?;
                let source = self.memory_address_type(reader.read_u32()?)?;
                let length = if destination == AddressType::I64 && source == AddressType::I64 {
                    I64
                } else {
                    I32
                };
                self.pop_expecting(length)?;
                self.pop_expecting(source.value_type())?;
                self.pop_expecting(destination.value_type())?;
            }
            opcode::misc::MEMORY_FILL => {
                let address_type = self.memory_address_type(reader.read_u32()?)?;
                self.pop_expecting(address_type.value_type())?;
                self.pop_expecting(I32)?;
                self.pop_expecting(address_type.value_type())?;
            }
            opcode::misc::TABLE_INIT => {
                let element = self.element_type(reader.read_u32()?)?;
                let table = self.table(reader.read_u32()?)?;
                if !self.types.ref_subtype(element, table.element) {
                    return type_mismatch();
                }
                self.pop_expecting(I32)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(table.limits.address_type.value_type())?;
            }
            opcode::misc::ELEM_DROP => {
                self.element_type(reader.read_u32()?)?;
            }
            opcode::misc::TABLE_COPY => {
                let destination = self.table(reader.read_u32()?)?;
                let source = self.table(reader.read_u32()?)?;
                if !self.types.ref_subtype(source.element, destination.element) {
                    return type_mismatch();
                }
                let length = if destination.limits.address_type == AddressType::I64
                    && source.limits.address_type == AddressType::I64
                {
                    I64
                } else {
                    I32
                };
                self.pop_expecting(length)?;
                self.pop_expecting(source.limits.address_type.value_type())?;
                self.pop_expecting(destination.limits.address_type.value_type())?;
            }
            opcode::misc::TABLE_GROW => {
                let table = self.table(reader.read_u32()?)?;
                let address = table.limits.address_type.value_type();
                self.pop_expecting(address)?;
                self.pop_expecting(ValType::Ref(table.element))?;
                self.push_value(address);
            }
            opcode::misc::TABLE_SIZE => {
                let table = self.table(reader.read_u32()?)?;
                self.push_value(table.limits.address_type.value_type());
            }
            opcode::misc::TABLE_FILL => {
                let table = self.table(reader.read_u32()?)?;
                let address = table.limits.address_type.value_type();
                self.pop_expecting(address)?;
                self.pop_expecting(ValType::Ref(table.element))?;
                self.pop_expecting(address)?;
            }
            _ => return Err(Error::malformed(format!("illegal opcode 0xfc {sub_opcode:#x}"))),
        }
        Ok(())
    }

    fn read_cast_reference(&self, reader: &mut Reader<'_>, nullable: bool) -> Result<RefType> {
        let heap = parse_heap_type(reader)?;
        self.check_heap_type(heap)?;
        Ok(RefType::new(nullable, heap))
    }

    fn validate_gc(&mut self, reader: &mut Reader<'_>, pc: u32) -> Result<()> {
        use ValType::I32;
        let sub_opcode = reader.read_u32()?;
        let reference = |nullable, type_index| ValType::Ref(RefType::new(nullable, HeapType::Concrete(type_index)));
        match sub_opcode {
            opcode::gc::STRUCT_NEW => {
                let type_index = reader.read_u32()?;
                let fields = self.struct_type(type_index)?;
                for field in fields.iter().rev() {
                    self.pop_expecting(field.storage.unpacked())?;
                }
                self.push_value(reference(false, type_index));
            }
            opcode::gc::STRUCT_NEW_DEFAULT => {
                let type_index = reader.read_u32()?;
                let fields = self.struct_type(type_index)?;
                if !fields.iter().all(|field| field.storage.unpacked().is_defaultable()) {
                    return invalid("field type is not defaultable");
                }
                self.push_value(reference(false, type_index));
            }
            opcode::gc::STRUCT_GET | opcode::gc::STRUCT_GET_S | opcode::gc::STRUCT_GET_U => {
                let type_index = reader.read_u32()?;
                let field_index = reader.read_u32()?;
                let fields = self.struct_type(type_index)?;
                let field = *fields
                    .get(field_index as usize)
                    .ok_or_else(|| Error::invalid("unknown field"))?;
                if field.storage.is_packed() != (sub_opcode != opcode::gc::STRUCT_GET) {
                    return invalid("type mismatch: field is packed or not as the instruction requires");
                }
                self.pop_expecting(reference(true, type_index))?;
                self.push_value(field.storage.unpacked());
            }
            opcode::gc::STRUCT_SET => {
                let type_index = reader.read_u32()?;
                let field_index = reader.read_u32()?;
                let fields = self.struct_type(type_index)?;
                let field = *fields
                    .get(field_index as usize)
                    .ok_or_else(|| Error::invalid("unknown field"))?;
                if !field.mutable {
                    return invalid("field is immutable");
                }
                self.pop_expecting(field.storage.unpacked())?;
                self.pop_expecting(reference(true, type_index))?;
            }
            opcode::gc::ARRAY_NEW => {
                let type_index = reader.read_u32()?;
                let element = self.array_type(type_index)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(element.storage.unpacked())?;
                self.push_value(reference(false, type_index));
            }
            opcode::gc::ARRAY_NEW_DEFAULT => {
                let type_index = reader.read_u32()?;
                let element = self.array_type(type_index)?;
                if !element.storage.unpacked().is_defaultable() {
                    return invalid("array type is not defaultable");
                }
                self.pop_expecting(I32)?;
                self.push_value(reference(false, type_index));
            }
            opcode::gc::ARRAY_NEW_FIXED => {
                let type_index = reader.read_u32()?;
                let count = reader.read_u32()?;
                let element = self.array_type(type_index)?;
                // NB: Beyond the operands on the stack, an unreachable frame's stack is polymorphic and every pop
                //     succeeds; those pops change nothing, so they are skipped rather than done up to 2^32 times.
                let frame = self.frames.last().expect("validation always has a frame");
                let available = self.operands.len() - frame.height;
                if count as usize > available && !frame.unreachable {
                    return type_mismatch();
                }
                for _ in 0..(count as usize).min(available) {
                    self.pop_expecting(element.storage.unpacked())?;
                }
                self.push_value(reference(false, type_index));
            }
            opcode::gc::ARRAY_NEW_DATA => {
                let type_index = reader.read_u32()?;
                let data = reader.read_u32()?;
                let element = self.array_type(type_index)?;
                if element.storage.unpacked().is_reference() {
                    return type_mismatch();
                }
                self.check_data_index(data)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(I32)?;
                self.push_value(reference(false, type_index));
            }
            opcode::gc::ARRAY_NEW_ELEM => {
                let type_index = reader.read_u32()?;
                let element_index = reader.read_u32()?;
                let element = self.array_type(type_index)?;
                let segment = self.element_type(element_index)?;
                if !self
                    .types
                    .value_subtype(ValType::Ref(segment), element.storage.unpacked())
                    || element.storage.is_packed()
                {
                    return type_mismatch();
                }
                self.pop_expecting(I32)?;
                self.pop_expecting(I32)?;
                self.push_value(reference(false, type_index));
            }
            opcode::gc::ARRAY_GET | opcode::gc::ARRAY_GET_S | opcode::gc::ARRAY_GET_U => {
                let type_index = reader.read_u32()?;
                let element = self.array_type(type_index)?;
                if element.storage.is_packed() != (sub_opcode != opcode::gc::ARRAY_GET) {
                    return invalid("type mismatch: element is packed or not as the instruction requires");
                }
                self.pop_expecting(I32)?;
                self.pop_expecting(reference(true, type_index))?;
                self.push_value(element.storage.unpacked());
            }
            opcode::gc::ARRAY_SET => {
                let type_index = reader.read_u32()?;
                let element = self.array_type(type_index)?;
                if !element.mutable {
                    return invalid("array is immutable");
                }
                self.pop_expecting(element.storage.unpacked())?;
                self.pop_expecting(I32)?;
                self.pop_expecting(reference(true, type_index))?;
            }
            opcode::gc::ARRAY_LEN => {
                self.pop_expecting(ValType::Ref(RefType::new(true, HeapType::Array)))?;
                self.push_value(I32);
            }
            opcode::gc::ARRAY_FILL => {
                let type_index = reader.read_u32()?;
                let element = self.array_type(type_index)?;
                if !element.mutable {
                    return invalid("array is immutable");
                }
                self.pop_expecting(I32)?;
                self.pop_expecting(element.storage.unpacked())?;
                self.pop_expecting(I32)?;
                self.pop_expecting(reference(true, type_index))?;
            }
            opcode::gc::ARRAY_COPY => {
                let destination_index = reader.read_u32()?;
                let source_index = reader.read_u32()?;
                let destination = self.array_type(destination_index)?;
                let source = self.array_type(source_index)?;
                if !destination.mutable {
                    return invalid("array is immutable");
                }
                if !self.types.storage_subtype(source.storage, destination.storage) {
                    return type_mismatch();
                }
                self.pop_expecting(I32)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(reference(true, source_index))?;
                self.pop_expecting(I32)?;
                self.pop_expecting(reference(true, destination_index))?;
            }
            opcode::gc::ARRAY_INIT_DATA => {
                let type_index = reader.read_u32()?;
                let data = reader.read_u32()?;
                let element = self.array_type(type_index)?;
                if !element.mutable {
                    return invalid("array is immutable");
                }
                if element.storage.unpacked().is_reference() {
                    return type_mismatch();
                }
                self.check_data_index(data)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(reference(true, type_index))?;
            }
            opcode::gc::ARRAY_INIT_ELEM => {
                let type_index = reader.read_u32()?;
                let element_index = reader.read_u32()?;
                let element = self.array_type(type_index)?;
                if !element.mutable {
                    return invalid("array is immutable");
                }
                let segment = self.element_type(element_index)?;
                if !self
                    .types
                    .value_subtype(ValType::Ref(segment), element.storage.unpacked())
                    || element.storage.is_packed()
                {
                    return type_mismatch();
                }
                self.pop_expecting(I32)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(reference(true, type_index))?;
            }
            opcode::gc::REF_TEST | opcode::gc::REF_TEST_NULL | opcode::gc::REF_CAST | opcode::gc::REF_CAST_NULL => {
                let nullable = matches!(sub_opcode, opcode::gc::REF_TEST_NULL | opcode::gc::REF_CAST_NULL);
                let target = self.read_cast_reference(reader, nullable)?;
                let top = RefType::new(true, self.types.top(target.heap));
                self.pop_expecting(ValType::Ref(top))?;
                if matches!(sub_opcode, opcode::gc::REF_TEST | opcode::gc::REF_TEST_NULL) {
                    self.push_value(I32);
                } else {
                    self.push_value(ValType::Ref(target));
                }
            }
            opcode::gc::BR_ON_CAST | opcode::gc::BR_ON_CAST_FAIL => {
                let flags = reader.read_u8()?;
                if flags & !0x03 != 0 {
                    return Err(Error::malformed("malformed cast flags"));
                }
                let depth = reader.read_u32()?;
                let source = self.read_cast_reference(reader, flags & 0x01 != 0)?;
                let target = self.read_cast_reference(reader, flags & 0x02 != 0)?;
                if !self.types.ref_subtype(target, source) {
                    return type_mismatch();
                }
                let difference = RefType::new(source.nullable && !target.nullable, source.heap);
                let (taken, fallthrough) = if sub_opcode == opcode::gc::BR_ON_CAST {
                    (target, difference)
                } else {
                    (difference, target)
                };
                let mut label = self.label_types(depth)?;
                let Some(ValType::Ref(label_reference)) = label.pop() else {
                    return type_mismatch();
                };
                if !self.types.ref_subtype(taken, label_reference) {
                    return type_mismatch();
                }
                self.pop_expecting(ValType::Ref(source))?;
                self.pop_values(&label)?;
                self.push_values(&label);
                self.push_value(ValType::Ref(taken));
                self.add_branch_entry(depth, pc);
                self.pop()?;
                self.push_value(ValType::Ref(fallthrough));
            }
            opcode::gc::ANY_CONVERT_EXTERN | opcode::gc::EXTERN_CONVERT_ANY => {
                let (from, to) = if sub_opcode == opcode::gc::ANY_CONVERT_EXTERN {
                    (HeapType::Extern, HeapType::Any)
                } else {
                    (HeapType::Any, HeapType::Extern)
                };
                let operand = self.pop_expecting(ValType::Ref(RefType::new(true, from)))?;
                let nullable = match operand {
                    Some(ValType::Ref(reference)) => reference.nullable,
                    _ => true,
                };
                self.push_value(ValType::Ref(RefType::new(nullable, to)));
            }
            opcode::gc::REF_I31 => self.unary(I32, ValType::Ref(RefType::new(false, HeapType::I31)))?,
            opcode::gc::I31_GET_S | opcode::gc::I31_GET_U => {
                self.unary(ValType::Ref(RefType::new(true, HeapType::I31)), I32)?;
            }
            _ => return Err(Error::malformed(format!("illegal opcode 0xfb {sub_opcode:#x}"))),
        }
        Ok(())
    }

    fn validate_simd(&mut self, reader: &mut Reader<'_>) -> Result<()> {
        use ValType::{I32, V128};
        let sub_opcode = reader.read_u32()?;
        let kind =
            simd_kind(sub_opcode).ok_or_else(|| Error::malformed(format!("illegal opcode 0xfd {sub_opcode:#x}")))?;
        self.checked_access = None;
        match kind {
            SimdKind::Load { size } => self.load(reader, size, V128)?,
            SimdKind::Store { size } => self.store(reader, size, V128)?,
            SimdKind::LoadLane { size } | SimdKind::StoreLane { size } => {
                let address_type = self.read_memory_argument(reader, size)?;
                let lane = reader.read_u8()?;
                if u32::from(lane) >= 16 / size {
                    return invalid("invalid lane index");
                }
                self.pop_expecting(V128)?;
                self.pop_expecting(address_type.value_type())?;
                if matches!(kind, SimdKind::LoadLane { .. }) {
                    self.push_value(V128);
                }
            }
            _ => {}
        }
        if let Some((memory, offset)) = self.checked_access.take() {
            self.rewrites.push(crate::quickening::checked_simd_access(
                &self.module.bytes,
                self.instruction_start as usize,
                sub_opcode,
                memory,
                offset,
                reader.position(),
            ));
        }
        match kind {
            SimdKind::Load { .. } | SimdKind::Store { .. } | SimdKind::LoadLane { .. } | SimdKind::StoreLane { .. } => {
            }
            SimdKind::Const => {
                reader.read_bytes(16)?;
                self.push_value(V128);
            }
            SimdKind::Shuffle => {
                for _ in 0..16 {
                    if reader.read_u8()? >= 32 {
                        return invalid("invalid lane index");
                    }
                }
                self.binary(V128, V128)?;
            }
            SimdKind::ExtractLane { lanes, result } => {
                if reader.read_u8()? >= lanes {
                    return invalid("invalid lane index");
                }
                self.unary(V128, result)?;
            }
            SimdKind::ReplaceLane { lanes, operand } => {
                if reader.read_u8()? >= lanes {
                    return invalid("invalid lane index");
                }
                self.pop_expecting(operand)?;
                self.unary(V128, V128)?;
            }
            SimdKind::Splat(operand) => self.unary(operand, V128)?,
            SimdKind::Shift => {
                self.pop_expecting(I32)?;
                self.unary(V128, V128)?;
            }
            SimdKind::Test => self.unary(V128, I32)?,
            SimdKind::Unary => self.unary(V128, V128)?,
            SimdKind::Binary => self.binary(V128, V128)?,
            SimdKind::Ternary => {
                self.pop_expecting(V128)?;
                self.binary(V128, V128)?;
            }
        }
        Ok(())
    }

    fn validate_atomic(&mut self, reader: &mut Reader<'_>) -> Result<()> {
        use ValType::{I32, I64};
        let sub_opcode = reader.read_u32()?;
        let kind =
            atomic_kind(sub_opcode).ok_or_else(|| Error::malformed(format!("illegal opcode 0xfe {sub_opcode:#x}")))?;
        match kind {
            AtomicKind::Fence => {
                if reader.read_u8()? != 0 {
                    return Err(Error::malformed("malformed atomic fence"));
                }
            }
            AtomicKind::Notify => {
                let address_type = self.read_atomic_memory_argument(reader, 4)?;
                self.pop_expecting(I32)?;
                self.pop_expecting(address_type.value_type())?;
                self.push_value(I32);
            }
            AtomicKind::Wait { value } => {
                let size = if value == I32 { 4 } else { 8 };
                let address_type = self.read_atomic_memory_argument(reader, size)?;
                self.pop_expecting(I64)?;
                self.pop_expecting(value)?;
                self.pop_expecting(address_type.value_type())?;
                self.push_value(I32);
            }
            AtomicKind::Load { value, size } => {
                let address_type = self.read_atomic_memory_argument(reader, size)?;
                self.unary(address_type.value_type(), value)?;
            }
            AtomicKind::Store { value, size } => {
                let address_type = self.read_atomic_memory_argument(reader, size)?;
                self.pop_expecting(value)?;
                self.pop_expecting(address_type.value_type())?;
            }
            AtomicKind::ReadModifyWrite { value, size } => {
                let address_type = self.read_atomic_memory_argument(reader, size)?;
                self.pop_expecting(value)?;
                self.pop_expecting(address_type.value_type())?;
                self.push_value(value);
            }
            AtomicKind::CompareExchange { value, size } => {
                let address_type = self.read_atomic_memory_argument(reader, size)?;
                self.pop_expecting(value)?;
                self.pop_expecting(value)?;
                self.pop_expecting(address_type.value_type())?;
                self.push_value(value);
            }
        }
        Ok(())
    }
}

fn check_heap_type(module: &Module, heap: HeapType) -> Result<()> {
    if let HeapType::Concrete(index) = heap
        && index as usize >= module.types.len()
    {
        return invalid("unknown type");
    }
    Ok(())
}

fn check_value_type(module: &Module, value: ValType) -> Result<()> {
    match value {
        ValType::Ref(reference) => check_heap_type(module, reference.heap),
        _ => Ok(()),
    }
}

fn check_limits(limits: &Limits, bound: u64) -> Result<()> {
    if limits.min > bound {
        return invalid("size minimum must not be greater than maximum");
    }
    if let Some(max) = limits.max {
        if max > bound {
            return invalid("size maximum must not be greater than bound");
        }
        if max < limits.min {
            return invalid("size minimum must not be greater than maximum");
        }
    }
    Ok(())
}

fn check_table_type(module: &Module, table: &crate::types::TableType) -> Result<()> {
    check_heap_type(module, table.element.heap)?;
    let bound = match table.limits.address_type {
        AddressType::I32 => u64::from(u32::MAX),
        AddressType::I64 => u64::MAX,
    };
    check_limits(&table.limits, bound)
}

fn check_memory_type(memory: &crate::types::MemoryType) -> Result<()> {
    let bound = match memory.limits.address_type {
        AddressType::I32 => MEMORY32_MAX_PAGES,
        AddressType::I64 => MEMORY64_MAX_PAGES,
    };
    if memory.limits.min > bound {
        return invalid("memory size must be at most 65536 pages (4GiB)");
    }
    if let Some(max) = memory.limits.max {
        if max > bound {
            return invalid("memory size must be at most 65536 pages (4GiB)");
        }
        if max < memory.limits.min {
            return invalid("size minimum must not be greater than maximum");
        }
    }
    if memory.limits.shared && memory.limits.max.is_none() {
        return invalid("shared memory must have maximum");
    }
    Ok(())
}

fn check_tag_type(module: &Module, type_index: u32) -> Result<()> {
    match module
        .types
        .get(type_index as usize)
        .map(|sub_type| &sub_type.composite)
    {
        None => invalid("unknown type"),
        Some(CompositeType::Func(function)) if function.results.is_empty() => Ok(()),
        Some(CompositeType::Func(_)) => invalid("non-empty tag result type"),
        Some(_) => type_mismatch(),
    }
}

/// Validate a constant expression of type `expected`. `global_limit` is the number of globals it may read: the
/// globals before the one it initializes, or all of them.
fn validate_constant_expression(
    module: &Module,
    types: &TypeContext<'_>,
    expression: &ConstantExpression,
    expected: ValType,
    global_limit: u32,
) -> Result<()> {
    use ValType::{F32, F64, I32, I64, V128};

    let mut reader = Reader::with_range(
        &module.bytes,
        expression.code.start as usize,
        expression.code.end as usize,
    );
    let mut stack: Vec<ValType> = Vec::new();
    let pop = |stack: &mut Vec<ValType>, expected: ValType| -> Result<()> {
        match stack.pop() {
            Some(actual) if types.value_subtype(actual, expected) => Ok(()),
            _ => type_mismatch(),
        }
    };
    loop {
        let opcode = reader.read_u8()?;
        match opcode {
            opcode::END => break,
            opcode::I32_CONST => {
                reader.read_i32()?;
                stack.push(I32);
            }
            opcode::I64_CONST => {
                reader.read_i64()?;
                stack.push(I64);
            }
            opcode::F32_CONST => {
                reader.read_bytes(4)?;
                stack.push(F32);
            }
            opcode::F64_CONST => {
                reader.read_bytes(8)?;
                stack.push(F64);
            }
            opcode::REF_NULL => {
                let heap = parse_heap_type(&mut reader)?;
                check_heap_type(module, heap)?;
                stack.push(ValType::Ref(RefType::new(true, heap)));
            }
            opcode::REF_FUNC => {
                let index = reader.read_u32()?;
                let type_index = *module
                    .function_space
                    .get(index as usize)
                    .ok_or_else(|| Error::invalid("unknown function"))?;
                stack.push(ValType::Ref(RefType::new(false, HeapType::Concrete(type_index))));
            }
            opcode::GLOBAL_GET => {
                let index = reader.read_u32()?;
                let global = module
                    .global_space
                    .get(index as usize)
                    .ok_or_else(|| Error::invalid("unknown global"))?;
                if index >= global_limit {
                    return invalid("unknown global");
                }
                if global.mutable {
                    return invalid("constant expression required");
                }
                stack.push(global.value);
            }
            opcode::I32_ADD | opcode::I32_SUB | opcode::I32_MUL => {
                pop(&mut stack, I32)?;
                pop(&mut stack, I32)?;
                stack.push(I32);
            }
            opcode::I64_ADD | opcode::I64_SUB | opcode::I64_MUL => {
                pop(&mut stack, I64)?;
                pop(&mut stack, I64)?;
                stack.push(I64);
            }
            opcode::PREFIX_SIMD => {
                reader.read_u32()?;
                reader.read_bytes(16)?;
                stack.push(V128);
            }
            opcode::PREFIX_GC => {
                let sub_opcode = reader.read_u32()?;
                let concrete = |type_index| ValType::Ref(RefType::new(false, HeapType::Concrete(type_index)));
                let composite = |type_index: u32| {
                    module
                        .types
                        .get(type_index as usize)
                        .map(|sub_type| sub_type.composite.clone())
                        .ok_or_else(|| Error::invalid("unknown type"))
                };
                match sub_opcode {
                    opcode::gc::STRUCT_NEW | opcode::gc::STRUCT_NEW_DEFAULT => {
                        let type_index = reader.read_u32()?;
                        let CompositeType::Struct(fields) = composite(type_index)? else {
                            return type_mismatch();
                        };
                        if sub_opcode == opcode::gc::STRUCT_NEW {
                            for field in fields.iter().rev() {
                                pop(&mut stack, field.storage.unpacked())?;
                            }
                        } else if !fields.iter().all(|field| field.storage.unpacked().is_defaultable()) {
                            return invalid("field type is not defaultable");
                        }
                        stack.push(concrete(type_index));
                    }
                    opcode::gc::ARRAY_NEW | opcode::gc::ARRAY_NEW_DEFAULT => {
                        let type_index = reader.read_u32()?;
                        let CompositeType::Array(element) = composite(type_index)? else {
                            return type_mismatch();
                        };
                        pop(&mut stack, I32)?;
                        if sub_opcode == opcode::gc::ARRAY_NEW {
                            pop(&mut stack, element.storage.unpacked())?;
                        } else if !element.storage.unpacked().is_defaultable() {
                            return invalid("array type is not defaultable");
                        }
                        stack.push(concrete(type_index));
                    }
                    opcode::gc::ARRAY_NEW_FIXED => {
                        let type_index = reader.read_u32()?;
                        let count = reader.read_u32()?;
                        let CompositeType::Array(element) = composite(type_index)? else {
                            return type_mismatch();
                        };
                        for _ in 0..count {
                            pop(&mut stack, element.storage.unpacked())?;
                        }
                        stack.push(concrete(type_index));
                    }
                    opcode::gc::REF_I31 => {
                        pop(&mut stack, I32)?;
                        stack.push(ValType::Ref(RefType::new(false, HeapType::I31)));
                    }
                    opcode::gc::ANY_CONVERT_EXTERN | opcode::gc::EXTERN_CONVERT_ANY => {
                        let (from, to) = if sub_opcode == opcode::gc::ANY_CONVERT_EXTERN {
                            (HeapType::Extern, HeapType::Any)
                        } else {
                            (HeapType::Any, HeapType::Extern)
                        };
                        let Some(ValType::Ref(operand)) = stack.pop() else {
                            return type_mismatch();
                        };
                        if !types.ref_subtype(operand, RefType::new(true, from)) {
                            return type_mismatch();
                        }
                        stack.push(ValType::Ref(RefType::new(operand.nullable, to)));
                    }
                    _ => return invalid("constant expression required"),
                }
            }
            _ => return invalid("constant expression required"),
        }
    }
    if !reader.is_at_end() {
        return Err(Error::malformed("section size mismatch"));
    }
    match stack.as_slice() {
        [actual] if types.value_subtype(*actual, expected) => Ok(()),
        _ => type_mismatch(),
    }
}

/// Collect the functions `ref.func` may refer to inside function bodies: those referenced anywhere else in the
/// module.
fn declared_functions(module: &Module) -> HashSet<u32> {
    let mut functions = HashSet::new();
    let mut scan = |expression: &ConstantExpression| {
        let mut reader = Reader::with_range(
            &module.bytes,
            expression.code.start as usize,
            expression.code.end as usize,
        );
        while let Ok(opcode) = reader.read_u8() {
            match opcode {
                opcode::REF_FUNC => {
                    if let Ok(index) = reader.read_u32() {
                        functions.insert(index);
                    }
                }
                opcode::I32_CONST => {
                    let _ = reader.read_i32();
                }
                opcode::I64_CONST => {
                    let _ = reader.read_i64();
                }
                opcode::F32_CONST => {
                    let _ = reader.read_bytes(4);
                }
                opcode::F64_CONST => {
                    let _ = reader.read_bytes(8);
                }
                opcode::REF_NULL => {
                    let _ = parse_heap_type(&mut reader);
                }
                opcode::GLOBAL_GET => {
                    let _ = reader.read_u32();
                }
                opcode::PREFIX_SIMD => {
                    let _ = reader.read_u32();
                    let _ = reader.read_bytes(16);
                }
                opcode::PREFIX_GC => match reader.read_u32() {
                    Ok(
                        opcode::gc::STRUCT_NEW
                        | opcode::gc::STRUCT_NEW_DEFAULT
                        | opcode::gc::ARRAY_NEW
                        | opcode::gc::ARRAY_NEW_DEFAULT,
                    ) => {
                        let _ = reader.read_u32();
                    }
                    Ok(opcode::gc::ARRAY_NEW_FIXED) => {
                        let _ = reader.read_u32();
                        let _ = reader.read_u32();
                    }
                    _ => {}
                },
                _ => {}
            }
        }
    };
    for global in &module.globals {
        scan(&global.initializer);
    }
    for table in &module.tables {
        if let Some(initializer) = &table.initializer {
            scan(initializer);
        }
    }
    let mut listed = Vec::new();
    for element in &module.elements {
        match &element.items {
            ElementItems::Functions(indices) => listed.extend(indices.iter().copied()),
            ElementItems::Expressions(expressions) => expressions.iter().for_each(&mut scan),
        }
    }
    for export in &module.exports {
        if export.kind == ExternalKind::Function {
            listed.push(export.index);
        }
    }
    functions.extend(listed);
    functions
}

fn validate_types(module: &Module, identities: &mut Vec<u32>) -> Result<()> {
    let mut canonicalizer = TypeCanonicalizer::new();
    // How many supertypes each type has above it.
    let mut depths: Vec<u32> = Vec::with_capacity(module.types.len());
    for group in &module.recursion_groups {
        let end = group.start + group.count;
        let sub_types = &module.types[group.start as usize..end as usize];
        // Types may refer to any type of their own group, but only to earlier groups otherwise.
        for sub_type in sub_types {
            let check = |value: ValType| match value {
                ValType::Ref(RefType {
                    heap: HeapType::Concrete(index),
                    ..
                }) if index >= end => invalid("unknown type"),
                _ => Ok(()),
            };
            let check_storage = |storage: StorageType| match storage {
                StorageType::Val(value) => check(value),
                _ => Ok(()),
            };
            match &sub_type.composite {
                CompositeType::Func(function) => {
                    function
                        .params
                        .iter()
                        .chain(&function.results)
                        .try_for_each(|&value| check(value))?;
                }
                CompositeType::Struct(fields) => fields.iter().try_for_each(|field| check_storage(field.storage))?,
                CompositeType::Array(field) => check_storage(field.storage)?,
            }
            if sub_type.supertypes.len() > 1 {
                return invalid("multiple supertypes are not supported");
            }
        }
        let group_identities =
            canonicalizer.canonicalize_group(sub_types, group.start, |index| identities[index as usize]);
        identities.extend(group_identities);
        let types = TypeContext {
            types: &module.types,
            identities: Some(identities),
        };
        for (offset, sub_type) in sub_types.iter().enumerate() {
            let index = group.start + offset as u32;
            let mut depth = 0;
            for &supertype in &sub_type.supertypes {
                if supertype >= index {
                    return invalid("unknown type: supertypes must be declared before their subtypes");
                }
                depth = depths[supertype as usize] + 1;
                if depth > MAX_SUBTYPING_DEPTH {
                    return invalid("subtyping depth is too large");
                }
                let parent = &module.types[supertype as usize];
                if parent.is_final {
                    return invalid("sub type of a final type");
                }
                if !types.composite_subtype(&sub_type.composite, &parent.composite) {
                    return invalid("sub type does not match its supertype");
                }
            }
            depths.push(depth);
        }
    }
    Ok(())
}

pub fn validate(module: &mut Module) -> Result<()> {
    let mut identities = Vec::with_capacity(module.types.len());
    validate_types(module, &mut identities)?;
    module.type_identities = identities;
    let types = TypeContext {
        types: &module.types,
        identities: Some(&module.type_identities),
    };

    for import in &module.imports {
        match &import.description {
            ImportDescription::Function(type_index) => match module.types.get(*type_index as usize) {
                None => return invalid("unknown type"),
                Some(sub_type) if sub_type.as_func().is_none() => return type_mismatch(),
                _ => {}
            },
            ImportDescription::Table(table) => check_table_type(module, table)?,
            ImportDescription::Memory(memory) => check_memory_type(memory)?,
            ImportDescription::Global(global) => check_value_type(module, global.value)?,
            ImportDescription::Tag(tag) => check_tag_type(module, tag.type_index)?,
        }
    }
    for &type_index in &module.functions {
        match module.types.get(type_index as usize) {
            None => return invalid("unknown type"),
            Some(sub_type) if sub_type.as_func().is_none() => return type_mismatch(),
            _ => {}
        }
    }
    let global_count = module.global_space.len() as u32;
    for table in &module.tables {
        check_table_type(module, &table.ty)?;
        match &table.initializer {
            Some(initializer) => {
                validate_constant_expression(
                    module,
                    &types,
                    initializer,
                    ValType::Ref(table.ty.element),
                    module.imported_global_count,
                )?;
            }
            None if !table.ty.element.nullable => return type_mismatch(),
            None => {}
        }
    }
    for memory in &module.memories {
        check_memory_type(memory)?;
    }
    for tag in &module.tags {
        check_tag_type(module, tag.type_index)?;
    }
    for (offset, global) in module.globals.iter().enumerate() {
        check_value_type(module, global.ty.value)?;
        let limit = module.imported_global_count + offset as u32;
        validate_constant_expression(module, &types, &global.initializer, global.ty.value, limit)?;
    }
    let mut export_names = HashSet::new();
    for export in &module.exports {
        if !export_names.insert(export.name.as_str()) {
            return invalid("duplicate export name");
        }
        let exists = match export.kind {
            ExternalKind::Function => (export.index as usize) < module.function_space.len(),
            ExternalKind::Table => (export.index as usize) < module.table_space.len(),
            ExternalKind::Memory => (export.index as usize) < module.memory_space.len(),
            ExternalKind::Global => (export.index as usize) < module.global_space.len(),
            ExternalKind::Tag => (export.index as usize) < module.tag_space.len(),
        };
        if !exists {
            return invalid(match export.kind {
                ExternalKind::Function => "unknown function",
                ExternalKind::Table => "unknown table",
                ExternalKind::Memory => "unknown memory",
                ExternalKind::Global => "unknown global",
                ExternalKind::Tag => "unknown tag",
            });
        }
    }
    if let Some(start) = module.start {
        let Some(&type_index) = module.function_space.get(start as usize) else {
            return invalid("unknown function");
        };
        let function = module.types[type_index as usize]
            .as_func()
            .expect("function types are checked");
        if !function.params.is_empty() || !function.results.is_empty() {
            return invalid("start function");
        }
    }
    for element in &module.elements {
        check_heap_type(module, element.ty.heap)?;
        match &element.items {
            ElementItems::Functions(indices) => {
                for &index in indices {
                    if index as usize >= module.function_space.len() {
                        return invalid("unknown function");
                    }
                }
            }
            ElementItems::Expressions(expressions) => {
                for expression in expressions {
                    validate_constant_expression(module, &types, expression, ValType::Ref(element.ty), global_count)?;
                }
            }
        }
        if let ElementMode::Active { table, offset } = &element.mode {
            let table = module
                .table_space
                .get(*table as usize)
                .ok_or_else(|| Error::invalid("unknown table"))?;
            validate_constant_expression(
                module,
                &types,
                offset,
                table.limits.address_type.value_type(),
                global_count,
            )?;
            if !types.ref_subtype(element.ty, table.element) {
                return type_mismatch();
            }
        }
    }
    for data in &module.data {
        if let DataMode::Active { memory, offset } = &data.mode {
            let memory = module
                .memory_space
                .get(*memory as usize)
                .ok_or_else(|| Error::invalid("unknown memory"))?;
            validate_constant_expression(
                module,
                &types,
                offset,
                memory.limits.address_type.value_type(),
                global_count,
            )?;
        }
    }

    let declared = declared_functions(module);
    let mut infos = Vec::with_capacity(module.code.len());
    let mut rewrites = Vec::new();
    for (offset, body) in module.code.iter().enumerate() {
        let type_index = module.functions[offset];
        let function = module.types[type_index as usize]
            .as_func()
            .expect("function types are checked");
        if function.params.len() + body.declared_local_count as usize > MAX_FUNCTION_LOCALS {
            return invalid("too many locals");
        }
        let mut locals = function.params.clone();
        locals.reserve(body.declared_local_count as usize);
        for &(count, ty) in &body.locals {
            check_value_type(module, ty)?;
            locals.extend(std::iter::repeat_n(ty, count as usize));
        }
        let mut local_initialized = vec![true; locals.len()];
        for (index, local) in locals.iter().enumerate().skip(function.params.len()) {
            local_initialized[index] = local.is_defaultable();
        }
        let mut validator = FunctionValidator {
            module,
            types: TypeContext {
                types: &module.types,
                identities: Some(&module.type_identities),
            },
            declared_functions: &declared,
            locals,
            local_initialized,
            initialization_log: Vec::new(),
            operands: Vec::new(),
            slot_height: 0,
            max_slot_height: 0,
            frames: Vec::new(),
            side_table: Vec::new(),
            try_regions: Vec::new(),
            catch_clauses: Vec::new(),
            exception_local_count: 0,
            results: function.results.clone(),
            instruction_start: body.code.start,
            rewrites: Vec::new(),
            checked_access: None,
        };
        validator.push_frame(
            FrameKind::Function,
            Vec::new(),
            function.results.clone(),
            body.code.start,
        );
        let mut reader = Reader::with_range(&module.bytes, body.code.start as usize, body.code.end as usize);
        validator.validate_body(&mut reader)?;
        rewrites.append(&mut validator.rewrites);
        infos.push(FunctionInfo {
            side_table: validator.side_table.into_boxed_slice(),
            try_regions: validator.try_regions.into_boxed_slice(),
            catch_clauses: validator.catch_clauses.into_boxed_slice(),
            local_count: validator.locals.len() as u32 + validator.exception_local_count,
            exception_local_count: validator.exception_local_count,
            max_stack_slots: validator.max_slot_height,
        });
    }
    module.function_infos = infos;
    crate::quickening::apply(&mut module.bytes, &rewrites);
    Ok(())
}
