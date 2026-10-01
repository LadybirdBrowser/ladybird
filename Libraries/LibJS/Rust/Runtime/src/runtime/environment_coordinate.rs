/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub use crate::layout::property_lookup_cache::EnvironmentCoordinate;

impl EnvironmentCoordinate {
    /// The coordinate that refers to nothing, which an empty environment coordinate cache holds.
    pub const fn invalid() -> Self {
        Self {
            hops: Self::INVALID_MARKER,
            index: Self::INVALID_MARKER,
        }
    }

    pub const fn is_valid(&self) -> bool {
        self.hops != Self::INVALID_MARKER && self.index != Self::INVALID_MARKER
    }
}

impl Default for EnvironmentCoordinate {
    fn default() -> Self {
        Self::invalid()
    }
}
