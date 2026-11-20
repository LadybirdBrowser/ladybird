/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The memory a thread compiles with.

use ak::Zone;

/// What a compile keeps its data in, apart from what the heap still holds of it. A thread that compiles has one, and
/// every compile of the thread uses it: a compile leaves nothing in it that the next one reads, only the memory.
/// The memory is in four ranges of address space that are the scratch's own, so all of it can go back to the system
/// at any time between two compiles (see `release_memory()`), whatever else the process allocated since.
pub struct Scratch {
    /// Where the graph of a compile is, with all the lists of its nodes, blocks and frame states. A compile starts
    /// with a zone that holds nothing.
    pub(crate) zone: Zone,
    /// Where the stages of a compile keep the data that only they use. The driver drops the data of the builder,
    /// and of each pass, when it ends. The register allocator keeps the allocation here, with its own work data,
    /// for the code generator, so those stay until the compile ends, with the data of the code generator and of the
    /// tables of the code.
    pub(crate) work: Zone,
    /// Where a stage that repeats a step keeps the data of one step, which the step drops when it ends. A stage
    /// keeps what lasts from one step to the next in `work`. So a stage whose steps each make data as large as the
    /// graph takes the memory of one step, not of all of them. A stage whose steps hand data on to later steps
    /// keeps that data in one of the two zones; when that zone holds much more than the data still in use, the
    /// stage moves the data in use to the other zone and resets the first (see `check_elimination`). The driver
    /// empties both after each stage.
    pub(crate) steps: [Zone; 2],
}

impl Default for Scratch {
    fn default() -> Self {
        Self::new()
    }
}

/// The most bytes that a zone of a compile gives out. It is address space, which costs nothing before it is used, and
/// it is far above what the largest function that the builder accepts takes.
const ZONE_LIMIT: usize = 1 << 34;

impl Scratch {
    /// A scratch, which takes no memory before a compile uses it.
    pub fn new() -> Self {
        Self {
            zone: Zone::new(ZONE_LIMIT, c"JIT graph"),
            work: Zone::new(ZONE_LIMIT, c"JIT work"),
            steps: [Zone::new(ZONE_LIMIT, c"JIT step"), Zone::new(ZONE_LIMIT, c"JIT step 2")],
        }
    }

    /// Drops the data of the compile that ended. The zones keep their memory, so that the next compile of the thread
    /// does not take it from the system again: a thread that has no compile for a while gives it back (see
    /// `release_memory()`).
    pub(crate) fn end_compile(&mut self) {
        let [step, other_step] = &mut self.steps;
        for zone in [&mut self.zone, &mut self.work, step, other_step] {
            zone.reset();
        }
    }

    /// Gives the memory of the scratch back to the system. The next compile takes memory again.
    pub fn release_memory(&mut self) {
        let [step, other_step] = &mut self.steps;
        for zone in [&mut self.zone, &mut self.work, step, other_step] {
            zone.reset();
            zone.trim(0);
        }
    }
}
