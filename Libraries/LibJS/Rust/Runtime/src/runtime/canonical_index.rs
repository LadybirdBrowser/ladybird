/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/CanonicalIndex.h: the result of CanonicalNumericIndexString.

/// Mirrors JS::CanonicalIndex::Type.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CanonicalIndexType {
    Index,
    Numeric,
    Undefined,
}

/// Mirrors JS::CanonicalIndex.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CanonicalIndex {
    index_type: CanonicalIndexType,
    index: u32,
}

impl CanonicalIndex {
    pub fn new(index_type: CanonicalIndexType, index: u32) -> Self {
        Self { index_type, index }
    }

    pub fn as_index(self) -> u32 {
        assert!(self.is_index());
        self.index
    }

    pub fn is_index(self) -> bool {
        self.index_type == CanonicalIndexType::Index
    }

    pub fn is_undefined(self) -> bool {
        self.index_type == CanonicalIndexType::Undefined
    }
}
