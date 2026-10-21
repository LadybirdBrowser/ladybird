/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Global value numbering, over the dominator tree, as part of scheduling's walk in linear order: a node computing
//! what a node of a block that dominates it computed already is replaced by that node.
//!
//! The builder numbers values too, but forgets what a construct numbered when it ends, even where that dominates the
//! code after it (whatever a `block` computes before its first branch out, say). This pass keeps every entry for
//! as long as its block dominates the walk.
//!
//! Reads are numbered by alias class, with epochs that the writes of the class advance: memory loads by a memory
//! epoch that stores and calls advance, and global values by an epoch of their global that its `global.set`
//! advances and one of all globals that calls advance. A block continues the epochs of the block before it only if
//! that is its only predecessor, so that a read is only ever shared with one on every path to it.

use crate::ir::{BlockId, Effect, FLAG_IMMUTABLE, Func, NONE, Op, Shape, Value};
use crate::passes::dominators::Dominators;
use crate::table::Table;

type Key = [u32; 6];

#[derive(Default)]
pub struct Gvn {
    dominators: Dominators,
    table: Table<Key>,
    log: Vec<(Key, u32)>,
    /// The blocks whose entries are in the table, from the entry down the dominator tree, and where their entries
    /// start in the log.
    stack: Vec<(BlockId, u32)>,
    /// The node each node was replaced by, or itself.
    pub canon: Vec<Value>,
    memory_epoch: u32,
    global_epochs: Vec<u32>,
    globals_epoch: u32,
    previous: BlockId,
    /// How many nodes were replaced.
    pub eliminated: u32,
}

impl Gvn {
    pub fn start(&mut self, func: &Func, globals: usize) {
        self.dominators.compute(func);
        self.table.clear();
        self.log.clear();
        self.stack.clear();
        self.canon.clear();
        self.canon.extend(0..func.nodes.len() as Value);
        self.memory_epoch = 0;
        self.global_epochs.clear();
        self.global_epochs.resize(globals, 0);
        self.globals_epoch = 0;
        self.previous = NONE;
        self.eliminated = 0;
    }

    pub fn enter_block(&mut self, func: &Func, block: BlockId) {
        while let Some(&(top, mark)) = self.stack.last() {
            if self.dominators.dominates(top, block) {
                break;
            }
            while self.log.len() > mark as usize {
                let (key, previous) = self.log.pop().expect("the log is longer than the mark");
                self.table.insert(key, previous);
            }
            self.stack.pop();
        }
        if func.predecessors(block) != [self.previous] {
            self.memory_epoch += 1;
            self.globals_epoch += 1;
        }
        self.stack.push((block, self.log.len() as u32));
        self.previous = block;
    }

    /// Numbers `value`, and returns the node that computes the same, if one does.
    pub fn visit(&mut self, func: &Func, value: Value) -> Option<Value> {
        let node = func.node(value);
        let epochs = match node.op {
            Op::Store => {
                self.memory_epoch += 1;
                return None;
            }
            Op::GlobalSet => {
                self.global_epochs[node.args[2] as usize] += 1;
                return None;
            }
            Op::Call | Op::Helper => {
                self.memory_epoch += 1;
                self.globals_epoch += 1;
                return None;
            }
            Op::Load | Op::VectorLoad => [self.memory_epoch, 0],
            Op::GlobalGet if node.flags & FLAG_IMMUTABLE == 0 => {
                [self.global_epochs[node.args[1] as usize], self.globals_epoch]
            }
            Op::Phi | Op::Forward | Op::CallResult | Op::Const128 => return None,
            _ if node.effect() == Effect::Pure && !node.op.is_entry_value() => [0, 0],
            _ => return None,
        };
        let Shape::Fixed(inputs) = node.op.shape() else {
            return None;
        };
        let mut arguments = node.args;
        for argument in arguments.iter_mut().take(inputs as usize) {
            *argument = self.canon[*argument as usize];
        }
        let key = [
            node.op as u32 | ((node.ty as u32) << 16) | (u32::from(node.flags) << 24),
            arguments[0],
            arguments[1],
            arguments[2],
            epochs[0],
            epochs[1],
        ];
        if let Some(existing) = self.table.get(key)
            && existing != NONE
        {
            self.canon[value as usize] = existing;
            self.eliminated += 1;
            return Some(existing);
        }
        let previous = self.table.insert(key, value).unwrap_or(NONE);
        self.log.push((key, previous));
        None
    }
}
