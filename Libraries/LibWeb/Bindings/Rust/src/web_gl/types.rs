/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.1

use libjs_rust::api::{Gc, TypedArrayBase};

pub(crate) enum Float32List {
    Float32Array(Gc<TypedArrayBase>),
    Sequence(Vec<f32>),
}
