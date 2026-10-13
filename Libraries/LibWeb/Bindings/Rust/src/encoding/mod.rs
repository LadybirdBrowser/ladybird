/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Encoding Standard, https://encoding.spec.whatwg.org/

pub(crate) mod decoding;
#[cfg(not(test))]
pub(crate) mod text_decoder;
#[cfg(not(test))]
pub(crate) mod text_encoder;
pub(crate) mod utf8;
