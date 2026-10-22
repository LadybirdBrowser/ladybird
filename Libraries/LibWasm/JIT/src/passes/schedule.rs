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
//! since they are defined where their block starts wherever they are.

use crate::ir::{Func, NONE, Node, Op, Shape, Value};
use crate::passes::cleanup::Liveliness;

#[derive(Default)]
pub struct Scheduler {
    order: Vec<Value>,
    renumbered: Vec<Value>,
    nodes: Vec<Node>,
    hoisted: Vec<Vec<Value>>,
}

impl Scheduler {
    /// Schedules `func`, moving nodes as `home` says.
    pub fn run(&mut self, func: &mut Func, liveliness: &mut Liveliness, home: &[u32]) {
        let count = func.nodes.len();
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
            for value in data.start..data.end - 1 {
                let op = func.nodes[value as usize].op;
                if live(value)
                    && !op.is_entry_value()
                    && !matches!(op, Op::Phi | Op::Forward)
                    && home[value as usize] == NONE
                {
                    self.order.push(value);
                }
            }
            for index in 0..self.hoisted[block].len() {
                self.order.push(self.hoisted[block][index]);
            }
            self.order.push(data.end - 1);
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
