/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The builder: one forward pass over a validated, quickened function body that produces typed SSA.
//!
//! Locals become SSA values with the algorithm of Braun et al. ("Simple and Efficient Construction of SSA Form"),
//! specialized for structured control flow:
//!
//! - The merge block of a `block` or `if` is created at its `end`, when every branch to it is known, so it is sealed
//!   from the start. Only loop headers are unsealed while their body is built: their back edges are the branches to
//!   the loop inside it, and the loop's `end` seals them.
//! - Reading a local first looks at a dense per-local cache of the last definition, tagged with its block. Older
//!   definitions move into one function-wide map from (block, local) to value when the cache entry is overwritten.
//!   A read that finds neither walks the predecessors, with an explicit stack, memoizing what it finds; reads in an
//!   unsealed loop header make incomplete phis.
//! - Phis that turn out trivial become `Forward` nodes. Cleanup removes the rest of them, to a fixed point.
//!
//! The operand stack holds values; values flowing into merges become explicit phis, as the block types say.
//! Unreachable code is skipped, only tracking nesting. Nodes fold as they are created (`fold`), and pure nodes are
//! value numbered in a table whose entries are undone when the construct that made them ends, so that every hit
//! dominates its use. Global values are numbered too, keyed by epochs that `global.set` and calls advance.

mod calls;
mod exceptions;
pub mod fold;
pub mod gc;
mod memories;
mod simd;
mod tables;

use crate::decode::{BlockType, Decoder, Instruction};
use crate::ir::{BlockId, Func, IntCondition, LoadKind, Loop, NONE, Node, Op, Type, Value};
use crate::ir::{FLAG_IMMUTABLE, FloatCondition};
use crate::table::Table;
use calls::CallKind;
use fold::Folded;
use libwasm_module::Module;
use libwasm_module::abi::{InstanceData, TRAP_NULL_REFERENCE};
use libwasm_module::opcode;
use libwasm_module::quickening;
use libwasm_module::types::{FuncType, ValType};
use std::mem::offset_of;

