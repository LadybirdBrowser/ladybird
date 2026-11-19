/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Memory for the tables of installed JIT code (see `libjs_jit::artifact`). The tables of all the code of a VM are in
//! a few chunks of words that come straight from the system, each in a block of its own, so they are no blocks of
//! the heap. A chunk whose blocks are all free goes back to the system.

use core::cell::{Cell, RefCell};
use std::rc::Rc;

use ak::WordChunk;

/// How many words a chunk has, unless it is for tables that need more.
const CHUNK_WORDS: u32 = 256 * 1024;

/// Blocks are multiples of this many words long.
const BLOCK_WORDS: u32 = 16;

struct Chunk {
    /// Identifies the chunk among those of its allocator. Later chunks have higher ids.
    id: u32,
    words: WordChunk,
}

#[derive(Clone, Copy)]
struct FreeBlock {
    /// The id of the chunk the block lies in.
    chunk: u32,
    start: u32,
    len: u32,
}

/// A first-fit allocator of blocks of words in chunks. Free blocks are kept sorted by chunk and start, and merged
/// with their neighbors.
#[derive(Default)]
pub struct DataAllocator {
    free_blocks: Vec<FreeBlock>,
    /// The chunks, in id order.
    chunks: Vec<Rc<Chunk>>,
    next_chunk_id: u32,
}

impl DataAllocator {
    /// A block of `len` words, which is a multiple of `BLOCK_WORDS`: its chunk, and where it starts there.
    fn allocate(&mut self, len: u32) -> (Rc<Chunk>, u32) {
        if let Some(index) = self.free_blocks.iter().position(|block| block.len >= len) {
            let block = &mut self.free_blocks[index];
            let (chunk, start) = (block.chunk, block.start);
            block.start += len;
            block.len -= len;
            if block.len == 0 {
                self.free_blocks.remove(index);
            }
            return (Rc::clone(self.chunk(chunk)), start);
        }

        let chunk_len = CHUNK_WORDS.max(len);
        let chunk = Rc::new(Chunk {
            id: self.next_chunk_id,
            words: WordChunk::new(chunk_len as usize, c"JIT code tables"),
        });
        self.next_chunk_id = self
            .next_chunk_id
            .checked_add(1)
            .expect("a VM maps fewer than 4 Gi chunks");
        self.chunks.push(Rc::clone(&chunk));
        if chunk_len > len {
            // NB: The chunk has the highest id, so the rest of it is the last free block.
            self.free_blocks.push(FreeBlock {
                chunk: chunk.id,
                start: len,
                len: chunk_len - len,
            });
        }
        (chunk, 0)
    }

    fn free(&mut self, chunk: u32, start: u32, len: u32) {
        let index = self
            .free_blocks
            .partition_point(|block| (block.chunk, block.start) < (chunk, start));
        self.free_blocks.insert(index, FreeBlock { chunk, start, len });
        let adjacent = |blocks: &[FreeBlock], first: usize| {
            blocks[first].chunk == blocks[first + 1].chunk
                && blocks[first].start + blocks[first].len == blocks[first + 1].start
        };
        let mut merged = index;
        if index + 1 < self.free_blocks.len() && adjacent(&self.free_blocks, index) {
            self.free_blocks[index].len += self.free_blocks[index + 1].len;
            self.free_blocks.remove(index + 1);
        }
        if index > 0 && adjacent(&self.free_blocks, index - 1) {
            self.free_blocks[index - 1].len += self.free_blocks[index].len;
            self.free_blocks.remove(index);
            merged = index - 1;
        }

        // NB: A chunk that is free as a whole goes back to the system, but for one of the usual size: code comes and
        //     goes, and this keeps a VM whose tables fill its chunks from mapping a chunk again and again.
        let block = self.free_blocks[merged];
        let chunk_len = self.chunk(chunk).words.words().len();
        if block.len as usize != chunk_len {
            return;
        }
        let is_spare = |other: &FreeBlock| other.chunk != chunk && other.start == 0 && other.len == CHUNK_WORDS;
        if block.len == CHUNK_WORDS && !self.free_blocks.iter().any(is_spare) {
            return;
        }
        self.free_blocks.remove(merged);
        self.chunks.retain(|other| other.id != chunk);
    }

    fn chunk(&self, id: u32) -> &Rc<Chunk> {
        let index = self
            .chunks
            .binary_search_by_key(&id, |chunk| chunk.id)
            .expect("a block lies in a chunk of its allocator");
        &self.chunks[index]
    }
}

/// The tables of installed code: a block of a VM's data memory, which goes back to the VM's allocator when dropped.
pub struct DataMemory {
    chunk: Rc<Chunk>,
    start: u32,
    /// How many words the tables have. The block may have some more.
    len: u32,
    allocator: Rc<RefCell<DataAllocator>>,
}

impl DataMemory {
    /// Copies `words` into a newly allocated block.
    pub fn allocate(allocator: &Rc<RefCell<DataAllocator>>, words: &[u32]) -> Self {
        let len = u32::try_from(words.len()).expect("the tables of compiled code have fewer than 4 Gi words");
        let (chunk, start) = allocator.borrow_mut().allocate(block_len(len));
        let memory = Self {
            chunk,
            start,
            len,
            allocator: Rc::clone(allocator),
        };
        for (destination, word) in memory.words().iter().zip(words) {
            destination.set(*word);
        }
        memory
    }

    pub fn words(&self) -> &[Cell<u32>] {
        &self.chunk.words.words()[self.start as usize..][..self.len as usize]
    }
}

impl Drop for DataMemory {
    fn drop(&mut self) {
        self.allocator
            .borrow_mut()
            .free(self.chunk.id, self.start, block_len(self.len));
    }
}

/// How many words the block for tables of `len` words has. Tables with no word have a block too.
fn block_len(len: u32) -> u32 {
    len.max(1)
        .checked_next_multiple_of(BLOCK_WORDS)
        .expect("the tables of compiled code have fewer than 4 Gi words")
}
