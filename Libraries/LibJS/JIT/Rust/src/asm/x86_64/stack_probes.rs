/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Stack probes, for the JITs of both LibJS and LibWasm. Windows commits a thread's stack as code touches the guard
//! pages below the committed part, and code that skips past them faults. So code that takes stack touches the pages
//! it takes from the top down, each touch at most a page below the one before. Reading a guard page commits it just
//! as well as writing it, and keeps what is on the stack.

use super::Address;
use super::AluOp;
use super::Assembler;
use super::Cond;
use super::Gpr;
use super::Width;

/// Whether code that takes stack has to touch it.
pub const PROBES_STACK: bool = cfg!(windows);

/// How far apart the touches are: the page size.
const STACK_PROBE_INTERVAL: i32 = 4096;

/// Code that takes less stack than this does not touch it. Two such takes in a row, or one and the return address of a
/// call, stay within a page of what was touched last, so that nothing is skipped however they fall on the pages.
const STACK_PROBE_THRESHOLD: i32 = STACK_PROBE_INTERVAL / 2;

/// Up to this many touches above the bottom one are emitted one by one, and more take a loop.
const UNROLLED_STACK_PROBES: i32 = 4;

/// Whether code that takes `size` bytes of stack has to touch them.
pub const fn needs_stack_probes(size: i32) -> bool {
    PROBES_STACK && size >= STACK_PROBE_THRESHOLD
}

impl Assembler {
    /// Touches the stack from the address in `top`, which is at most a page below what was touched last, down to the
    /// address in `bottom`: every page in between, then `bottom` itself. `size` is `top - bottom` if it is known when
    /// the code is generated, which keeps small ranges from taking a loop. Clobbers `top` and the flags.
    pub fn probe_stack(&mut self, top: Gpr, bottom: Gpr, size: Option<i32>) {
        match size {
            Some(size) if size <= (UNROLLED_STACK_PROBES + 1) * STACK_PROBE_INTERVAL => {
                let mut offset = STACK_PROBE_INTERVAL;
                while offset < size {
                    self.touch_stack(&Address::new(top, -offset), top);
                    offset += STACK_PROBE_INTERVAL;
                }
            }
            _ => {
                let (again, done) = (self.new_label(), self.new_label());
                self.bind(again);
                self.sub_ri(Width::W64, top, STACK_PROBE_INTERVAL);
                self.cmp_rr(Width::W64, top, bottom);
                self.jcc(Cond::BelowOrEqual, done);
                self.touch_stack(&Address::new(top, 0), top);
                self.jmp(again);
                self.bind(done);
            }
        }
        self.touch_stack(&Address::new(bottom, 0), top);
    }

    /// Reads the stack at `address` by comparing it with `register`, which keeps every register as it is.
    fn touch_stack(&mut self, address: &Address, register: Gpr) {
        self.alu_rm(AluOp::Cmp, Width::W32, register, address);
    }
}
