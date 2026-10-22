/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Exception handling, in the IR of the function.
//!
//! A handler is a `try_table`, or the body of a legacy `try`. Every way an exception comes up in a handler's region
//! is an edge to the handler's dispatch, carrying the exception: a throw branches there, and a call checks the
//! context's `exception` when it returns, which the runtime sets when it unwinds an exception to the call (a call
//! with `FLAG_CHECKS_EXCEPTION` is an exception site). The dispatch is a block of its own, made where the region ends,
//! which tests the clauses in order: the first that catches the exception gets what it pushes, and an exception none
//! catches goes on to the handler around, or leaves the function through `Op::Throw`, which has the runtime unwind it
//! to the callers.
//!
//! The clauses of a `try_table` branch to labels around it. Those of a legacy `try` branch to its handlers, the
//! `catch` and `catch_all` blocks that follow its body, which run outside the region and keep the exception for
//! `rethrow`. A body that ends with `delegate` has no clauses, and its exceptions go to the handler that the label is,
//! or is in.

use super::{Builder, ControlKind, Edge};
use crate::ir::{BlockId, FLAG_CHECKS_EXCEPTION, Func, Helper, IntCondition, NONE, Node, Op, Type, Value};
use libwasm_module::Module;
use libwasm_module::abi::{EXCEPTION_PAYLOAD_OFFSET, EXCEPTION_TAG_OFFSET, ExecutionContext, InstanceData};
use libwasm_module::types::ValType;
use std::mem::offset_of;

/// A clause of a handler: catch (0), catch_ref (1), catch_all (2) or catch_all_ref (3), its tag, and for `try_table`
/// its label. For a legacy `try`, once the dispatch is made, the block and successor slot of its edge to the handler,
/// or NONE.
#[derive(Debug, Clone, Copy)]
pub(super) struct Clause {
    pub kind: u32,
    pub tag: u32,
    pub label: u32,
    pub block: BlockId,
}

const CATCH_ALL: u32 = 2;

fn pushes_reference(kind: u32) -> bool {
    kind == 1 || kind == 3
}

impl Builder {
    /// The parameter types of tag `tag`.
    fn tag_params(module: &Module, tag: u32) -> &[ValType] {
        let type_index = module.tag_space[tag as usize].type_index;
        &module.types[type_index as usize]
            .as_func()
            .expect("tags have function types")
            .params
    }

    /// Makes the construct just pushed a handler, with the clauses from `clauses` on.
    fn begin_handler(&mut self, clauses: usize) {
        let index = self.controls.len() - 1;
        let control = &mut self.controls[index];
        control.clauses = (clauses as u32, (self.clauses.len() - clauses) as u32);
        control.outer_handler = self.handler;
        self.handler = index as u32;
    }

    pub(super) fn begin_try_table(&mut self, params: (u32, u32), results: (u32, u32), clauses: &[[u32; 3]]) {
        let start = self.clauses.len();
        self.clauses.extend(clauses.iter().map(|&[kind, tag, label]| Clause {
            kind,
            tag,
            label,
            block: NONE,
        }));
        self.push_control(ControlKind::TryTable, params, results, NONE);
        self.begin_handler(start);
    }

    /// A legacy `try` whose body starts at module offset `body`.
    pub(super) fn begin_try(&mut self, module: &Module, params: (u32, u32), results: (u32, u32), body: usize) {
        let info = &module.function_infos[self.defined as usize];
        // NB: The regions are in the order their instructions appear, and the builder meets them in that order (but
        //     for those in unreachable code, which it skips), so the region is at or after the cursor.
        let regions = &info.try_regions;
        while (regions[self.try_region_cursor].start as usize) < body {
            self.try_region_cursor += 1;
        }
        let region = &regions[self.try_region_cursor];
        assert_eq!(region.start as usize, body, "validation made a region of every try");
        self.try_region_cursor += 1;
        let start = self.clauses.len();
        let clauses =
            &info.catch_clauses[region.first_clause as usize..(region.first_clause + region.clause_count) as usize];
        for clause in clauses {
            let kind = match clause.kind {
                libwasm_module::validator::CatchKind::Catch => 0,
                _ => CATCH_ALL,
            };
            self.clauses.push(Clause {
                kind,
                tag: clause.tag,
                label: NONE,
                block: NONE,
            });
        }
        self.push_control(ControlKind::Try, params, results, NONE);
        self.begin_handler(start);
    }

