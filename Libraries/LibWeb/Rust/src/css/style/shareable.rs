/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What an evaluation step may read, written down as compile-time witnesses.
//!
//! An evaluation step borrows the engine's retained state and the update's committed answers, and
//! writes only this flush's scratch. For many workers to run the same step over one read side,
//! everything the step reads has to be shareable across threads and the per-worker scratch has to
//! be movable to one. None of this is a runtime check: a type that stops being shareable stops
//! compiling.

use super::*;

const _: () = {
    const fn assert_sync<T: Sync + ?Sized>() {}
    assert_sync::<RetainedState>();
};
