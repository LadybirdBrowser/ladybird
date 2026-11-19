/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The memory a thread compiles with.

use ak::Zone;

use crate::artifact::ArtifactWriter;
use crate::codegen::sites::SiteScratch;
use crate::passes::dominators::Dominators;
use crate::regalloc::Regalloc;

/// What a compile keeps its data in, apart from what the heap still holds of it. A thread that compiles has one, and
/// every compile of the thread uses it: a compile leaves nothing in it that the next one reads, only the memory.
/// The memory is in a few ranges of address space that are the scratch's own, so all of it can go back to the system
/// at any time between two compiles (see `release_memory()`), whatever else the process allocated since.
pub struct Scratch {
    /// Where the graph of a compile is, with all the lists of its nodes, blocks and frame states. A compile starts
    /// with a zone that holds nothing.
    pub(crate) zone: Zone,
    /// Where the stages of a compile keep the data that only they use. The stage that is done leaves nothing in it
    /// for the next one: the driver drops the data of the builder, and of each pass, when it ends.
    pub(crate) work: Zone,
    pub(crate) tables: Tables,
}

/// The tables of a compile that are not part of its graph.
pub struct Tables {
    pub(crate) artifact: ArtifactWriter,
    pub(crate) sites: SiteScratch,
    pub(crate) regalloc: Regalloc,
    /// The dominators of the graph. The passes and the register allocator compute them when they need them, for the
    /// graph as it is then.
    pub(crate) dominators: Dominators,
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
            tables: Tables {
                artifact: ArtifactWriter::new(),
                sites: SiteScratch::new(),
                regalloc: Regalloc::new(),
                dominators: Dominators::new(),
            },
        }
    }

    /// Drops the data of the compile that ended. The zones keep their memory, so that the next compile of the thread
    /// does not take it from the system again: a thread that has no compile for a while gives it back (see
    /// `release_memory()`).
    pub(crate) fn end_compile(&mut self) {
        for zone in [&mut self.zone, &mut self.work] {
            zone.reset();
        }
    }

    /// Gives the memory of the scratch back to the system. The next compile takes memory again.
    pub fn release_memory(&mut self) {
        for zone in [&mut self.zone, &mut self.work] {
            zone.reset();
            zone.trim(0);
        }
        self.tables.artifact.release_memory();
        self.tables.sites.release_memory();
        self.tables.regalloc.release_memory();
        self.tables.dominators.release_memory();
    }
}
