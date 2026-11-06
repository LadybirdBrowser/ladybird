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