    /// Records an edge from the current block, through the successor `slot`, to the dispatch of the handler
    /// `controls[handler]`, carrying `exception`.
    fn add_throw_edge(&mut self, handler: u32, slot: u32, exception: Value) {
        let values = self.edge_values.len() as u32;
        self.edge_values.push(exception);
        let control = &mut self.controls[handler as usize];
        self.edges.push(Edge {
            block: self.current_block,
            slot,
            values,
            next: control.throws,
        });
        control.throws = self.edges.len() as u32 - 1;
    }

    /// Ends the current block by throwing `exception` to the handler `handler`, or out of the function if it is NONE.
    fn throw_to(&mut self, func: &mut Func, handler: u32, exception: Value) {
        if handler == NONE {
            self.terminate(func, Node::new(Op::Throw, Type::None, 0, [exception, 0, 0]), &[]);
        } else {
            let slot = self.jump(func, NONE);
            self.add_throw_edge(handler, slot, exception);
        }
    }

    /// Throws `exception` from the current position, and makes what follows unreachable.
    pub(super) fn throw_exception(&mut self, func: &mut Func, exception: Value) {
        let handler = self.handler;
        self.throw_to(func, handler, exception);
        self.set_unreachable();
    }

    /// `throw tag`: a new exception with the tag and the values on the stack.
    pub(super) fn throw_tag(&mut self, func: &mut Func, module: &Module, tag: u32) {
        let identity = self.instance_pointer(func, offset_of!(InstanceData, tags), tag, Type::Pointer);
        let exception = self.helper(func, Helper::AllocateException, &[identity], Type::Ref);
        let payload = self.read_field(func, exception, EXCEPTION_PAYLOAD_OFFSET, Type::Pointer);
        let params = Self::tag_params(module, tag);
        // The payload is in operand stack order: the last value in the lowest slot.
        let mut slot = 0;
        for &param in params.iter().rev() {
            let value = func.resolve(self.pop());
            self.write_payload(func, payload, slot, param, value);
            slot += param.slot_count();
        }
        self.throw_exception(func, exception);
    }

    fn read_field(&mut self, func: &mut Func, pointer: Value, offset: usize, ty: Type) -> Value {
        let bytes = if ty == Type::I32 { 4 } else { 8 };
        Self::append(func, Node::new(Op::ReadField, ty, bytes, [pointer, offset as u32, 0]))
    }

    fn write_field(func: &mut Func, pointer: Value, offset: u32, value: Value) {
        let bytes = if func.node(value).ty == Type::I32 { 4 } else { 8 };
        Self::append(
            func,
            Node::new(Op::WriteField, Type::None, bytes, [pointer, value, offset]),
        );
    }

    /// Stores `value`, of type `ty`, in the payload at `payload` from `slot` on.
    fn write_payload(&mut self, func: &mut Func, payload: Value, slot: u32, ty: ValType, value: Value) {
        let offset = 8 * slot;
        match ty {
            ValType::I32 | ValType::I64 | ValType::Ref(_) => Self::write_field(func, payload, offset, value),
            ValType::F32 | ValType::F64 => {
                let bits = if ty == ValType::F32 { Type::I32 } else { Type::I64 };
                let bits = self.unary(func, Op::Reinterpret, bits, value);
                Self::write_field(func, payload, offset, bits);
            }
            ValType::V128 => {
                for lane in 0..2 {
                    let half = self.pure(
                        func,
                        Node::new(Op::ExtractLane, Type::I64, 0, [value, I64X2_EXTRACT_LANE, lane]),
                    );
                    Self::write_field(func, payload, offset + 8 * lane, half);
                }
            }
        }
    }

