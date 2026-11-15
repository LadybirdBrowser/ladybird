/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Cookie Store API, https://cookiestore.spec.whatwg.org/

pub(crate) mod cookie_change_event;
#[allow(
    clippy::module_inception,
    reason = "the module mirrors the CookieStore interface file"
)]
pub(crate) mod cookie_store;
pub(crate) mod exports;
