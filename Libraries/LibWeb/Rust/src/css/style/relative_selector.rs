/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Relational selector query descriptors and the candidate witnesses along their axes.
//!
//! A relative selector matches a witness along one axis from an anchor, and matching enumerates
//! the candidates along that axis.

/// The single axis a simple relative selector traverses from its anchor.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RelativeAxis {
    /// `:has(.error)`
    Descendant,
    /// `:has(> .item)`
    Child,
    /// `:has(+ .selected)`
    NextSibling,
    /// `:has(~ .selected)`
    FollowingSibling,
    /// `:has(+ .row .cell)`: the witness is somewhere under the anchor's next sibling.
    NextSiblingSubtree,
    /// `:has(~ .row .cell)`: the witness is somewhere under one of the following siblings.
    FollowingSiblingSubtree,
}