    /// The value of type `ty` in the payload at `payload` from `slot` on.
    fn read_payload(&mut self, func: &mut Func, payload: Value, slot: u32, ty: ValType) -> Value {
        let offset = 8 * slot as usize;
        match ty {
            ValType::I32 => self.read_field(func, payload, offset, Type::I32),
            ValType::I64 => self.read_field(func, payload, offset, Type::I64),
            ValType::Ref(_) => self.read_field(func, payload, offset, Type::Ref),
            ValType::F32 => {
                let bits = self.read_field(func, payload, offset, Type::I32);
                self.unary(func, Op::Reinterpret, Type::F32, bits)
            }
            ValType::F64 => {
                let bits = self.read_field(func, payload, offset, Type::I64);
                self.unary(func, Op::Reinterpret, Type::F64, bits)
            }
            ValType::V128 => {
                let low = self.read_field(func, payload, offset, Type::I64);
                let high = self.read_field(func, payload, offset + 8, Type::I64);
                let vector = self.pure(func, Node::new(Op::VectorUnary, Type::V128, 0, [low, I64X2_SPLAT, 0]));
                self.pure(
                    func,
                    Node::new(
                        Op::ReplaceLane,
                        Type::V128,
                        0,
                        [vector, high, I64X2_REPLACE_LANE | (1 << 16)],
                    ),
                )
            }
        }
    }

    /// Pushes what a clause of `kind` with tag `tag` pushes for `exception`.
    fn push_caught(&mut self, func: &mut Func, module: &Module, kind: u32, tag: u32, exception: Value) {
        if kind < CATCH_ALL {
            let params = Self::tag_params(module, tag);
            let payload = self.read_field(func, exception, EXCEPTION_PAYLOAD_OFFSET, Type::Pointer);
            let mut slot: u32 = params.iter().map(|param| param.slot_count()).sum();
            for &param in params {
                slot -= param.slot_count();
                let value = self.read_payload(func, payload, slot, param);
                self.push(value);
            }
        }
        if pushes_reference(kind) {
            self.push(exception);
        }
    }

    /// After a call in a handler's region: continues at the handler's dispatch if the call threw.
    pub(super) fn check_exception(&mut self, func: &mut Func, call: Value) {
        let handler = self.handler;
        if handler == NONE {
            return;
        }
        func.nodes[call as usize].flags |= FLAG_CHECKS_EXCEPTION;
        let context = self.instance_field(func, offset_of!(InstanceData, context));
        let exception = self.read_field(func, context, offset_of!(ExecutionContext, exception), Type::Ref);
        let zero = self.constant(func, Type::Ref, 0);
        let slot = self.terminate(
            func,
            Node::new(
                Op::BranchCompare,
                Type::None,
                IntCondition::Ne as u8,
                [exception, zero, 0],
            ),
            &[NONE, NONE],
        );
        self.add_throw_edge(handler, slot, exception);
        self.continue_after(func, slot + 1);
    }

    /// Starts a block whose only predecessor is the current block, through its successor `slot`.
    fn continue_after(&mut self, func: &mut Func, slot: u32) {
        let block = self.current_block;
        self.continue_from(func, block, slot);
    }

    /// Starts a block whose only predecessor is `predecessor`, through its successor `slot`.
    fn continue_from(&mut self, func: &mut Func, predecessor: BlockId, slot: u32) {
        let block = self.new_block(func);
        Self::set_predecessors(func, block, &[predecessor]);
        func.successors[slot as usize] = block;
        self.start_block(func, block);
    }

