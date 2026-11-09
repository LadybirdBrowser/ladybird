/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// A bytecode offset with the line and column that the source map says it came from.
#[derive(Debug, Clone, Copy)]
pub struct SourceMapEntry {
    pub bytecode_offset: u32,
    pub line: u32,
    pub column: u32,
}

/// An entry of the source map of an executable: where in the source the instructions from a bytecode offset on came from.
#[derive(Debug, Clone, Copy)]
pub struct SourceOffsetEntry {
    pub bytecode_offset: u32,
    pub source_offset: SourceOffset,
}

/// Where an instruction came from: an offset into the source, which only turns into a line and a column for what ends
/// up in the source map.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SourceOffset(pub u32);

impl SourceOffset {
    /// An instruction that no code in the source stands for.
    pub const NONE: Self = Self(u32::MAX);
}

/// How many entries of a source map there are from one checkpoint to the next.
const ENTRIES_PER_CHECKPOINT: usize = 64;

/// The size of a checkpoint: the bytecode offset of its entry, the last source offset before that entry, and where the
/// entry starts after the first number, each as four bytes.
const CHECKPOINT_SIZE: usize = 12;

/// Packs the `entry_count` entries of a source map, in the order of their bytecode offsets, into `out`. An entry is how
/// far its bytecode offset is from the entry before it, and then how far its source offset is from the last source
/// offset there was, both as variable-length integers. A zero in place of the second stands for `SourceOffset::NONE`.
///
/// A source map with more than `ENTRIES_PER_CHECKPOINT` entries ends with a table of checkpoints, one for every
/// `ENTRIES_PER_CHECKPOINT` entries after the first, so that a lookup decodes only the entries after the closest
/// checkpoint. The lowest bit of the first number says if there is a table, and then the number of checkpoints comes
/// right after it.
pub fn encode_source_map(entry_count: usize, entries: impl Iterator<Item = SourceOffsetEntry>, out: &mut Vec<u8>) {
    let checkpoint_count = entry_count.saturating_sub(1) / ENTRIES_PER_CHECKPOINT;
    let mut entries = entries.peekable();
    let Some(first) = entries.peek() else {
        return;
    };
    put_variable_length(
        out,
        u64::from(first.bytecode_offset) << 1 | u64::from(checkpoint_count != 0),
    );
    if checkpoint_count != 0 {
        put_variable_length(out, checkpoint_count as u64);
    }
    let start = out.len();
    let mut checkpoints = Vec::with_capacity(checkpoint_count);
    let mut previous_bytecode_offset = 0;
    let mut previous_source_offset = 0;
    for (index, entry) in entries.enumerate() {
        if index != 0 {
            if index % ENTRIES_PER_CHECKPOINT == 0 {
                checkpoints.push([
                    entry.bytecode_offset,
                    previous_source_offset,
                    (out.len() - start) as u32,
                ]);
            }
            put_variable_length(out, u64::from(entry.bytecode_offset - previous_bytecode_offset));
        }
        previous_bytecode_offset = entry.bytecode_offset;
        if entry.source_offset == SourceOffset::NONE {
            put_variable_length(out, 0);
            continue;
        }
        let distance = entry.source_offset.0.wrapping_sub(previous_source_offset) as i32;
        previous_source_offset = entry.source_offset.0;
        put_variable_length(out, u64::from(((distance << 1) ^ (distance >> 31)) as u32) + 1);
    }
    assert_eq!(checkpoints.len(), checkpoint_count, "the entry count is right");
    for checkpoint in checkpoints {
        for field in checkpoint {
            out.extend_from_slice(&field.to_le_bytes());
        }
    }
}

fn put_variable_length(out: &mut Vec<u8>, mut value: u64) {
    while value >= 0x80 {
        out.push(value as u8 | 0x80);
        value >>= 7;
    }
    out.push(value as u8);
}

fn take_variable_length(bytes: &mut &[u8]) -> u64 {
    let mut value = 0;
    let mut shift = 0;
    loop {
        let (&byte, rest) = bytes.split_first().expect("a source map ends after a whole entry");
        *bytes = rest;
        value |= u64::from(byte & 0x7f) << shift;
        if byte < 0x80 {
            return value;
        }
        shift += 7;
    }
}

/// The entries of a source map that `encode_source_map` packed.
pub struct SourceMapEntries<'a> {
    checkpoints: &'a [u8],
    entries: &'a [u8],
    bytes: &'a [u8],
    bytecode_offset: u32,
    source_offset: u32,
    /// How far the next entry is from the one before it, if that is not in `bytes`.
    next_distance: Option<u32>,
}

impl<'a> SourceMapEntries<'a> {
    pub fn new(mut bytes: &'a [u8]) -> Self {
        let mut next_distance = None;
        let mut checkpoints: &[u8] = &[];
        if !bytes.is_empty() {
            let first = take_variable_length(&mut bytes);
            next_distance = Some((first >> 1) as u32);
            if first & 1 != 0 {
                let checkpoint_count = take_variable_length(&mut bytes) as usize;
                let (entries, table) = bytes.split_at(bytes.len() - checkpoint_count * CHECKPOINT_SIZE);
                bytes = entries;
                checkpoints = table;
            }
        }
        Self {
            checkpoints,
            entries: bytes,
            bytes,
            bytecode_offset: 0,
            source_offset: 0,
            next_distance,
        }
    }

