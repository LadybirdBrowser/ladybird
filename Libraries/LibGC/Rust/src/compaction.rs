/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compaction policy: which blocks a collection that may move cells evacuates.
//!
//! Cells only move within their allocator, so each allocator is looked at on its own. Its blocks are taken emptiest
//! first, as long as the live cells of the blocks taken still fit into the free slots of the blocks left. Then every
//! block that fits is a block that the collection frees, unless something pins one of its cells. Of the blocks that fit
//! in all allocators, the ones that move the fewest bytes go first, since each frees one block, until moving the next
//! one would go over the budget. A block that an earlier collection could not empty because something pinned some of
//! its cells is not taken again until one of the cells it kept dies, since evacuating it again would only move the
//! cells that moved into it since.

use crate::block::BLOCK_SIZE;
use crate::rust_panic::abort_on_panic;

/// A block of a movable allocator, as the allocator knows it before the collection marks.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct CompactionCandidate {
    pub block: *mut core::ffi::c_void,
    /// Which allocator the block belongs to. The blocks of an allocator come one after another.
    pub allocator: u32,
    /// The cells of the block that are allocated: at least the ones that are alive. A block whose count the allocator
    /// does not know counts as full.
    pub live_cells: u32,
    pub cell_count: u32,
    pub cell_size: u32,
    /// Whether an earlier collection could not empty the block because of pinned cells, see
    /// BlockDescriptor::kept_by_pins. Its free slots still take the cells of other blocks.
    pub kept_by_pins: bool,
    /// Set for the blocks the collection evacuates.
    pub selected: bool,
}

/// What selection found.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CompactionSelection {
    /// How many bytes of blocks compaction could free in these allocators if it moved every cell it had to, which is
    /// the blocks each allocator has beyond the fewest that its live cells fit in.
    pub recoverable_bytes: usize,
    /// The blocks whose live cells fit into the free slots of their allocator's other blocks, which compaction frees
    /// unless something pins their cells. The ones over the budget are left for another collection.
    pub fitting_blocks: usize,
    pub selected_blocks: usize,
    /// The bytes of the live cells of the selected blocks, which move unless pinned.
    pub bytes_to_move: usize,
}

/// Selects the blocks to evacuate, moving at most `budget_bytes` of cells, and says how much could be recovered.
fn select(candidates: &mut [CompactionCandidate], budget_bytes: usize) -> CompactionSelection {
    let mut selection = CompactionSelection::default();
    // The blocks that fit, of all allocators, by index into the candidates.
    let mut fitting = Vec::new();
    let mut start = 0;
    while start < candidates.len() {
        let allocator = candidates[start].allocator;
        let end = start
            + candidates[start..]
                .iter()
                .take_while(|candidate| candidate.allocator == allocator)
                .count();
        let blocks = &candidates[start..end];
        debug_assert!(blocks.iter().all(|block| block.cell_count == blocks[0].cell_count));
        let cell_count = blocks[0].cell_count as usize;

        let live_cells: usize = blocks.iter().map(|block| block.live_cells as usize).sum();
        let fewest_blocks = live_cells.div_ceil(cell_count);
        selection.recoverable_bytes += (blocks.len() - fewest_blocks) * BLOCK_SIZE;

        let mut by_live_cells: Vec<usize> = (start..end).collect();
        by_live_cells.sort_by_key(|&index| candidates[index].live_cells);
        let mut free_cells_elsewhere = blocks.len() * cell_count - live_cells;
        let mut cells_to_move = 0;
        for index in by_live_cells {
            let block = &candidates[index];
            let live = block.live_cells as usize;
            // NB: A full block frees nothing that its cells would not take elsewhere.
            if live >= cell_count {
                break;
            }
            if block.kept_by_pins {
                continue;
            }
            let free = cell_count - live;
            if cells_to_move + live > free_cells_elsewhere - free {
                break;
            }
            cells_to_move += live;
            free_cells_elsewhere -= free;
            fitting.push(index);
        }
        start = end;
    }

    let bytes_of = |candidate: &CompactionCandidate| candidate.live_cells as usize * candidate.cell_size as usize;
    selection.fitting_blocks = fitting.len();
    if budget_bytes == 0 {
        return selection;
    }
    // NB: A stable sort keeps the blocks of an allocator emptiest first, so the ones within budget fit as well.
    fitting.sort_by_key(|&index| bytes_of(&candidates[index]));
    for index in fitting {
        let bytes = bytes_of(&candidates[index]);
        if selection.bytes_to_move + bytes > budget_bytes {
            break;
        }
        selection.bytes_to_move += bytes;
        selection.selected_blocks += 1;
        candidates[index].selected = true;
    }
    selection
}

