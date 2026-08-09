/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// The result of consulting an optional physical materialization.
///
/// A known absence is a semantic answer. A missing result names the coverage gap that prevents an
/// answer, so eviction can never silently turn unknown state into an empty relation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Lookup<T, Gap> {
    Known(T),
    KnownAbsent,
    Missing(Gap),
}

impl<T, Gap> Lookup<T, Gap> {
    /// Convert a sparse lookup, whose materialization never represents known absence.
    pub(super) fn sparse(self) -> Result<T, Gap> {
        match self {
            Self::Known(value) => Ok(value),
            Self::KnownAbsent => unreachable!("a sparse lookup cannot be known absent"),
            Self::Missing(gap) => Err(gap),
        }
    }
}
