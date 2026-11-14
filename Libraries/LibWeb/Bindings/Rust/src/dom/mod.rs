/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! DOM, https://dom.spec.whatwg.org/

#[cfg(not(test))]
pub(crate) mod abort_controller;
#[cfg(not(test))]
pub(crate) mod abort_signal;
#[cfg(not(test))]
pub(crate) mod document;
#[cfg(not(test))]
pub(crate) mod element;
pub(crate) mod event;
