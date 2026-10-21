/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Scheduling: rebuilds the node arena in linear order, which leaves out the dead nodes and puts the nodes loop
//! invariant code motion moved (`Licm::home`) at the end of their new block, before its terminator. Every input is
//! renumbered, so a node's index is its position again.
//!
//! The entry values come first, in the range of the entry block, and the phis last, outside every block's range,
//! since they are defined where their block starts wherever they are. Global value numbering (`gvn`) runs in the
//! same walk and leaves out the nodes it replaces.

use crate::ir::{Func, NONE, Node, Op, Shape, Value};
use crate::passes::cleanup::Liveliness;
use crate::passes::gvn::Gvn;

#[derive(Default)]
pub struct Scheduler {
    order: Vec<Value>,
    renumbered: Vec<Value>,
    nodes: Vec<Node>,
    hoisted: Vec<Vec<Value>>,
}

impl Scheduler {
    /// Schedules `func`, moving nodes as `home` says, and numbering values with `gvn` if it is given, for a module
    /// with `globals` globals.
    pub fn run(
        &mut self,
        func: &mut Func,
        liveliness: &mut Liveliness,
        home: &[u32],
        mut gvn: Option<&mut Gvn>,
        globals: usize,
    ) {
        let count = func.nodes.len();
        if let Some(gvn) = gvn.as_deref_mut() {
            gvn.start(func, globals);
        }
        for list in &mut self.hoisted {
            list.clear();
        }
        self.hoisted.resize_with(func.blocks.len(), Vec::new);
        for (value, &block) in home.iter().enumerate() {
            if block != NONE {
                self.hoisted[block as usize].push(value as Value);
            }
        }
        self.order.clear();
        let live = |value: Value| !liveliness.is_dead(value);
        // The entry values, wherever cleanup or the builder put them.
        for value in 0..count as Value {
            if live(value) && func.nodes[value as usize].op.is_entry_value() {
                self.order.push(value);
            }
        }
        for block in 0..func.blocks.len() {
            let data = func.blocks[block];
            let start = if block == 0 { 0 } else { self.order.len() };
            if let Some(gvn) = gvn.as_deref_mut() {
                gvn.enter_block(func, block as u32);
            }
            let mut emit = |value: Value, order: &mut Vec<Value>| {
                if let Some(gvn) = gvn.as_deref_mut()
                    && gvn.visit(func, value).is_some()
                {
                    return;
                }
                order.push(value);
            };
            for value in data.start..data.end - 1 {
                let op = func.nodes[value as usize].op;
                if live(value)
                    && !op.is_entry_value()
                    && !matches!(op, Op::Phi | Op::Forward)
                    && home[value as usize] == NONE
                {
                    emit(value, &mut self.order);
                }
            }
            for index in 0..self.hoisted[block].len() {
                emit(self.hoisted[block][index], &mut self.order);
            }
            emit(data.end - 1, &mut self.order);
            func.blocks[block].start = start as u32;
            func.blocks[block].end = self.order.len() as u32;
        }
        for block in 0..func.blocks.len() {
            let mut phi = func.blocks[block].first_phi;
            while phi != NONE {
                self.order.push(phi);
                phi = func.nodes[phi as usize].args[2];
            }
        }

        self.renumbered.clear();
        self.renumbered.resize(count, NONE);
        for (index, value) in self.order.iter().enumerate() {
            self.renumbered[*value as usize] = index as Value;
        }
        if let Some(gvn) = gvn {
            for value in 0..count {
                let canon = gvn.canon[value];
                if canon as usize != value {
                    self.renumbered[value] = self.renumbered[canon as usize];
                }
            }
        }
        self.nodes.clear();
        for &value in &self.order {
            let mut node = func.nodes[value as usize];
            match node.op.shape() {
                Shape::Fixed(inputs) => {
                    for slot in 0..inputs as usize {
                        node.args[slot] = self.renumbered[node.args[slot] as usize];
                    }
                }
                Shape::Variadic => {
                    let start = node.args[0] as usize;
                    for slot in start..start + node.args[1] as usize {
                        func.operands[slot] = self.renumbered[func.operands[slot] as usize];
                    }
                }
            }
            match node.op {
                Op::CallResult => node.args[0] = self.renumbered[node.args[0] as usize],
                Op::Phi if node.args[2] != NONE => node.args[2] = self.renumbered[node.args[2] as usize],
                _ => {}
            }
            self.nodes.push(node);
        }
        for block in &mut func.blocks {
            if block.first_phi != NONE {
                block.first_phi = self.renumbered[block.first_phi as usize];
            }
        }
        std::mem::swap(&mut func.nodes, &mut self.nodes);
        liveliness.dead.clear();
        liveliness.dead.resize(func.nodes.len(), false);
    }
}
