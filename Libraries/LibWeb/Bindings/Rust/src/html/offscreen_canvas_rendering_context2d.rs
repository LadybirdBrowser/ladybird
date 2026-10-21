/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#offscreencanvasrenderingcontext2d

use crate::canvas_2d::context::{Offscreen, RenderingContext2D};

pub(crate) type OffscreenCanvasRenderingContext2D = RenderingContext2D<Offscreen>;