/// Selects the blocks of `candidates` that a collection evacuates, as the module comment says, moving at most
/// `budget_bytes` of cells. A budget of 0 only tells how much could be recovered.
///
/// # Safety
///
/// `candidates` must point to `count` candidates, the blocks of each allocator one after another.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_compaction_select(
    candidates: *mut CompactionCandidate,
    count: usize,
    budget_bytes: usize,
) -> CompactionSelection {
    abort_on_panic(|| {
        if count == 0 {
            return CompactionSelection::default();
        }
        // SAFETY: The caller passes count candidates.
        select(unsafe { ak::slice_from_raw_parts_mut(candidates, count) }, budget_bytes)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn candidate(allocator: u32, live_cells: u32) -> CompactionCandidate {
        CompactionCandidate {
            block: core::ptr::null_mut(),
            allocator,
            live_cells,
            cell_count: 100,
            cell_size: 64,
            kept_by_pins: false,
            selected: false,
        }
    }

    #[test]
    fn selects_the_emptiest_blocks_whose_cells_fit_elsewhere() {
        let mut candidates = [candidate(0, 10), candidate(0, 50), candidate(0, 20), candidate(0, 90)];
        let selection = select(&mut candidates, usize::MAX);
        // 170 live cells fit in 2 blocks of 4. The blocks with 10 and 20 fit into the free slots of the others.
        assert_eq!(selection.recoverable_bytes, 2 * BLOCK_SIZE);
        assert_eq!(selection.fitting_blocks, 2);
        assert_eq!(selection.selected_blocks, 2);
        let selected: Vec<bool> = candidates.iter().map(|candidate| candidate.selected).collect();
        assert_eq!(selected, [true, false, true, false]);
    }

    #[test]
    fn selects_nothing_from_full_allocators() {
        let mut candidates = [candidate(0, 100), candidate(0, 95), candidate(1, 100)];
        let selection = select(&mut candidates, usize::MAX);
        assert_eq!(selection.recoverable_bytes, 0);
        assert_eq!(selection.selected_blocks, 0);
    }

    #[test]
    fn skips_blocks_kept_by_pins_but_fills_them() {
        let mut candidates = [candidate(0, 10), candidate(0, 20), candidate(0, 50)];
        candidates[0].kept_by_pins = true;
        let selection = select(&mut candidates, usize::MAX);
        assert_eq!(selection.fitting_blocks, 2);
        let selected: Vec<bool> = candidates.iter().map(|candidate| candidate.selected).collect();
        assert_eq!(selected, [false, true, true]);
    }

    #[test]
    fn keeps_to_the_budget_with_the_cheapest_blocks_first() {
        let mut candidates = [candidate(0, 30), candidate(0, 5), candidate(1, 10), candidate(1, 10)];
        let selection = select(&mut candidates, 15 * 64);
        assert_eq!(selection.fitting_blocks, 2);
        assert_eq!(selection.selected_blocks, 2);
        assert_eq!(selection.bytes_to_move, 15 * 64);
        let selected: Vec<bool> = candidates.iter().map(|candidate| candidate.selected).collect();
        assert_eq!(selected, [false, true, true, false]);
    }
}
