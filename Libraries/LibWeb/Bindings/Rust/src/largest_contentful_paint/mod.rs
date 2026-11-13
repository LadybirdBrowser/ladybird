/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Largest Contentful Paint, https://w3c.github.io/largest-contentful-paint/

#[allow(
    clippy::module_inception,
    reason = "the module mirrors the specification's LargestContentfulPaint interface, which the crate module is named after"
)]
pub(crate) mod largest_contentful_paint;
