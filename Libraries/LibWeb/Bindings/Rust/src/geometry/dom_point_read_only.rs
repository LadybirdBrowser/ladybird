/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The point dictionary of Geometry/DOMPointReadOnly.idl, https://drafts.fxtf.org/geometry/#dictdef-dompointinit

/// The dictionary DOMPointInit of Geometry/DOMPointReadOnly.idl, whose members the bindings convert.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct DOMPointInit {
    pub(crate) x: f64,
    pub(crate) y: f64,
    pub(crate) z: f64,
    pub(crate) w: f64,
}
