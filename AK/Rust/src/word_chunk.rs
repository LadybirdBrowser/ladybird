/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Memory for many small tables of words that live and die one by one.

use core::cell::Cell;
use core::ffi::CStr;

use crate::region::Region;

/// A fixed number of 32-bit words in memory that comes straight from the system, and goes back to it when the chunk
/// is dropped.
///
/// An owner of many tables keeps them in a few chunks, each table in a range of words it hands out, so the tables
/// are no blocks of the heap. Everything that holds a part of a chunk shares the chunk, so the words can be read
/// and written through a shared reference, on the one thread that has the chunk.
pub struct WordChunk {
    region: Region,
    len: usize,
}

impl WordChunk {
    /// A chunk of `len` words, which are zero. `name` shows up for its memory in /proc/<pid>/smaps on Linux.
    pub fn new(len: usize, name: &'static CStr) -> Self {
        let size = len
            .checked_mul(size_of::<u32>())
            .expect("a chunk fits in the address space");
        let mut region = Region::new(size, name);
        // NB: A page of the chunk takes memory when it is first written, not here.
        region.commit(size);
        Self { region, len }
    }

    pub fn words(&self) -> &[Cell<u32>] {
        // SAFETY: The region has memory for `len` words from its base on, which is a multiple of 4096 and so aligned
        //         for them. The memory holds zeros until a word is set, so every word has a value. A `Cell<u32>` is laid
        //         out like a `u32`, and nothing refers to the words other than as cells. The region keeps the
        //         memory for as long as the chunk lives, and a chunk is not `Sync`, so the cells stay on the thread
        //         that has it.
        unsafe { crate::slice_from_raw_parts(self.region.base().as_ptr().cast(), self.len) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn words_are_zero_until_they_are_set() {
        let chunk = WordChunk::new(100_000, c"test");
        let words = chunk.words();
        assert_eq!(words.len(), 100_000);
        assert!(words.iter().all(|word| word.get() == 0));
        words[0].set(1);
        words[99_999].set(2);
        // NB: Every view of the chunk is of the same words.
        assert_eq!(chunk.words()[0].get(), 1);
        assert_eq!(chunk.words()[99_999].get(), 2);
        assert_eq!(chunk.words()[1].get(), 0);
    }

    #[test]
    fn a_chunk_may_hold_no_word() {
        let chunk = WordChunk::new(0, c"test");
        assert!(chunk.words().is_empty());
    }

    #[test]
    fn moves_to_another_thread() {
        let chunk = WordChunk::new(16, c"test");
        chunk.words()[3].set(7);
        let chunk = std::thread::spawn(move || {
            chunk.words()[4].set(8);
            chunk
        })
        .join()
        .unwrap();
        assert_eq!(chunk.words()[3].get(), 7);
        assert_eq!(chunk.words()[4].get(), 8);
    }
}
