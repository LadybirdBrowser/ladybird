/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// https://html.spec.whatwg.org/multipage/interaction.html#visibility-state
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum VisibilityState {
    Visible,
    Hidden,
}
