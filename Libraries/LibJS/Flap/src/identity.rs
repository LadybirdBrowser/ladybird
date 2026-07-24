/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiler-wide identities for source declarations.

use std::fmt;

/// A link-time symbol referenced by generated machine code.
#[derive(Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct ExternalSymbol(String);

impl ExternalSymbol {
    pub(crate) fn new(name: impl Into<String>) -> Self {
        Self(name.into())
    }

    pub(crate) fn as_str(&self) -> &str {
        &self.0
    }
}

impl fmt::Debug for ExternalSymbol {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

impl fmt::Display for ExternalSymbol {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct InlineFunctionId(u32);

impl InlineFunctionId {
    pub(crate) fn new(index: usize) -> Self {
        Self(u32::try_from(index).expect("Flap inline function count fits in u32"))
    }

    pub(crate) fn index(self) -> usize {
        self.0 as usize
    }
}

/// The stable identity of an interpreter handler within a prepared program.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct HandlerId(u32);

impl HandlerId {
    pub(crate) fn new(index: usize) -> Self {
        Self(u32::try_from(index).expect("Flap handler count fits in u32"))
    }

    pub(crate) fn index(self) -> usize {
        self.0 as usize
    }
}