    /// Makes the dispatch of the handler `controls[index]`, if anything throws to it, and returns the exception it
    /// starts with, in its block, which is current then. Clears the context's `exception`, which a call that threw
    /// left there.
    fn begin_dispatch(&mut self, func: &mut Func, index: usize) -> Option<Value> {
        let control = self.controls[index];
        if control.throws == NONE {
            return None;
        }
        self.collect_edges(control.throws);
        let block = self.new_block(func);
        let predecessors = std::mem::take(&mut self.scratch_blocks);
        Self::set_predecessors(func, block, &predecessors);
        self.scratch_blocks = predecessors;
        for &edge in &self.scratch_values {
            func.successors[self.edges[edge as usize].slot as usize] = block;
        }
        self.start_block(func, block);
        let exception = self.merge_edge_values(func, block, Type::Ref, 0);
        let context = self.instance_field(func, offset_of!(InstanceData, context));
        let zero = self.constant(func, Type::I64, 0);
        Self::append(
            func,
            Node::new(
                Op::WriteField,
                Type::None,
                8,
                [context, zero, offset_of!(ExecutionContext, exception) as u32],
            ),
        );
        Some(exception)
    }

    /// Ends the current block with a test of whether `exception` has tag `tag`, whose first successor is the match,
    /// and returns the block and the slot of that successor.
    fn test_tag(&mut self, func: &mut Func, tag: u32, exception: Value) -> (BlockId, u32) {
        let identity = self.instance_pointer(func, offset_of!(InstanceData, tags), tag, Type::Pointer);
        let thrown = self.read_field(func, exception, EXCEPTION_TAG_OFFSET, Type::Pointer);
        let slot = self.terminate(
            func,
            Node::new(
                Op::BranchCompare,
                Type::None,
                IntCondition::Eq as u8,
                [thrown, identity, 0],
            ),
            &[NONE, NONE],
        );
        (self.current_block, slot)
    }

    /// Ends the body of the handler `controls[index]`: the fallthrough goes to the end of the construct, and the
    /// handler stops being the current one.
    fn end_region(&mut self, func: &mut Func, index: usize) {
        let control = self.controls[index];
        if !control.unreachable {
            let slot = self.jump(func, NONE);
            self.add_edge(index, slot);
            self.controls[index].unreachable = true;
        }
        self.unnumber(control.numbering_mark);
        self.stack.truncate(control.height as usize);
        self.handler = control.outer_handler;
    }

    /// The `end` of a `try_table`.
    pub(super) fn end_try_table(&mut self, func: &mut Func, module: &Module, index: usize) {
        let control = self.controls[index];
        if control.throws == NONE {
            self.handler = control.outer_handler;
            self.end_block(func, index);
            return;
        }
        self.end_region(func, index);
        let exception = self.begin_dispatch(func, index).expect("something throws");
        let mut caught_all = false;
        for clause_index in control.clauses.0..control.clauses.0 + control.clauses.1 {
            let clause = self.clauses[clause_index as usize];
            let otherwise = if clause.kind < CATCH_ALL {
                let (test, slot) = self.test_tag(func, clause.tag, exception);
                self.continue_from(func, test, slot);
                Some((test, slot + 1))
            } else {
                None
            };
            self.push_caught(func, module, clause.kind, clause.tag, exception);
            // NB: The labels are counted from outside the try_table, whose own construct is still open.
            self.branch(func, clause.label + 1);
            self.stack.truncate(control.height as usize);
            match otherwise {
                Some((test, slot)) => self.continue_from(func, test, slot),
                None => {
                    caught_all = true;
                    break;
                }
            }
        }
        if !caught_all {
            self.throw_to(func, control.outer_handler, exception);
        }
        self.end_block(func, index);
    }

