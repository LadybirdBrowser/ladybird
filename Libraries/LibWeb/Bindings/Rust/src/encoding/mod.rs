/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Encoding Standard, https://encoding.spec.whatwg.org/

#[cfg(not(test))]
pub(crate) mod text_decoder;
#[cfg(not(test))]
pub(crate) mod text_decoder_stream;
#[cfg(not(test))]
pub(crate) mod text_encoder;
#[cfg(not(test))]
pub(crate) mod text_encoder_stream;
pub(crate) mod utf8;
