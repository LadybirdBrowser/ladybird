/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The dominator tree. Linear order is a topological order of the forward edges and loops are entered at their
//! header only, so one pass over the blocks finds every immediate dominator. The tree is numbered in preorder, so
//! that a dominance query compares two numbers.

use crate::ir::{BlockId, Func, NONE};

#[derive(Default)]
pub struct Dominators {
    pub idom: Vec<BlockId>,
    /// The preorder number of each block, and the largest number in its subtree.
    pre: Vec<u32>,
    last: Vec<u32>,
    children_start: Vec<u32>,
    children: Vec<BlockId>,
    stack: Vec<(BlockId, u32)>,
}

impl Dominators {
    pub fn compute(&mut self, func: &Func) {
        let count = func.blocks.len();
        self.idom.clear();
        self.idom.resize(count, NONE);
        if count == 0 {
            return;
        }
        self.idom[0] = 0;
        for block in 1..count as BlockId {
            let mut dominator = NONE;
            for &predecessor in func.predecessors(block) {
                if predecessor >= block || self.idom[predecessor as usize] == NONE {
                    continue;
                }
                dominator = if dominator == NONE {
                    predecessor
                } else {
                    let (mut a, mut b) = (dominator, predecessor);
                    while a != b {
                        while a > b {
                            a = self.idom[a as usize];
                        }
                        while b > a {
                            b = self.idom[b as usize];
                        }
                    }
                    a
                };
            }
            self.idom[block as usize] = dominator;
        }
        // Children lists, then a preorder walk.
        self.children_start.clear();
        self.children_start.resize(count + 1, 0);
        for block in 1..count {
            let parent = self.idom[block];
            if parent != NONE {
                self.children_start[parent as usize + 1] += 1;
            }
        }
        for index in 0..count {
            self.children_start[index + 1] += self.children_start[index];
        }
        self.children.clear();
        self.children.resize(self.children_start[count] as usize, 0);
        let mut cursor = self.children_start.clone();
        for block in 1..count {
            let parent = self.idom[block];
            if parent != NONE {
                self.children[cursor[parent as usize] as usize] = block as BlockId;
                cursor[parent as usize] += 1;
            }
        }
        self.pre.clear();
        self.pre.resize(count, NONE);
        self.last.clear();
        self.last.resize(count, NONE);
        let mut number = 0;
        self.stack.clear();
        self.stack.push((0, self.children_start[0]));
        self.pre[0] = 0;
        while let Some(&mut (block, ref mut next)) = self.stack.last_mut() {
            if *next < self.children_start[block as usize + 1] {
                let child = self.children[*next as usize];
                *next += 1;
                number += 1;
                self.pre[child as usize] = number;
                self.stack.push((child, self.children_start[child as usize]));
            } else {
                self.last[block as usize] = number;
                self.stack.pop();
            }
        }
    }

    /// Whether block `a` dominates block `b`.
    #[inline]
    pub fn dominates(&self, a: BlockId, b: BlockId) -> bool {
        let (pre_a, pre_b) = (self.pre[a as usize], self.pre[b as usize]);
        pre_a <= pre_b && pre_b <= self.last[a as usize]
    }
}