    /// `catch` or `catch_all` of a legacy `try`: ends the body or the previous handler, and starts the next handler.
    pub(super) fn begin_catch(&mut self, func: &mut Func, module: &Module) {
        let index = self.controls.len() - 1;
        let control = self.controls[index];
        if control.kind == ControlKind::Try {
            self.end_region(func, index);
            // The dispatch branches to each handler that catches, in order; the handlers' blocks come later.
            let exception = self.begin_dispatch(func, index);
            let mut caught_all = false;
            if let Some(exception) = exception {
                for clause_index in control.clauses.0..control.clauses.0 + control.clauses.1 {
                    let clause = self.clauses[clause_index as usize];
                    let (block, slot) = if clause.kind == CATCH_ALL {
                        let slot = self.jump(func, NONE);
                        (self.current_block, slot)
                    } else {
                        self.test_tag(func, clause.tag, exception)
                    };
                    self.clauses[clause_index as usize].label = slot;
                    self.clauses[clause_index as usize].block = block;
                    if clause.kind == CATCH_ALL {
                        caught_all = true;
                        break;
                    }
                    self.continue_from(func, block, slot + 1);
                }
                if !caught_all {
                    self.throw_to(func, control.outer_handler, exception);
                }
            }
            let control = &mut self.controls[index];
            control.kind = ControlKind::Catch;
            control.exception = exception.unwrap_or(NONE);
            control.next_clause = control.clauses.0;
        } else {
            // The end of the previous handler.
            if !control.unreachable {
                let slot = self.jump(func, NONE);
                self.add_edge(index, slot);
            }
            self.unnumber(control.numbering_mark);
            self.stack.truncate(control.height as usize);
        }
        let control = self.controls[index];
        let clause_index = control.next_clause;
        self.controls[index].next_clause += 1;
        let clause = self.clauses[clause_index as usize];
        if control.exception == NONE || clause.label == NONE {
            // Nothing throws to the try, or an earlier catch_all catches everything: the handler is unreachable.
            self.controls[index].unreachable = true;
            return;
        }
        // The dispatch's edge to this handler: the first successor of the test's block, or the jump of catch_all.
        self.continue_from(func, clause.block, clause.label);
        self.controls[index].unreachable = false;
        self.push_caught(func, module, clause.kind, clause.tag, control.exception);
    }

    /// The `end` of a legacy `try`, after its body or a handler.
    pub(super) fn end_try(&mut self, func: &mut Func, index: usize) {
        let control = self.controls[index];
        if control.kind == ControlKind::Try {
            // A try without handlers: what its body throws goes on to the handler around it.
            if control.throws == NONE {
                self.handler = control.outer_handler;
                self.end_block(func, index);
                return;
            }
            self.end_region(func, index);
            let exception = self.begin_dispatch(func, index).expect("something throws");
            self.throw_to(func, control.outer_handler, exception);
        }
        self.end_block(func, index);
    }

    /// `delegate label` of a legacy `try`: its body ends, and what it throws goes to the handler that the label is,
    /// or the one around it.
    pub(super) fn delegate(&mut self, func: &mut Func, label: u32) {
        let index = self.controls.len() - 1;
        let control = self.controls[index];
        self.handler = control.outer_handler;
        if control.throws != NONE {
            // The label is counted from outside the try. A legacy try whose body is open catches what is delegated to
            // its label; any other construct passes it on to the handler around it.
            let target = index - 1 - label as usize;
            let mut handler = control.outer_handler;
            while handler != NONE
                && !(handler as usize == target && self.controls[target].kind == ControlKind::Try)
                && handler as usize >= target
            {
                handler = self.controls[handler as usize].outer_handler;
            }
            if !control.unreachable {
                let slot = self.jump(func, NONE);
                self.add_edge(index, slot);
                self.controls[index].unreachable = true;
            }
            self.unnumber(control.numbering_mark);
            self.stack.truncate(control.height as usize);
            let exception = self.begin_dispatch(func, index).expect("something throws");
            self.throw_to(func, handler, exception);
        }
        self.end_block(func, index);
    }

    /// `rethrow label`: throws the exception the legacy handler `label` caught again.
    pub(super) fn rethrow(&mut self, func: &mut Func, label: u32) {
        let index = self.controls.len() - 1 - label as usize;
        let exception = self.controls[index].exception;
        debug_assert_ne!(
            exception, NONE,
            "a reachable rethrow is in a handler that caught something"
        );
        self.throw_exception(func, exception);
    }
}

const I64X2_SPLAT: u32 = 0x12;
const I64X2_EXTRACT_LANE: u32 = 0x1d;
const I64X2_REPLACE_LANE: u32 = 0x1e;
