/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The memory a thread compiles with.

use crate::artifact::ArtifactWriter;
use crate::codegen::sites::SiteScratch;
use crate::regalloc::Regalloc;

/// What a compile keeps its data in, apart from what the heap still holds of it. A thread that compiles has one, and
/// every compile of the thread uses it: a compile leaves nothing in it that the next one reads, only the memory.
/// The memory is in a few ranges of address space that are the scratch's own, so all of it can go back to the system
/// at any time between two compiles (see `release_memory()`), whatever else the process allocated since.
pub struct Scratch {
    pub(crate) artifact: ArtifactWriter,
    pub(crate) sites: SiteScratch,
    pub(crate) regalloc: Regalloc,
}

impl Default for Scratch {
    fn default() -> Self {
        Self::new()
    }
}

impl Scratch {
    /// A scratch, which takes no memory before a compile uses it.
    pub fn new() -> Self {
        Self {
            artifact: ArtifactWriter::new(),
            sites: SiteScratch::new(),
            regalloc: Regalloc::new(),
        }
    }

    /// Gives the memory of the scratch back to the system. The next compile takes memory again.
    pub fn release_memory(&mut self) {
        self.artifact.release_memory();
        self.sites.release_memory();
        self.regalloc.release_memory();
    }
}