    fn checkpoint_field(&self, index: usize, field: usize) -> u32 {
        let at = index * CHECKPOINT_SIZE + field * 4;
        u32::from_le_bytes(self.checkpoints[at..at + 4].try_into().expect("four bytes"))
    }

    /// The last entry at or before the bytecode offset `offset`.
    pub fn entry_at(mut self, offset: u32) -> Option<SourceOffsetEntry> {
        let checkpoint_count = self.checkpoints.len() / CHECKPOINT_SIZE;
        let mut low = 0;
        let mut high = checkpoint_count;
        while low < high {
            let middle = (low + high) / 2;
            if self.checkpoint_field(middle, 0) <= offset {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        if low != 0 {
            let checkpoint = low - 1;
            self.bytes = &self.entries[self.checkpoint_field(checkpoint, 2) as usize..];
            take_variable_length(&mut self.bytes);
            self.bytecode_offset = 0;
            self.next_distance = Some(self.checkpoint_field(checkpoint, 0));
            self.source_offset = self.checkpoint_field(checkpoint, 1);
        }
        self.take_while(|entry| entry.bytecode_offset <= offset).last()
    }
}

impl Iterator for SourceMapEntries<'_> {
    type Item = SourceOffsetEntry;

    fn next(&mut self) -> Option<SourceOffsetEntry> {
        if self.bytes.is_empty() {
            return None;
        }
        let distance = match self.next_distance.take() {
            Some(distance) => distance,
            None => take_variable_length(&mut self.bytes) as u32,
        };
        self.bytecode_offset += distance;
        let packed = take_variable_length(&mut self.bytes);
        if packed == 0 {
            return Some(SourceOffsetEntry {
                bytecode_offset: self.bytecode_offset,
                source_offset: SourceOffset::NONE,
            });
        }
        let zigzag = (packed - 1) as u32;
        let distance = (zigzag >> 1) as i32 ^ -((zigzag & 1) as i32);
        self.source_offset = self.source_offset.wrapping_add(distance as u32);
        Some(SourceOffsetEntry {
            bytecode_offset: self.bytecode_offset,
            source_offset: SourceOffset(self.source_offset),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn packed_entries_come_back_the_same() {
        let entries = [
            (0, 500),
            (3, u32::MAX),
            (3, 12),
            (9, 3_000_000_000),
            (70_000, 0),
            (70_001, u32::MAX),
            (70_100, 1),
        ];
        let mut bytes = Vec::new();
        encode_source_map(
            entries.len(),
            entries
                .iter()
                .map(|&(bytecode_offset, source_offset)| SourceOffsetEntry {
                    bytecode_offset,
                    source_offset: SourceOffset(source_offset),
                }),
            &mut bytes,
        );
        let decoded: Vec<_> = SourceMapEntries::new(&bytes)
            .map(|entry| (entry.bytecode_offset, entry.source_offset.0))
            .collect();
        assert_eq!(decoded, entries);
        assert_eq!(SourceMapEntries::new(&bytes).entry_at(8).unwrap().source_offset.0, 12);
    }

    #[test]
    fn lookups_from_checkpoints_match_a_scan_from_the_start() {
        let entries: Vec<_> = (0..1000u32)
            .map(|index| SourceOffsetEntry {
                bytecode_offset: 5 + index * 3 - index % 2,
                source_offset: if index % 7 == 0 {
                    SourceOffset::NONE
                } else {
                    SourceOffset(index.wrapping_mul(2_654_435_761) % 100_000)
                },
            })
            .collect();
        let mut bytes = Vec::new();
        encode_source_map(entries.len(), entries.iter().copied(), &mut bytes);
        let decoded: Vec<_> = SourceMapEntries::new(&bytes)
            .map(|entry| (entry.bytecode_offset, entry.source_offset))
            .collect();
        let expected: Vec<_> = entries
            .iter()
            .map(|entry| (entry.bytecode_offset, entry.source_offset))
            .collect();
        assert_eq!(decoded, expected);
        for offset in 0..3000 {
            let scanned = entries
                .iter()
                .take_while(|entry| entry.bytecode_offset <= offset)
                .last();
            let looked_up = SourceMapEntries::new(&bytes).entry_at(offset);
            assert_eq!(
                looked_up.map(|entry| (entry.bytecode_offset, entry.source_offset)),
                scanned.map(|entry| (entry.bytecode_offset, entry.source_offset))
            );
        }
    }

    #[test]
    fn an_empty_source_map_has_no_entries() {
        let mut bytes = Vec::new();
        encode_source_map(0, std::iter::empty(), &mut bytes);
        assert!(bytes.is_empty());
        assert!(SourceMapEntries::new(&bytes).entry_at(0).is_none());
    }
}