/// The IR type of a WebAssembly value type, if the JIT compiles values of it.
pub fn ir_type(value: ValType) -> Option<Type> {
    match value {
        ValType::I32 => Some(Type::I32),
        ValType::I64 => Some(Type::I64),
        ValType::F32 => Some(Type::F32),
        ValType::F64 => Some(Type::F64),
        ValType::V128 => Some(Type::V128),
        ValType::Ref(_) => Some(Type::Ref),
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ControlKind {
    Function,
    Block,
    Loop,
    If,
    Else,
    /// A `try_table`, the body of a legacy `try`, and a legacy `catch` or `catch_all` handler.
    TryTable,
    Try,
    Catch,
}

/// The parameter and result types of a block, as ranges of `Builder::types`.
type Signature = ((u32, u32), (u32, u32));

/// A construct whose `end` is still ahead.
#[derive(Debug, Clone, Copy)]
struct Control {
    kind: ControlKind,
    /// The types of its parameters and results, in `Builder::types`.
    params: (u32, u32),
    results: (u32, u32),
    /// The height of the operand stack below its parameters.
    height: u32,
    /// The loop's header, or for `if` the block that branches on the condition.
    block: BlockId,
    /// The `else` edge of an `if`: its slot in `Func::successors`, or NONE once taken care of.
    else_slot: u32,
    /// For `if`, where its parameters start in `Builder::edge_values`, for the second arm.
    if_params: u32,
    /// The branches to the label, as a list through `Builder::edges`.
    edges: u32,
    /// The block that branched to the label last, so that a `br_table` adds one edge per block.
    last_edge_block: BlockId,
    /// Whether the rest of the construct is unreachable.
    unreachable: bool,
    /// Where the construct's entries start in the value numbering log.
    numbering_mark: u32,
    /// For loops, their index in `Func::loops` and the start of their incomplete phis.
    loop_index: u32,
    incomplete_start: u32,
    /// For a handler (see `exceptions.rs`): the exceptions thrown in its region, as a list through `Builder::edges`
    /// of edges carrying the exception, and the handler around it in `Builder::controls`, or NONE.
    throws: u32,
    outer_handler: u32,
    /// For `try_table` and legacy `try`: the start and count of its clauses in `Builder::clauses`.
    clauses: (u32, u32),
    /// For a legacy handler: the exception it caught, or NONE if nothing throws to it, and its next clause.
    exception: Value,
    next_clause: u32,
}

impl Control {
    fn label_arity(&self) -> (u32, u32) {
        if self.kind == ControlKind::Loop {
            self.params
        } else {
            self.results
        }
    }
}

/// A branch to a label whose block may not exist yet: its source, the successor slot to fill, and the values it
/// carries, in `Builder::edge_values`.
#[derive(Debug, Clone, Copy)]
struct Edge {
    block: BlockId,
    slot: u32,
    values: u32,
    next: u32,
}

/// The key of a value numbering entry: a node's op, type and flags, then its arguments.
type NumberingKey = [u32; 4];

fn numbering_key(node: &Node) -> NumberingKey {
    [
        node.op as u32 | ((node.ty as u32) << 16) | (u32::from(node.flags) << 24),
        node.args[0],
        node.args[1],
        node.args[2],
    ]
}

/// A node of the explicit stack that `read_local_slow` walks predecessors with.
#[derive(Debug, Clone, Copy)]
struct ReadFrame {
    block: BlockId,
    /// The phi whose inputs are being read, or NONE.
    phi: Value,
    /// Where in `read_values` the values of the predecessors read so far start, for a merge whose phi is only made
    /// if they differ, or NONE.
    collected: u32,
    next_predecessor: u32,
}

/// The builder's state, kept between compiles so that its storage is reused.
#[derive(Default)]
pub struct Builder {
    local_types: Vec<Type>,
    /// The last definition of each local and its block.
    current: Vec<(BlockId, Value)>,
    definitions: Table<u64>,
    /// The locals each loop writes, itself or in loops in it: keyed by (loop, local), and as a list per loop of
    /// (local, next entry) from its entry in `loop_write_heads`.
    loop_writes: Table<u64>,
    loop_write_list: Vec<(u32, u32)>,
    loop_write_heads: Vec<u32>,
    sealed: Vec<bool>,
    /// (header, phi, local) of every incomplete phi of an unsealed loop header.
    incomplete: Vec<(BlockId, Value, u32)>,
    stack: Vec<Value>,
    controls: Vec<Control>,
    edges: Vec<Edge>,
    edge_values: Vec<Value>,
    types: Vec<Type>,
    /// Value numbers, and constants, which are never undone. A value of NONE is no entry.
    numbering: Table<NumberingKey>,
    numbering_log: Vec<(NumberingKey, Value)>,
    global_epochs: Vec<u32>,
    call_epoch: u32,
    read_stack: Vec<ReadFrame>,
    read_values: Vec<Value>,
    scratch_values: Vec<Value>,
    scratch_blocks: Vec<BlockId>,
    params: Vec<Value>,
    current_block: BlockId,
    instance: Value,
    memory_base: Value,
    /// The zero vector, which every v128 local starts as, once made.
    vector_zero: Value,
    /// The next of the function's `try_regions` a legacy `try` may be (see `begin_try`).
    try_region_cursor: usize,
    /// The byte offsets of struct fields, by (type index, field index); the field count gives the size.
    field_offsets: Table<u64>,
    /// What an OSR compile needs (see `build_osr`), or what probing for one found.
    osr: Option<Osr>,
    /// The innermost handler whose region the code is in, as an index into `controls`, or NONE.
    handler: u32,
    clauses: Vec<exceptions::Clause>,
    /// The function, among the module's defined ones.
    defined: u32,
}

/// An OSR compile: the function as a continuation of an interpreter frame at a loop.
///
/// Its only parameter is the frame's fp. Its entry block branches, on a constant, to a block that loads the frame's
/// locals and operand stack and jumps to the header of the outermost loop around the frame's position, and to the
/// function's code as it starts, which never runs. That keeps the entry block dominating everything. Every loop
/// entered this way has a second way in, from the frame, besides its entry and back edges, and every value on the
/// operand stack at its header becomes a phi of them.
///
/// A loop in other loops is entered through them, so that each loop keeps a single header: each loop around it starts
/// with a branch on a flag (a local of its own, which only the way in from the frame sets), which goes from the header
/// to the next loop in, through a block that loads the operand stack at that loop from the frame.
#[derive(Default)]
struct Osr {
    /// The module offset of the loop instruction.
    pc: u32,
    /// The types of the operand stack at the loop, from bottom to top, parameters included.
    stack_types: Vec<Type>,
    /// The module offsets of the loops around it, outermost first, and how many of them have been entered.
    around: Vec<u32>,
    entered: usize,
    /// While probing: whether the loop was found and reachable, and the loops open where the probe is.
    found: bool,
    probing: bool,
    open_loops: Vec<u32>,
    /// The frame's fp, and the local that flags the way in from the frame (see above).
    frame: Value,
    flag: u32,
    /// The block that jumps to the next loop to enter from the frame, the slot of that jump, and the values the
    /// operand stack has at that loop's header, from the frame.
    block: BlockId,
    slot: u32,
    stack: Vec<Value>,
    /// The loops entered from the frame.
    entries: Vec<OsrEntry>,
}

/// A loop an OSR compile enters from the frame: its index, the indices of its entry edge and its edge from the frame,
/// and the phis of the values below its parameters (phi, value from the entry, value from the frame).
struct OsrEntry {
    loop_index: u32,
    entry_edge: u32,
    edge: u32,
    below: Vec<(Value, Value, Value)>,
}

/// Where an OSR compile enters a function: the types of the operand stack at the loop, and the loops around it.
pub type OsrEntryPoint = (Vec<Type>, Vec<u32>);

/// The fp offset of the local slot `local` of an interpreter frame.
fn frame_local_offset(local: u32) -> i32 {
    8 * local as i32
}

fn key(block: BlockId, local: u32) -> u64 {
    (u64::from(block) << 32) | u64::from(local)
}

fn instance_offset(offset: usize) -> u32 {
    offset as u32
}

impl Builder {
    fn clear(&mut self) {
        self.local_types.clear();
        self.current.clear();
        self.definitions.clear();
        self.loop_writes.clear();
        self.loop_write_list.clear();
        self.loop_write_heads.clear();
        self.sealed.clear();
        self.incomplete.clear();
        self.stack.clear();
        self.controls.clear();
        self.edges.clear();
        self.edge_values.clear();
        self.types.clear();
        self.numbering.clear();
        self.numbering_log.clear();
        self.global_epochs.clear();
        self.call_epoch = 0;
        self.read_stack.clear();
        self.read_values.clear();
        self.params.clear();
        self.vector_zero = NONE;
        self.try_region_cursor = 0;
        self.field_offsets.clear();
        self.handler = NONE;
        self.clauses.clear();
    }

    /// Builds the IR of the defined function `index` (counting imports) of `module` into `func`.
    pub fn build(&mut self, module: &Module, index: u32, func: &mut Func) {
        self.osr = None;
        self.build_function(module, index, func);
    }

    /// The types of the operand stack at the loop at module offset `pc`, and the loops around it, if an OSR compile
    /// can enter it: if it is reachable and in no legacy handler. Builds the function into `func` to find out.
    pub fn probe_osr(&mut self, module: &Module, index: u32, pc: u32, func: &mut Func) -> Option<OsrEntryPoint> {
        self.osr = Some(Osr {
            pc,
            probing: true,
            ..Osr::default()
        });
        self.build_function(module, index, func);
        let osr = self.osr.take().expect("the probe is there");
        osr.found.then_some((osr.stack_types, osr.around))
    }

    /// Builds the OSR compile of the function at the loop at `pc`, whose operand stack has `stack_types` there (see
    /// `probe_osr`).
    pub fn build_osr(
        &mut self,
        module: &Module,
        index: u32,
        pc: u32,
        (stack_types, around): OsrEntryPoint,
        func: &mut Func,
    ) {
        self.osr = Some(Osr {
            pc,
            stack_types,
            around,
            ..Osr::default()
        });
        self.build_function(module, index, func);
        self.osr = None;
    }

    fn is_osr_compile(&self) -> bool {
        self.osr.as_ref().is_some_and(|osr| !osr.probing)
    }

    fn build_function(&mut self, module: &Module, index: u32, func: &mut Func) {
        self.clear();
        func.clear();
        let defined = (index - module.imported_function_count) as usize;
        self.defined = defined as u32;
        let body = &module.code[defined];
        let ty = module.function_type(index).expect("functions have function types");
        let start = body.code.start as usize;
        for &param in &ty.params {
            let param = ir_type(param).expect("the JIT has a type for every value");
            func.params.push(param);
            self.local_types.push(param);
        }
        for &result in &ty.results {
            let result = ir_type(result).expect("the JIT has a type for every value");
            func.results.push(result);
        }
        for &(count, local) in &body.locals {
            let local = ir_type(local).expect("the JIT has a type for every value");
            self.local_types.extend(std::iter::repeat_n(local, count as usize));
        }
        self.current.resize(self.local_types.len(), (NONE, NONE));
        self.global_epochs.resize(module.global_space.len(), 0);

        let mut entry = self.new_block(func);
        self.start_block(func, entry);
        self.sealed[entry as usize] = true;
        self.instance = self.entry_value(func, Op::Instance, Type::Pointer, 0);
        self.memory_base = self.entry_value(func, Op::MemoryBase, Type::Pointer, 0);
        if self.is_osr_compile() {
            entry = self.enter_osr(func);
        } else {
            for (param_index, &param) in func.params.clone().iter().enumerate() {
                let value = self.entry_value(func, Op::Param, param, param_index as u32);
                self.params.push(value);
            }
        }
        let results = self.push_types(&ty.results);
        self.controls.push(Control {
            kind: ControlKind::Function,
            params: (0, 0),
            results,
            height: 0,
            block: entry,
            else_slot: NONE,
            if_params: 0,
            edges: NONE,
            last_edge_block: NONE,
            unreachable: false,
            numbering_mark: 0,
            loop_index: NONE,
            incomplete_start: 0,
            throws: NONE,
            outer_handler: NONE,
            clauses: (0, 0),
            exception: NONE,
            next_clause: 0,
        });

        let mut decoder = Decoder::new(&module.bytes, start, body.code.end as usize);
        while !self.controls.is_empty() {
            let pc = decoder.position();
            let instruction = decoder.decode();
            if self.controls.last().expect("there is a construct").unreachable {
                self.skip(func, module, &instruction, &mut decoder, pc);
                continue;
            }
            self.instruction(func, module, instruction, &decoder, pc);
        }
    }

    // Blocks.

    /// Makes the entry of an OSR compile (see `Osr`) and returns the block the function's code starts in.
    /// Loads the bottom values of the operand stack of the interpreter frame at `frame`, of types `types`.
    fn load_frame_stack(func: &mut Func, frame: Value, types: &[Type]) -> Vec<Value> {
        let mut offset = -(libwasm_module::abi::FRAME_HEADER_SIZE as i32);
        let mut stack = Vec::with_capacity(types.len());
        for &ty in types {
            // The operand stack grows down from the frame header; a value's lowest slot is its first.
            offset -= 8 * ty.slots() as i32;
            stack.push(Self::append(
                func,
                Node::new(Op::FrameLoad, ty, 0, [frame, offset as u32, 0]),
            ));
        }
        stack
    }

    fn enter_osr(&mut self, func: &mut Func) -> BlockId {
        let param_types = std::mem::take(&mut func.params);
        func.params.push(Type::Pointer);
        let frame = self.entry_value(func, Op::Param, Type::Pointer, 0);
        // The function's own code never runs, but it must build as it would, so its parameters are not constants
        // there.
        for (local, &ty) in param_types.iter().enumerate() {
            let indirect = u8::from(ty == Type::V128);
            let value = Self::append(
                func,
                Node::new(
                    Op::FrameLoad,
                    ty,
                    indirect,
                    [frame, frame_local_offset(local as u32) as u32, 0],
                ),
            );
            self.params.push(value);
        }
        let from_frame = self.new_block(func);
        let start = self.new_block(func);
        let always = self.constant(func, Type::I32, 1);
        let entry = self.current_block;
        self.terminate(
            func,
            Node::new(Op::Branch, Type::None, 0, [always, 0, 0]),
            &[from_frame, start],
        );
        Self::set_predecessors(func, from_frame, &[entry]);
        Self::set_predecessors(func, start, &[entry]);
        self.start_block(func, from_frame);
        for local in 0..self.local_types.len() as u32 {
            let ty = self.local_types[local as usize];
            let indirect = u8::from(ty == Type::V128);
            let value = Self::append(
                func,
                Node::new(
                    Op::FrameLoad,
                    ty,
                    indirect,
                    [frame, frame_local_offset(local) as u32, 0],
                ),
            );
            self.write_local(local, value);
        }
        let flag = self.local_types.len() as u32;
        self.local_types.push(Type::I32);
        self.current.push((NONE, NONE));
        let one = self.constant(func, Type::I32, 1);
        self.write_local(flag, one);
        let stack_types = std::mem::take(&mut self.osr.as_mut().expect("this is an OSR compile").stack_types);
        let stack = Self::load_frame_stack(func, frame, &stack_types);
        let slot = self.jump(func, NONE);
        let osr = self.osr.as_mut().expect("this is an OSR compile");
        osr.frame = frame;
        osr.flag = flag;
        osr.block = from_frame;
        osr.slot = slot;
        osr.stack = stack;
        osr.stack_types = stack_types;
        self.start_block(func, start);
        start
    }

    fn new_block(&mut self, func: &mut Func) -> BlockId {
        let block = func.blocks.len() as BlockId;
        let loop_index = self
            .controls
            .iter()
            .rev()
            .find(|control| control.kind == ControlKind::Loop)
            .map_or(NONE, |control| control.loop_index);
        func.blocks.push(crate::ir::Block {
            loop_index,
            ..Default::default()
        });
        self.sealed.push(true);
        block
    }

    fn start_block(&mut self, func: &mut Func, block: BlockId) {
        func.blocks[block as usize].start = func.nodes.len() as u32;
        self.current_block = block;
    }

    fn set_predecessors(func: &mut Func, block: BlockId, predecessors: &[BlockId]) {
        let data = &mut func.blocks[block as usize];
        data.predecessors_start = func.predecessors.len() as u32;
        data.predecessor_count = predecessors.len() as u32;
        func.predecessors.extend_from_slice(predecessors);
    }

    /// Ends the current block with `node`, whose successors are `successors` (NONE for those not known yet).
    /// Returns the slot of the first successor.
    fn terminate(&mut self, func: &mut Func, node: Node, successors: &[BlockId]) -> u32 {
        func.nodes.push(node);
        let slot = func.successors.len() as u32;
        let block = &mut func.blocks[self.current_block as usize];
        block.end = func.nodes.len() as u32;
        block.successors_start = slot;
        block.successor_count = successors.len() as u32;
        func.successors.extend_from_slice(successors);
        slot
    }

    // Nodes.

    fn append(func: &mut Func, node: Node) -> Value {
        func.nodes.push(node);
        func.nodes.len() as Value - 1
    }

    fn entry_value(&mut self, func: &mut Func, op: Op, ty: Type, argument: u32) -> Value {
        Self::append(func, Node::new(op, ty, 0, [argument, 0, 0]))
    }

    fn constant(&mut self, func: &mut Func, ty: Type, bits: u64) -> Value {
        if ty == Type::V128 {
            if self.vector_zero == NONE {
                self.vector_zero = self.vector_constant(func, u128::from(bits));
            }
            debug_assert_eq!(bits, 0);
            return self.vector_zero;
        }
        let node = if ty.is_64() {
            Node::new(Op::Const64, ty, 0, [bits as u32, (bits >> 32) as u32, 0])
        } else {
            Node::new(Op::Const32, ty, 0, [bits as u32, 0, 0])
        };
        let key = numbering_key(&node);
        if let Some(value) = self.numbering.get(key) {
            return value;
        }
        let value = Self::append(func, node);
        // NB: Constants have no position, so they are never undone.
        self.numbering.insert(key, value);
        value
    }

    /// A pure node, or the value of an equal one that dominates the current position.
    fn pure(&mut self, func: &mut Func, node: Node) -> Value {
        let key = numbering_key(&node);
        if let Some(value) = self.numbering.get(key)
            && value != NONE
        {
            return value;
        }
        let value = Self::append(func, node);
        self.number(key, value);
        value
    }

    fn number(&mut self, key: NumberingKey, value: Value) {
        let previous = self.numbering.insert(key, value).unwrap_or(NONE);
        self.numbering_log.push((key, previous));
    }

    /// Undoes the value numbering entries made since `mark`.
    fn unnumber(&mut self, mark: u32) {
        while self.numbering_log.len() > mark as usize {
            let (key, previous) = self.numbering_log.pop().expect("the log is longer than the mark");
            self.numbering.insert(key, previous);
        }
    }

    fn binary(&mut self, func: &mut Func, op: Op, ty: Type, a: Value, b: Value) -> Value {
        self.binary_with_flags(func, op, ty, 0, a, b)
    }

    fn binary_with_flags(&mut self, func: &mut Func, op: Op, ty: Type, flags: u8, a: Value, b: Value) -> Value {
        let (a, b) = (func.resolve(a), func.resolve(b));
        let result_type = if op == Op::Compare { Type::I32 } else { ty };
        match fold::binary(func, op, ty, flags, a, b) {
            Folded::Value(value) => value,
            Folded::Constant(bits) => self.constant(func, result_type, bits),
            Folded::Node { op, flags, a, b } => {
                let node = Node::new(op, result_type, flags, [a, b, 0]);
                if node.effect() == crate::ir::Effect::Pure {
                    self.pure(func, node)
                } else {
                    Self::append(func, node)
                }
            }
        }
    }

    fn unary(&mut self, func: &mut Func, op: Op, ty: Type, a: Value) -> Value {
        let a = func.resolve(a);
        match fold::unary(func, op, ty, a) {
            Some(Folded::Value(value)) => value,
            Some(Folded::Constant(bits)) => self.constant(func, ty, bits),
            Some(Folded::Node { op, flags, a, b }) => self.pure(func, Node::new(op, ty, flags, [a, b, 0])),
            None => {
                let node = Node::new(op, ty, 0, [a, 0, 0]);
                if node.effect() == crate::ir::Effect::Pure {
                    self.pure(func, node)
                } else {
                    Self::append(func, node)
                }
            }
        }
    }

    // Locals (see the module documentation).

    fn write_local(&mut self, local: u32, value: Value) {
        let (block, previous) = self.current[local as usize];
        if block != self.current_block && block != NONE {
            self.definitions.insert(key(block, local), previous);
        }
        self.current[local as usize] = (self.current_block, value);
    }

    fn note_loop_write(&mut self, loop_index: u32, local: u32) {
        if loop_index == NONE || self.loop_writes.insert(key(loop_index, local), 1).is_some() {
            return;
        }
        if self.loop_write_heads.len() <= loop_index as usize {
            self.loop_write_heads.resize(loop_index as usize + 1, NONE);
        }
        let next = self.loop_write_heads[loop_index as usize];
        self.loop_write_heads[loop_index as usize] = self.loop_write_list.len() as u32;
        self.loop_write_list.push((local, next));
    }

    /// Counts the locals a loop that has ended writes as written by the loop around it.
    fn propagate_loop_writes(&mut self, func: &Func, loop_index: u32) {
        let parent = func.loops[loop_index as usize].parent;
        let mut entry = self.loop_write_heads.get(loop_index as usize).copied().unwrap_or(NONE);
        while entry != NONE && parent != NONE {
            let (local, next) = self.loop_write_list[entry as usize];
            self.note_loop_write(parent, local);
            entry = next;
        }
    }

    /// Records `value` as the value of `local` at the end of `block`.
    fn memoize(&mut self, block: BlockId, local: u32, value: Value) {
        if block == self.current_block {
            self.write_local(local, value);
        } else {
            self.definitions.insert(key(block, local), value);
        }
    }

    fn lookup(&self, func: &Func, block: BlockId, local: u32) -> Option<Value> {
        let (cached_block, value) = self.current[local as usize];
        if cached_block == block {
            return Some(func.resolve(value));
        }
        self.definitions.get(key(block, local)).map(|value| func.resolve(value))
    }

    fn read_local(&mut self, func: &mut Func, block: BlockId, local: u32) -> Value {
        if let Some(value) = self.lookup(func, block, local) {
            return value;
        }
        self.read_local_slow(func, block, local)
    }

    fn new_phi(func: &mut Func, block: BlockId, ty: Type) -> Value {
        let first = func.blocks[block as usize].first_phi;
        let phi = Self::append(func, Node::new(Op::Phi, ty, 0, [0, 0, first]));
        func.blocks[block as usize].first_phi = phi;
        phi
    }

    /// The value of a local that a block is entered with: a parameter, or zero.
    fn initial_value(&mut self, func: &mut Func, local: u32) -> Value {
        if (local as usize) < self.params.len() {
            return self.params[local as usize];
        }
        let ty = self.local_types[local as usize];
        self.constant(func, ty, 0)
    }

    /// Whether a read of a local can be on a cycle through `block`: only if the block is in a loop whose header is
    /// sealed, so that reads go around the back edges.
    fn may_be_on_cycle(&self, func: &Func, block: BlockId) -> bool {
        let loop_index = func.blocks[block as usize].loop_index;
        loop_index != NONE && self.sealed[func.loops[loop_index as usize].header as usize]
    }

    fn read_local_slow(&mut self, func: &mut Func, block: BlockId, local: u32) -> Value {
        let ty = self.local_types[local as usize];
        let base = self.read_stack.len();
        self.read_stack.push(ReadFrame {
            block,
            phi: NONE,
            collected: NONE,
            next_predecessor: 0,
        });
        // The value the frame on top of the stack delivers, if it is done.
        let mut delivered: Option<Value> = None;
        loop {
            let top = self.read_stack.len() - 1;
            if let Some(value) = delivered.take() {
                let frame = &mut self.read_stack[top];
                if frame.collected != NONE {
                    self.read_values.push(value);
                    frame.next_predecessor += 1;
                } else if frame.phi != NONE {
                    let phi = func.nodes[frame.phi as usize];
                    func.operands[(phi.args[0] + frame.next_predecessor) as usize] = value;
                    frame.next_predecessor += 1;
                } else {
                    let frame_block = frame.block;
                    self.read_stack.pop();
                    self.memoize(frame_block, local, value);
                    if self.read_stack.len() == base {
                        return value;
                    }
                    delivered = Some(value);
                    continue;
                }
            }
            let frame = self.read_stack[top];
            if frame.phi != NONE || frame.collected != NONE {
                let predecessors = func.predecessors(frame.block);
                if (frame.next_predecessor as usize) < predecessors.len() {
                    let predecessor = predecessors[frame.next_predecessor as usize];
                    if let Some(value) = self.lookup(func, predecessor, local) {
                        delivered = Some(value);
                    } else {
                        self.read_stack.push(ReadFrame {
                            block: predecessor,
                            phi: NONE,
                            collected: NONE,
                            next_predecessor: 0,
                        });
                    }
                    continue;
                }
                self.read_stack.pop();
                let value = if frame.phi != NONE {
                    let value = Self::try_remove_trivial_phi(func, frame.phi);
                    if value != frame.phi {
                        self.memoize(frame.block, local, value);
                    }
                    value
                } else {
                    let value = self.merge_collected(func, frame.block, ty, frame.collected);
                    self.memoize(frame.block, local, value);
                    value
                };
                if self.read_stack.len() == base {
                    return value;
                }
                delivered = Some(value);
                continue;
            }
            // A frame visited for the first time.
            if let Some(value) = self.lookup(func, frame.block, local) {
                delivered = Some(value);
                continue;
            }
            if !self.sealed[frame.block as usize] {
                let phi = Self::new_phi(func, frame.block, ty);
                self.incomplete.push((frame.block, phi, local));
                delivered = Some(phi);
                continue;
            }
            let loop_index = func.blocks[frame.block as usize].loop_index;
            let entered_from_frame = self
                .osr
                .as_ref()
                .is_some_and(|osr| osr.entries.iter().any(|entry| entry.loop_index == loop_index));
            if self.may_be_on_cycle(func, frame.block)
                && !entered_from_frame
                && self.loop_writes.get(key(loop_index, local)).is_none()
            {
                // OPTIMIZATION: Everywhere in a loop that does not write the local, it has the value it entered
                // the loop with. Reads after a loop would otherwise go around all of it.
                let header = func.loops[loop_index as usize].header;
                let entry = func.predecessors(header)[0];
                if let Some(value) = self.lookup(func, entry, local) {
                    delivered = Some(value);
                } else {
                    self.read_stack.push(ReadFrame {
                        block: entry,
                        phi: NONE,
                        collected: NONE,
                        next_predecessor: 0,
                    });
                }
                continue;
            }
            let predecessors = func.predecessors(frame.block);
            match predecessors.len() {
                0 => {
                    let value = self.initial_value(func, local);
                    delivered = Some(value);
                }
                1 => {
                    let predecessor = predecessors[0];
                    if let Some(value) = self.lookup(func, predecessor, local) {
                        delivered = Some(value);
                    } else {
                        self.read_stack.push(ReadFrame {
                            block: predecessor,
                            phi: NONE,
                            collected: NONE,
                            next_predecessor: 0,
                        });
                    }
                }
                // OPTIMIZATION: Where no read can come back around to the block, its phi is only made if the
                // predecessors' values differ. Most merges of large functions need no phi for most locals, and
                // making and forwarding them took most of the build time of such functions.
                _ if !self.may_be_on_cycle(func, frame.block) => {
                    self.read_stack[top].collected = self.read_values.len() as u32;
                }
                count => {
                    let phi = Self::new_phi(func, frame.block, ty);
                    let start = func.operands.len() as u32;
                    func.operands.extend(std::iter::repeat_n(phi, count));
                    func.nodes[phi as usize].args[0] = start;
                    func.nodes[phi as usize].args[1] = count as u32;
                    // NB: Memoized before its inputs are read, so that cycles through it end at it.
                    self.memoize(frame.block, local, phi);
                    self.read_stack[top].phi = phi;
                }
            }
        }
    }

    /// The value of a local at the start of `block`, given its predecessors' values in `read_values` from `start`
    /// on, which it removes: their value if they are all the same, else a new phi of them.
    fn merge_collected(&mut self, func: &mut Func, block: BlockId, ty: Type, start: u32) -> Value {
        let values = &self.read_values[start as usize..];
        let first = func.resolve(values[0]);
        let value = if values[1..].iter().all(|value| func.resolve(*value) == first) {
            first
        } else {
            let phi = Self::new_phi(func, block, ty);
            let operands = func.operands.len() as u32;
            func.operands.extend_from_slice(values);
            func.nodes[phi as usize].args[0] = operands;
            func.nodes[phi as usize].args[1] = values.len() as u32;
            phi
        };
        self.read_values.truncate(start as usize);
        value
    }

    /// Value `index` of each of the edges `collect_edges()` collected, merged at the start of `block`.
    fn merge_edge_values(&mut self, func: &mut Func, block: BlockId, ty: Type, index: usize) -> Value {
        let start = self.read_values.len() as u32;
        for position in 0..self.scratch_values.len() {
            let edge = self.scratch_values[position];
            let value = self.edge_values[self.edges[edge as usize].values as usize + index];
            self.read_values.push(value);
        }
        self.merge_collected(func, block, ty, start)
    }

    /// Forwards `phi` if its inputs are only itself and one other value, which it returns; returns `phi` otherwise.
    fn try_remove_trivial_phi(func: &mut Func, phi: Value) -> Value {
        // NB: A phi with no other input is only reachable through itself: the loop is never entered with a value for
        //     it, and the phi stays.
        let Some(same) = func.trivial_phi_input(phi) else {
            return phi;
        };
        func.forward(phi, same);
        same
    }

    // Types and labels.

    fn push_types(&mut self, types: &[ValType]) -> (u32, u32) {
        let start = self.types.len() as u32;
        for &ty in types {
            let ty = ir_type(ty).expect("the JIT has a type for every value");
            self.types.push(ty);
        }
        (start, types.len() as u32)
    }

    fn block_signature(&mut self, module: &Module, block_type: BlockType) -> Signature {
        match block_type {
            BlockType::Empty => ((0, 0), (0, 0)),
            BlockType::Value(value) => ((0, 0), self.push_types(&[value])),
            BlockType::Function(index) => {
                let function: &FuncType = module.types[index as usize]
                    .as_func()
                    .expect("block types are functions");
                let params = self.push_types(&function.params);
                let results = self.push_types(&function.results);
                (params, results)
            }
        }
    }

    /// Ends the current block with a jump to `target`, or to a block the caller sets later if it is NONE, and returns
    /// the jump's successor slot.
    fn jump(&mut self, func: &mut Func, target: BlockId) -> u32 {
        self.terminate(func, Node::new(Op::Jump, Type::None, 0, [0; 3]), &[target])
    }

    /// Records a branch from the current block to the label of `controls[index]`, carrying the top values of the
    /// stack, through the successor `slot`.
    fn add_edge(&mut self, index: usize, slot: u32) {
        let control = self.controls[index];
        let (_, arity) = control.label_arity();
        let values = self.edge_values.len() as u32;
        let top = self.stack.len() - arity as usize;
        self.edge_values.extend_from_slice(&self.stack[top..]);
        self.edges.push(Edge {
            block: self.current_block,
            slot,
            values,
            next: control.edges,
        });
        let control = &mut self.controls[index];
        control.edges = self.edges.len() as u32 - 1;
        control.last_edge_block = self.current_block;
    }

    /// Collects the edges of a label in the order they were made, which is linear order, into `scratch_blocks`
    /// (their blocks) and returns the edges' indices in `scratch_values`.
    fn collect_edges(&mut self, head: u32) {
        self.scratch_blocks.clear();
        self.scratch_values.clear();
        let mut edge = head;
        while edge != NONE {
            self.scratch_values.push(edge);
            edge = self.edges[edge as usize].next;
        }
        self.scratch_values.reverse();
        for &edge in &self.scratch_values {
            self.scratch_blocks.push(self.edges[edge as usize].block);
        }
    }

    fn is_unreachable(&self) -> bool {
        self.controls.last().is_some_and(|control| control.unreachable)
    }

    fn set_unreachable(&mut self) {
        let control = self.controls.last_mut().expect("there is a construct");
        control.unreachable = true;
        self.stack.truncate(control.height as usize);
    }

    // Instructions.

    fn pop(&mut self) -> Value {
        self.stack.pop().expect("validated code does not underflow the stack")
    }

    fn push(&mut self, value: Value) {
        self.stack.push(value);
    }

    fn skip(
        &mut self,
        func: &mut Func,
        module: &Module,
        instruction: &Instruction,
        decoder: &mut Decoder<'_>,
        pc: usize,
    ) {
        match instruction {
            Instruction::Else
            | Instruction::End
            | Instruction::Catch(_)
            | Instruction::CatchAll
            | Instruction::Delegate(_) => self.instruction(func, module, *instruction, decoder, pc),
            _ if crate::decode::nesting_change(instruction) > 0 => {
                // Skips the nested construct, which is unreachable as a whole.
                let mut depth = 1;
                while depth > 0 {
                    depth += crate::decode::nesting_change(&decoder.decode());
                }
            }
            _ => {}
        }
    }

    fn instruction(
        &mut self,
        func: &mut Func,
        module: &Module,
        instruction: Instruction,
        decoder: &Decoder<'_>,
        pc: usize,
    ) {
        use Instruction as I;
        match instruction {
            I::Nop => {}
            I::Unreachable => {
                self.terminate(
                    func,
                    Node::new(Op::Trap, Type::None, 0, [libwasm_module::abi::TRAP_UNREACHABLE, 0, 0]),
                    &[],
                );
                self.set_unreachable();
            }
            I::Block(block_type) => {
                let (params, results) = self.block_signature(module, block_type);
                self.push_control(ControlKind::Block, params, results, NONE);
            }
            I::Loop(block_type) => {
                let (params, results) = self.block_signature(module, block_type);
                let at_osr_loop = self.osr.as_ref().is_some_and(|osr| osr.pc == pc as u32);
                if self.osr.as_ref().is_some_and(|osr| osr.probing) {
                    // NB: A legacy handler's exception is no value the frame has, so loops in handlers do not OSR.
                    let in_handler = self.controls.iter().any(|control| control.kind == ControlKind::Catch);
                    // NB: A value below the operand stack of an enclosing construct that is no loop flows to the
                    //     construct's end around the loops too, where it would need a phi of the value the frame has
                    //     and the one the function's start made, which the merge does not make for values below its
                    //     height. Such loops do not OSR.
                    let below_construct = self.controls.iter().any(|control| {
                        !matches!(control.kind, ControlKind::Function | ControlKind::Loop) && control.height != 0
                    });
                    let stack_types = self.stack.iter().map(|value| func.node(*value).ty).collect();
                    let osr = self.osr.as_mut().expect("probing");
                    if at_osr_loop {
                        osr.found = !in_handler && !below_construct;
                        osr.stack_types = stack_types;
                        osr.around = osr.open_loops.clone();
                    }
                    osr.open_loops.push(pc as u32);
                    self.begin_loop(func, params, results, None);
                } else if let Some(osr) = &self.osr
                    && osr
                        .around
                        .get(osr.entered)
                        .map_or(at_osr_loop, |around| *around == pc as u32)
                {
                    self.enter_osr_loop(func, params, results, at_osr_loop);
                } else {
                    self.begin_loop(func, params, results, None);
                }
            }
            I::If(block_type) => {
                let (params, results) = self.block_signature(module, block_type);
                self.begin_if(func, params, results);
            }
            I::Else => self.begin_else(func),
            I::End => self.end(func, module),
            I::Br(depth) => {
                self.branch(func, depth);
                self.set_unreachable();
            }
            I::BrIf(depth) => {
                let condition = func.resolve(self.pop());
                self.conditional_branch(func, depth, condition);
            }
            I::BrTable { .. } => {
                let index = func.resolve(self.pop());
                self.branch_table(func, index, decoder.targets());
                self.set_unreachable();
            }
            I::Return => {
                let depth = self.controls.len() as u32 - 1;
                self.branch(func, depth);
                self.set_unreachable();
            }
            I::Call(function) => self.call(func, module, function, CallKind::Normal),
            I::Drop => {
                self.pop();
            }
            I::Select | I::SelectTyped(_) => {
                let condition = func.resolve(self.pop());
                let if_false = func.resolve(self.pop());
                let if_true = func.resolve(self.pop());
                let value = match func.node(condition).constant() {
                    Some(0) => if_false,
                    Some(_) => if_true,
                    None if if_true == if_false => if_true,
                    None => {
                        let ty = func.node(if_true).ty;
                        self.pure(func, Node::new(Op::Select, ty, 0, [condition, if_true, if_false]))
                    }
                };
                self.push(value);
            }
            I::LocalGet(local) => {
                let block = self.current_block;
                let value = self.read_local(func, block, local);
                self.push(value);
            }
            I::LocalSet(local) => {
                let value = func.resolve(self.pop());
                self.write_local(local, value);
                self.note_loop_write(func.blocks[self.current_block as usize].loop_index, local);
            }
            I::LocalTee(local) => {
                let value = func.resolve(*self.stack.last().expect("validated code has an operand"));
                self.write_local(local, value);
                self.note_loop_write(func.blocks[self.current_block as usize].loop_index, local);
            }
            I::GlobalGet(global) => self.global_get(func, module, global),
            I::GlobalSet(global) => self.global_set(func, global),
            I::Access { opcode, argument } => {
                debug_assert_eq!(argument.memory, 0, "plain accesses are of memory 0");
                let offset = argument.offset as u32;
                if opcode <= opcode::I64_LOAD32_U {
                    let (kind, ty) = load_kind(opcode);
                    let index = func.resolve(self.pop());
                    let value = Self::append(
                        func,
                        Node::new(Op::Load, ty, kind as u8, [self.memory_base, index, offset]),
                    );
                    self.push(value);
                } else {
                    let value = func.resolve(self.pop());
                    let index = func.resolve(self.pop());
                    let size_log2 = store_size_log2(opcode);
                    let start = func.operands.len() as u32;
                    func.operands.extend_from_slice(&[self.memory_base, index, value]);
                    Self::append(func, Node::new(Op::Store, Type::None, size_log2, [start, 3, offset]));
                }
            }
            I::I32Const(value) => {
                let value = self.constant(func, Type::I32, u64::from(value as u32));
                self.push(value);
            }
            I::I64Const(value) => {
                let value = self.constant(func, Type::I64, value as u64);
                self.push(value);
            }
            I::F32Const(bits) => {
                let value = self.constant(func, Type::F32, u64::from(bits));
                self.push(value);
            }
            I::F64Const(bits) => {
                let value = self.constant(func, Type::F64, bits);
                self.push(value);
            }
            I::Numeric(opcode) => self.numeric(func, opcode),
            I::Misc(sub_opcode) if sub_opcode <= opcode::misc::I64_TRUNC_SAT_F64_U => {
                let (op, ty) = match sub_opcode {
                    opcode::misc::I32_TRUNC_SAT_F32_S | opcode::misc::I32_TRUNC_SAT_F64_S => (Op::TruncSatS, Type::I32),
                    opcode::misc::I32_TRUNC_SAT_F32_U | opcode::misc::I32_TRUNC_SAT_F64_U => (Op::TruncSatU, Type::I32),
                    opcode::misc::I64_TRUNC_SAT_F32_S | opcode::misc::I64_TRUNC_SAT_F64_S => (Op::TruncSatS, Type::I64),
                    _ => (Op::TruncSatU, Type::I64),
                };
                let a = func.resolve(self.pop());
                let value = self.pure(func, Node::new(op, ty, 0, [a, 0, 0]));
                self.push(value);
            }
            I::CheckedAccess { opcode, argument } => self.checked_access(func, module, opcode, argument),
            I::MemorySize(memory) => self.memory_size(func, module, memory),
            I::MemoryGrow(memory) => self.memory_grow(func, module, memory),
            I::CallIndirect { type_index, table } => {
                self.call_indirect(func, module, type_index, table, CallKind::Normal);
            }
            I::CallRef(type_index) => self.call_ref(func, module, type_index, CallKind::Normal),
            I::ReturnCall(function) => self.call(func, module, function, CallKind::Tail),
            I::ReturnCallIndirect { type_index, table } => {
                self.call_indirect(func, module, type_index, table, CallKind::Tail);
            }
            I::ReturnCallRef(type_index) => self.call_ref(func, module, type_index, CallKind::Tail),
            I::TryTable(block_type) => {
                let (params, results) = self.block_signature(module, block_type);
                let clauses: Vec<[u32; 3]> = decoder.catch_clauses().collect();
                self.begin_try_table(params, results, &clauses);
            }
            I::Try(block_type) => {
                let (params, results) = self.block_signature(module, block_type);
                self.begin_try(module, params, results, decoder.position());
            }
            I::Catch(_) | I::CatchAll => self.begin_catch(func, module),
            I::Delegate(label) => self.delegate(func, label),
            I::Throw(tag) => self.throw_tag(func, module, tag),
            I::ThrowRef => {
                let exception = func.resolve(self.pop());
                Self::append(
                    func,
                    Node::new(Op::CheckNull, Type::None, TRAP_NULL_REFERENCE as u8, [exception, 0, 0]),
                );
                self.throw_exception(func, exception);
            }
            I::Rethrow(label) => self.rethrow(func, label),
            I::TableGet(table) => self.table_get(func, table),
            I::TableSet(table) => self.table_set(func, table),
            I::RefNull(_) => {
                let value = self.constant(func, Type::Ref, 0);
                self.push(value);
            }
            I::RefIsNull => {
                let a = self.pop();
                let value = self.unary(func, Op::Eqz, Type::I32, a);
                self.push(value);
            }
            I::RefEq => {
                let b = self.pop();
                let a = self.pop();
                let value = self.binary_with_flags(func, Op::Compare, Type::Ref, IntCondition::Eq as u8, a, b);
                self.push(value);
            }
            I::RefFunc(function) => {
                let value = self.function_reference(func, function);
                self.push(value);
            }
            I::RefAsNonNull => {
                let reference = func.resolve(self.pop());
                Self::append(
                    func,
                    Node::new(Op::CheckNull, Type::None, TRAP_NULL_REFERENCE as u8, [reference, 0, 0]),
                );
                self.push(reference);
            }
            I::BrOnNull(depth) => {
                let reference = func.resolve(self.pop());
                let is_null = self.unary(func, Op::Eqz, Type::I32, reference);
                self.conditional_branch(func, depth, is_null);
                if !self.is_unreachable() {
                    self.push(reference);
                }
            }
            I::BrOnNonNull(depth) => {
                let reference = func.resolve(*self.stack.last().expect("br_on_non_null has an operand"));
                let null = self.constant(func, Type::Ref, 0);
                let is_not_null =
                    self.binary_with_flags(func, Op::Compare, Type::Ref, IntCondition::Ne as u8, reference, null);
                self.conditional_branch(func, depth, is_not_null);
                if !self.is_unreachable() {
                    self.pop();
                }
            }
            I::Misc(sub_opcode) => self.bulk(func, module, sub_opcode, decoder.misc_indices()),
            I::Gc(sub_opcode) => self.gc(
                func,
                module,
                sub_opcode,
                decoder.gc_indices(),
                decoder.gc_heap_types(),
                decoder.gc_flags(),
            ),
            I::Vector { opcode, index } => {
                let plain = match opcode {
                    quickening::LOCAL_GET_V128 => I::LocalGet(index),
                    quickening::LOCAL_SET_V128 => I::LocalSet(index),
                    quickening::LOCAL_TEE_V128 => I::LocalTee(index),
                    quickening::GLOBAL_GET_V128 => I::GlobalGet(index),
                    quickening::GLOBAL_SET_V128 => I::GlobalSet(index),
                    quickening::DROP_V128 => I::Drop,
                    _ => I::Select,
                };
                self.instruction(func, module, plain, decoder, pc);
            }
            I::Simd { sub_opcode, checked } => self.simd(func, module, sub_opcode, checked, decoder),
            I::Atomic(sub_opcode) => self.atomic(func, module, sub_opcode, decoder),
        }
    }

    fn numeric(&mut self, func: &mut Func, opcode: u8) {
        use Op::*;
        use Type::{F32, F64, I32, I64};
        let (op, ty, flags, arity) = match opcode {
            opcode::I32_EQZ => (Eqz, I32, 0, 1),
            opcode::I64_EQZ => (Eqz, I32, 0, 1),
            0x46..=0x4f => (Compare, I32, opcode - 0x46, 2),
            0x51..=0x5a => (Compare, I64, opcode - 0x51, 2),
            0x5b..=0x60 => (FloatCompare, F32, opcode - 0x5b, 2),
            0x61..=0x66 => (FloatCompare, F64, opcode - 0x61, 2),
            opcode::I32_CLZ => (Clz, I32, 0, 1),
            opcode::I32_CTZ => (Ctz, I32, 0, 1),
            opcode::I32_POPCNT => (Popcnt, I32, 0, 1),
            0x6a..=0x78 => (INTEGER_BINARY[(opcode - 0x6a) as usize], I32, 0, 2),
            opcode::I64_CLZ => (Clz, I64, 0, 1),
            opcode::I64_CTZ => (Ctz, I64, 0, 1),
            opcode::I64_POPCNT => (Popcnt, I64, 0, 1),
            0x7c..=0x8a => (INTEGER_BINARY[(opcode - 0x7c) as usize], I64, 0, 2),
            0x8b..=0x91 => (FLOAT_UNARY[(opcode - 0x8b) as usize], F32, 0, 1),
            0x92..=0x98 => (FLOAT_BINARY[(opcode - 0x92) as usize], F32, 0, 2),
            0x99..=0x9f => (FLOAT_UNARY[(opcode - 0x99) as usize], F64, 0, 1),
            0xa0..=0xa6 => (FLOAT_BINARY[(opcode - 0xa0) as usize], F64, 0, 2),
            opcode::I32_WRAP_I64 => (Wrap, I32, 0, 1),
            opcode::I32_TRUNC_F32_S | opcode::I32_TRUNC_F64_S => (TruncS, I32, 0, 1),
            opcode::I32_TRUNC_F32_U | opcode::I32_TRUNC_F64_U => (TruncU, I32, 0, 1),
            opcode::I64_EXTEND_I32_S => (ExtendI32S, I64, 0, 1),
            opcode::I64_EXTEND_I32_U => (ExtendI32U, I64, 0, 1),
            opcode::I64_TRUNC_F32_S | opcode::I64_TRUNC_F64_S => (TruncS, I64, 0, 1),
            opcode::I64_TRUNC_F32_U | opcode::I64_TRUNC_F64_U => (TruncU, I64, 0, 1),
            opcode::F32_CONVERT_I32_S | opcode::F32_CONVERT_I64_S => (ConvertS, F32, 0, 1),
            opcode::F32_CONVERT_I32_U | opcode::F32_CONVERT_I64_U => (ConvertU, F32, 0, 1),
            opcode::F32_DEMOTE_F64 => (Demote, F32, 0, 1),
            opcode::F64_CONVERT_I32_S | opcode::F64_CONVERT_I64_S => (ConvertS, F64, 0, 1),
            opcode::F64_CONVERT_I32_U | opcode::F64_CONVERT_I64_U => (ConvertU, F64, 0, 1),
            opcode::F64_PROMOTE_F32 => (Promote, F64, 0, 1),
            opcode::I32_REINTERPRET_F32 => (Reinterpret, I32, 0, 1),
            opcode::I64_REINTERPRET_F64 => (Reinterpret, I64, 0, 1),
            opcode::F32_REINTERPRET_I32 => (Reinterpret, F32, 0, 1),
            opcode::F64_REINTERPRET_I64 => (Reinterpret, F64, 0, 1),
            opcode::I32_EXTEND8_S => (Extend8S, I32, 0, 1),
            opcode::I32_EXTEND16_S => (Extend16S, I32, 0, 1),
            opcode::I64_EXTEND8_S => (Extend8S, I64, 0, 1),
            opcode::I64_EXTEND16_S => (Extend16S, I64, 0, 1),
            opcode::I64_EXTEND32_S => (Extend32S, I64, 0, 1),
            _ => unreachable!("only numeric opcodes are numeric instructions"),
        };
        let value = if arity == 2 {
            let b = self.pop();
            let a = self.pop();
            match op {
                Compare => self.binary_with_flags(func, op, ty, flags, a, b),
                FloatCompare => {
                    let (a, b) = (func.resolve(a), func.resolve(b));
                    self.pure(func, Node::new(op, I32, flags, [a, b, 0]))
                }
                _ if ty.is_float() => {
                    let (a, b) = (func.resolve(a), func.resolve(b));
                    self.pure(func, Node::new(op, ty, 0, [a, b, 0]))
                }
                _ => self.binary(func, op, ty, a, b),
            }
        } else {
            let a = self.pop();
            if ty.is_float() || matches!(op, TruncS | TruncU | Reinterpret) {
                let a = func.resolve(a);
                let node = Node::new(op, ty, 0, [a, 0, 0]);
                if node.effect() == crate::ir::Effect::Pure {
                    self.pure(func, node)
                } else {
                    Self::append(func, node)
                }
            } else {
                self.unary(func, op, ty, a)
            }
        };
        self.push(value);
    }

    // Control.

    fn push_control(&mut self, kind: ControlKind, params: (u32, u32), results: (u32, u32), block: BlockId) {
        let height = self.stack.len() as u32 - params.1;
        self.controls.push(Control {
            kind,
            params,
            results,
            height,
            block,
            else_slot: NONE,
            if_params: 0,
            edges: NONE,
            last_edge_block: NONE,
            unreachable: false,
            numbering_mark: self.numbering_log.len() as u32,
            loop_index: NONE,
            incomplete_start: self.incomplete.len() as u32,
            throws: NONE,
            outer_handler: NONE,
            clauses: (0, 0),
            exception: NONE,
            next_clause: 0,
        });
    }

    /// Begins a loop, entered from the current block, or from `entry`, a block and the slot of its jump, which
    /// is already made.
    fn begin_loop(&mut self, func: &mut Func, params: (u32, u32), results: (u32, u32), entry: Option<(BlockId, u32)>) {
        let header = self.new_block(func);
        // NB: The header is created before the loop's control, so it takes the enclosing loop; fixed up below.
        let entry = match entry {
            Some((block, slot)) => {
                func.successors[slot as usize] = header;
                block
            }
            None => {
                self.jump(func, header);
                self.current_block
            }
        };
        self.sealed[header as usize] = false;
        let loop_index = func.loops.len() as u32;
        let parent = func.blocks[header as usize].loop_index;
        let depth = if parent == NONE {
            0
        } else {
            func.loops[parent as usize].depth + 1
        };
        func.loops.push(Loop {
            header,
            last: header,
            parent,
            depth,
        });
        func.blocks[header as usize].loop_index = loop_index;
        self.push_control(ControlKind::Loop, params, results, header);
        let control = self.controls.last_mut().expect("the loop was just pushed");
        control.loop_index = loop_index;
        // The edge from the entry is the first, before every back edge.
        let values = self.edge_values.len() as u32;
        let height = control.height as usize;
        self.edge_values.extend_from_slice(&self.stack[height..]);
        self.edges.push(Edge {
            block: entry,
            slot: NONE,
            values,
            next: NONE,
        });
        let control = self.controls.last_mut().expect("the loop was just pushed");
        control.edges = self.edges.len() as u32 - 1;
        self.start_block(func, header);
        for index in 0..params.1 {
            let ty = self.types[(params.0 + index) as usize];
            let phi = Self::new_phi(func, header, ty);
            self.stack[height + index as usize] = phi;
        }
        // Calls and global.set in the body may change globals before the header runs again.
        self.call_epoch += 1;
    }

    /// Begins a loop an OSR compile enters from the frame (see `Osr`): the loop the frame is at if `target`, else a
    /// loop around it.
    fn enter_osr_loop(&mut self, func: &mut Func, params: (u32, u32), results: (u32, u32), target: bool) {
        let height = self.stack.len() - params.1 as usize;
        let mut osr = self.osr.take().expect("this is an OSR compile");
        let first = osr.entered == 0;
        let mut entry = None;
        if !first {
            // The way in from the frame comes from the branch of the loop around this one, through a block that loads
            // the operand stack this loop starts with. It goes before the loop's header, in linear order.
            let slot = self.jump(func, NONE);
            entry = Some((self.current_block, slot));
            let load = self.new_block(func);
            Self::set_predecessors(func, load, &[osr.block]);
            func.successors[osr.slot as usize] = load;
            self.start_block(func, load);
            let count = if target { osr.stack_types.len() } else { height };
            osr.stack = Self::load_frame_stack(func, osr.frame, &osr.stack_types[..count]);
            if !target {
                // The flag is up again for this loop's header, which branches on it.
                let one = self.constant(func, Type::I32, 1);
                self.write_local(osr.flag, one);
            }
            osr.block = load;
            osr.slot = self.jump(func, NONE);
        }
        // The values the loop's parameters have from the frame, which only the loop the frame is at needs.
        if !target {
            osr.stack.truncate(height);
            for index in 0..params.1 {
                let ty = self.types[(params.0 + index) as usize];
                let zero = self.constant(func, ty, 0);
                osr.stack.push(zero);
            }
        }
        self.begin_loop(func, params, results, entry);
        let index = self.controls.len() - 1;
        let control = self.controls[index];
        let header = control.block;
        func.successors[osr.slot as usize] = header;
        // The edge from the frame comes second, after the one from the function's start and before every back edge.
        let values = self.edge_values.len() as u32;
        self.edge_values.extend_from_slice(&osr.stack[height..]);
        let entry_edge = control.edges;
        self.edges.push(Edge {
            block: osr.block,
            slot: NONE,
            values,
            next: control.edges,
        });
        let edge = self.edges.len() as u32 - 1;
        self.controls[index].edges = edge;
        let mut below = Vec::with_capacity(height);
        for position in 0..height {
            let normal = func.resolve(self.stack[position]);
            let phi = Self::new_phi(func, header, func.node(normal).ty);
            below.push((phi, normal, osr.stack[position]));
            self.stack[position] = phi;
        }
        osr.entries.push(OsrEntry {
            loop_index: control.loop_index,
            entry_edge,
            edge,
            below,
        });
        osr.entered += 1;
        let flag = osr.flag;
        self.osr = Some(osr);
        // Nothing numbered before the loop dominates the code after it any more, since the way in from the frame
        // skips it. Every construct still open forgets what is numbered from here on when it ends, the loop included.
        self.unnumber(0);
        for control in &mut self.controls {
            control.numbering_mark = 0;
        }
        if target {
            return;
        }
        // A loop around the one the frame is at: from the frame, go on to the next loop in.
        let block = self.current_block;
        let set = self.read_local(func, block, flag);
        let slot = self.terminate(func, Node::new(Op::Branch, Type::None, 0, [set, 0, 0]), &[NONE, NONE]);
        let onward = self.new_block(func);
        Self::set_predecessors(func, onward, &[block]);
        func.successors[slot as usize] = onward;
        self.start_block(func, onward);
        // The flag is down from here on, so that no back edge or exit of a loop carries it.
        let zero = self.constant(func, Type::I32, 0);
        self.write_local(flag, zero);
        let onward_slot = self.jump(func, NONE);
        let body = self.new_block(func);
        Self::set_predecessors(func, body, &[block]);
        func.successors[slot as usize + 1] = body;
        self.start_block(func, body);
        self.write_local(flag, zero);
        self.note_loop_write(control.loop_index, flag);
        let osr = self.osr.as_mut().expect("this is an OSR compile");
        osr.block = onward;
        osr.slot = onward_slot;
    }

    fn begin_if(&mut self, func: &mut Func, params: (u32, u32), results: (u32, u32)) {
        let condition = func.resolve(self.pop());
        let then_block = self.new_block(func);
        let slot = self.terminate(
            func,
            Node::new(Op::Branch, Type::None, 0, [condition, 0, 0]),
            &[then_block, NONE],
        );
        let if_block = self.current_block;
        self.push_control(ControlKind::If, params, results, if_block);
        let if_params = self.edge_values.len() as u32;
        let height = self.controls.last().expect("the if was just pushed").height as usize;
        self.edge_values.extend_from_slice(&self.stack[height..]);
        let control = self.controls.last_mut().expect("the if was just pushed");
        control.else_slot = slot + 1;
        control.if_params = if_params;
        Self::set_predecessors(func, then_block, &[if_block]);
        self.start_block(func, then_block);
    }

    fn begin_else(&mut self, func: &mut Func) {
        let index = self.controls.len() - 1;
        let control = self.controls[index];
        if !control.unreachable {
            let slot = self.jump(func, NONE);
            self.add_edge(index, slot);
        }
        self.unnumber(control.numbering_mark);
        self.stack.truncate(control.height as usize);
        let else_block = self.new_block(func);
        func.successors[control.else_slot as usize] = else_block;
        Self::set_predecessors(func, else_block, &[control.block]);
        self.start_block(func, else_block);
        let params = control.if_params as usize;
        for index in 0..control.params.1 as usize {
            let value = self.edge_values[params + index];
            self.stack.push(value);
        }
        let control = &mut self.controls[index];
        control.kind = ControlKind::Else;
        control.else_slot = NONE;
        control.unreachable = false;
    }

    fn end(&mut self, func: &mut Func, module: &Module) {
        let index = self.controls.len() - 1;
        let control = self.controls[index];
        match control.kind {
            ControlKind::Function => {
                if !control.unreachable {
                    let depth = self.controls.len() as u32 - 1;
                    self.branch(func, depth);
                }
                self.controls.pop();
            }
            ControlKind::Loop => self.end_loop(func, index),
            ControlKind::Block | ControlKind::If | ControlKind::Else => self.end_block(func, index),
            ControlKind::TryTable => self.end_try_table(func, module, index),
            ControlKind::Try | ControlKind::Catch => self.end_try(func, index),
        }
    }

    fn end_block(&mut self, func: &mut Func, index: usize) {
        let control = self.controls[index];
        let arity = control.results.1 as usize;
        if control.kind == ControlKind::If {
            // Without an else arm, the condition's false edge carries the parameters to the end.
            let values = self.edge_values.len() as u32;
            for offset in 0..control.params.1 {
                let value = self.edge_values[(control.if_params + offset) as usize];
                self.edge_values.push(value);
            }
            self.edges.push(Edge {
                block: control.block,
                slot: control.else_slot,
                values,
                next: control.edges,
            });
            self.controls[index].edges = self.edges.len() as u32 - 1;
        }
        let control = self.controls[index];
        let reachable = !control.unreachable;
        self.unnumber(control.numbering_mark);
        if control.edges == NONE {
            // Only the fallthrough, if anything, reaches the end: the current block goes on.
            self.controls.pop();
            if !reachable {
                self.set_unreachable();
            }
            return;
        }
        if reachable {
            let slot = self.jump(func, NONE);
            self.add_edge(index, slot);
        }
        let control = self.controls[index];
        self.stack.truncate(control.height as usize);
        self.collect_edges(control.edges);
        let merge = self.new_block(func);
        let predecessors = std::mem::take(&mut self.scratch_blocks);
        Self::set_predecessors(func, merge, &predecessors);
        self.scratch_blocks = predecessors;
        for &edge in &self.scratch_values {
            func.successors[self.edges[edge as usize].slot as usize] = merge;
        }
        self.start_block(func, merge);
        for result in 0..arity {
            let ty = self.types[(control.results.0 + result as u32) as usize];
            let value = self.merge_edge_values(func, merge, ty, result);
            self.stack.push(value);
        }
        self.controls.pop();
    }

    fn end_loop(&mut self, func: &mut Func, index: usize) {
        let control = self.controls[index];
        if let Some(osr) = self.osr.as_mut()
            && osr.probing
        {
            osr.open_loops.pop();
        }
        let header = control.block;
        let reachable = !control.unreachable;
        // The back edges are the loop label's edges but the first, which enters the loop.
        self.collect_edges(control.edges);
        let predecessors = std::mem::take(&mut self.scratch_blocks);
        Self::set_predecessors(func, header, &predecessors);
        self.scratch_blocks = predecessors;
        self.sealed[header as usize] = true;
        self.propagate_loop_writes(func, control.loop_index);
        // The parameters' phis.
        let edge_list = std::mem::take(&mut self.scratch_values);
        let mut phi = func.blocks[header as usize].first_phi;
        let mut params = Vec::new();
        while phi != NONE {
            params.push(phi);
            phi = func.nodes[phi as usize].args[2];
        }
        // NB: Phis are linked newest first; the parameters' phis are the oldest ones, in reverse order.
        let param_phis = &params[params.len() - control.params.1 as usize..];
        for (offset, &phi) in param_phis.iter().rev().enumerate() {
            let start = func.operands.len() as u32;
            for &edge in &edge_list {
                let value = self.edge_values[self.edges[edge as usize].values as usize + offset];
                func.operands.push(func.resolve(value));
            }
            func.nodes[phi as usize].args[0] = start;
            func.nodes[phi as usize].args[1] = edge_list.len() as u32;
        }
        self.scratch_values = edge_list;
        // The locals' incomplete phis.
        let mut incomplete_index = control.incomplete_start as usize;
        while incomplete_index < self.incomplete.len() {
            let (block, phi, local) = self.incomplete[incomplete_index];
            if block != header {
                incomplete_index += 1;
                continue;
            }
            self.incomplete.swap_remove(incomplete_index);
            let count = func.predecessors(header).len();
            let start = func.operands.len() as u32;
            func.operands.extend(std::iter::repeat_n(phi, count));
            func.nodes[phi as usize].args[0] = start;
            func.nodes[phi as usize].args[1] = count as u32;
            for predecessor_index in 0..count {
                let predecessor = func.predecessors(header)[predecessor_index];
                let value = self.read_local(func, predecessor, local);
                func.operands[start as usize + predecessor_index] = value;
            }
            Self::try_remove_trivial_phi(func, phi);
        }
        if let Some(osr) = &self.osr
            && let Some(entry) = osr.entries.iter().find(|entry| entry.loop_index == control.loop_index)
        {
            for &(phi, normal, from_frame) in &entry.below {
                let start = func.operands.len() as u32;
                for &edge in &self.scratch_values {
                    let value = if edge == entry.entry_edge {
                        normal
                    } else if edge == entry.edge {
                        from_frame
                    } else {
                        phi
                    };
                    func.operands.push(value);
                }
                func.nodes[phi as usize].args[0] = start;
                func.nodes[phi as usize].args[1] = self.scratch_values.len() as u32;
            }
        }
        for &phi in param_phis {
            Self::try_remove_trivial_phi(func, phi);
        }
        self.unnumber(control.numbering_mark);
        let height = control.height as usize;
        let results = control.results;
        self.controls.pop();
        if reachable {
            // NB: The code after the loop goes in a block of its own, so that the loop's blocks end where it does.
            let after = self.new_block(func);
            let loop_index = control.loop_index as usize;
            func.loops[loop_index].last = func.blocks.len() as u32 - 2;
            self.jump(func, after);
            let from = self.current_block;
            Self::set_predecessors(func, after, &[from]);
            self.start_block(func, after);
            debug_assert_eq!(self.stack.len(), height + results.1 as usize);
        } else {
            func.loops[control.loop_index as usize].last = func.blocks.len() as u32 - 1;
            self.set_unreachable();
        }
    }

    /// A branch to the label at `depth`, which ends the current block. The caller marks the rest unreachable.
    fn branch(&mut self, func: &mut Func, depth: u32) {
        let index = self.controls.len() - 1 - depth as usize;
        let control = self.controls[index];
        match control.kind {
            ControlKind::Function => {
                let count = control.results.1 as usize;
                let start = func.operands.len() as u32;
                let top = self.stack.len() - count;
                for position in top..self.stack.len() {
                    let value = func.resolve(self.stack[position]);
                    func.operands.push(value);
                }
                self.terminate(
                    func,
                    Node::new(Op::Return, Type::None, 0, [start, count as u32, 0]),
                    &[],
                );
            }
            ControlKind::Loop => {
                let slot = self.jump(func, control.block);
                self.add_edge(index, slot);
            }
            _ => {
                let slot = self.jump(func, NONE);
                self.add_edge(index, slot);
            }
        }
    }

    /// A branch to label `depth` if `condition` is nonzero. If it is a nonzero constant, what follows is unreachable.
    fn conditional_branch(&mut self, func: &mut Func, depth: u32, condition: Value) {
        match func.node(condition).constant() {
            Some(0) => {}
            Some(_) => {
                self.branch(func, depth);
                self.set_unreachable();
            }
            None => self.branch_if(func, depth, condition),
        }
    }

    fn branch_if(&mut self, func: &mut Func, depth: u32, condition: Value) {
        let index = self.controls.len() - 1 - depth as usize;
        let control = self.controls[index];
        let fallthrough = self.new_block(func);
        if control.kind == ControlKind::Function {
            // A conditional return: the taken edge goes to a block that returns.
            let returning = fallthrough;
            let after = self.new_block(func);
            let from = self.current_block;
            self.terminate(
                func,
                Node::new(Op::Branch, Type::None, 0, [condition, 0, 0]),
                &[returning, after],
            );
            Self::set_predecessors(func, returning, &[from]);
            Self::set_predecessors(func, after, &[from]);
            self.start_block(func, returning);
            let depth = self.controls.len() as u32 - 1;
            self.branch(func, depth);
            self.start_block(func, after);
            return;
        }
        let target = if control.kind == ControlKind::Loop {
            control.block
        } else {
            NONE
        };
        let slot = self.terminate(
            func,
            Node::new(Op::Branch, Type::None, 0, [condition, 0, 0]),
            &[target, fallthrough],
        );
        self.add_edge(index, slot);
        let from = self.current_block;
        Self::set_predecessors(func, fallthrough, &[from]);
        self.start_block(func, fallthrough);
    }

    fn branch_table(&mut self, func: &mut Func, index: Value, depths: &[u32]) {
        if let Some(bits) = func.node(index).constant() {
            let position = (bits as usize).min(depths.len() - 1);
            self.branch(func, depths[position]);
            return;
        }
        // The distinct labels, each with the index of its successor.
        let mut labels: Vec<(u32, u32)> = Vec::with_capacity(depths.len().min(16));
        let start = func.operands.len() as u32;
        let mut has_return = false;
        for &depth in depths {
            let successor = match labels.iter().position(|(label, _)| *label == depth) {
                Some(position) => labels[position].1,
                None => {
                    let successor = labels.len() as u32;
                    labels.push((depth, successor));
                    has_return |= depth as usize == self.controls.len() - 1;
                    successor
                }
            };
            func.operands.push(successor);
        }
        let mut successors = Vec::with_capacity(labels.len());
        let mut returning = NONE;
        for &(depth, _) in &labels {
            let control = &self.controls[self.controls.len() - 1 - depth as usize];
            successors.push(match control.kind {
                ControlKind::Loop => control.block,
                ControlKind::Function => {
                    returning = func.blocks.len() as u32;
                    // A block that returns, made below.
                    returning
                }
                _ => NONE,
            });
        }
        if has_return {
            self.new_block(func);
        }
        let count = depths.len() as u32 - 1;
        let from = self.current_block;
        let slot = self.terminate(
            func,
            Node::new(Op::BrTable, Type::None, 0, [index, start, count]),
            &successors,
        );
        for (position, &(depth, _)) in labels.iter().enumerate() {
            let control_index = self.controls.len() - 1 - depth as usize;
            if self.controls[control_index].kind == ControlKind::Function {
                continue;
            }
            self.add_edge(control_index, slot + position as u32);
        }
        if has_return {
            Self::set_predecessors(func, returning, &[from]);
            self.start_block(func, returning);
            let depth = self.controls.len() as u32 - 1;
            self.branch(func, depth);
        }
    }

    // Globals and calls.

    /// The storage of a global: `globals[index]` of the instance.
    fn global_storage(&mut self, func: &mut Func, global: u32) -> Value {
        let globals = self.pure(
            func,
            Node::new(
                Op::InstanceField,
                Type::Pointer,
                0,
                [self.instance, instance_offset(offset_of!(InstanceData, globals)), 0],
            ),
        );
        self.pure(
            func,
            Node::new(Op::LoadPointer, Type::Pointer, 0, [globals, global * 8, 0]),
        )
    }

    fn global_get(&mut self, func: &mut Func, module: &Module, global: u32) {
        let ty = module.global_space[global as usize];
        let value_type = ir_type(ty.value).expect("the JIT has a type for every value");
        let storage = self.global_storage(func, global);
        let value = if ty.mutable {
            let key = numbering_key(&Node::new(
                Op::GlobalGet,
                value_type,
                0,
                [global, self.global_epochs[global as usize], self.call_epoch],
            ));
            if let Some(value) = self.numbering.get(key)
                && value != NONE
            {
                value
            } else {
                let value = Self::append(func, Node::new(Op::GlobalGet, value_type, 0, [storage, global, 0]));
                self.number(key, value);
                value
            }
        } else {
            self.pure(
                func,
                Node::new(Op::GlobalGet, value_type, FLAG_IMMUTABLE, [storage, global, 0]),
            )
        };
        self.push(value);
    }

    fn global_set(&mut self, func: &mut Func, global: u32) {
        let value = func.resolve(self.pop());
        let storage = self.global_storage(func, global);
        let ty = func.node(value).ty;
        Self::append(func, Node::new(Op::GlobalSet, Type::None, 0, [storage, value, global]));
        self.global_epochs[global as usize] += 1;
        // The global holds the stored value until it changes again.
        let key = numbering_key(&Node::new(
            Op::GlobalGet,
            ty,
            0,
            [global, self.global_epochs[global as usize], self.call_epoch],
        ));
        self.number(key, value);
    }
}

const INTEGER_BINARY: [Op; 15] = [
    Op::Add,
    Op::Sub,
    Op::Mul,
    Op::DivS,
    Op::DivU,
    Op::RemS,
    Op::RemU,
    Op::And,
    Op::Or,
    Op::Xor,
    Op::Shl,
    Op::ShrS,
    Op::ShrU,
    Op::Rotl,
    Op::Rotr,
];

const FLOAT_UNARY: [Op; 7] = [
    Op::FAbs,
    Op::FNeg,
    Op::FCeil,
    Op::FFloor,
    Op::FTrunc,
    Op::FNearest,
    Op::FSqrt,
];

const FLOAT_BINARY: [Op; 7] = [
    Op::FAdd,
    Op::FSub,
    Op::FMul,
    Op::FDiv,
    Op::FMin,
    Op::FMax,
    Op::FCopysign,
];

fn load_kind(opcode: u8) -> (LoadKind, Type) {
    match opcode {
        opcode::I32_LOAD => (LoadKind::I32, Type::I32),
        opcode::I64_LOAD => (LoadKind::I64, Type::I64),
        opcode::F32_LOAD => (LoadKind::F32, Type::F32),
        opcode::F64_LOAD => (LoadKind::F64, Type::F64),
        opcode::I32_LOAD8_S => (LoadKind::I32S8, Type::I32),
        opcode::I32_LOAD8_U => (LoadKind::I32U8, Type::I32),
        opcode::I32_LOAD16_S => (LoadKind::I32S16, Type::I32),
        opcode::I32_LOAD16_U => (LoadKind::I32U16, Type::I32),
        opcode::I64_LOAD8_S => (LoadKind::I64S8, Type::I64),
        opcode::I64_LOAD8_U => (LoadKind::I64U8, Type::I64),
        opcode::I64_LOAD16_S => (LoadKind::I64S16, Type::I64),
        opcode::I64_LOAD16_U => (LoadKind::I64U16, Type::I64),
        opcode::I64_LOAD32_S => (LoadKind::I64S32, Type::I64),
        _ => (LoadKind::I64U32, Type::I64),
    }
}

fn store_size_log2(opcode: u8) -> u8 {
    match opcode {
        opcode::I32_STORE8 | opcode::I64_STORE8 => 0,
        opcode::I32_STORE16 | opcode::I64_STORE16 => 1,
        opcode::I32_STORE | opcode::F32_STORE | opcode::I64_STORE32 => 2,
        _ => 3,
    }
}

#[allow(dead_code)]
fn condition_name(flags: u8) -> &'static str {
    IntCondition::from_flags(flags).name()
}

#[allow(dead_code)]
fn float_condition_name(flags: u8) -> &'static str {
    FloatCondition::from_flags(flags).name()
}
