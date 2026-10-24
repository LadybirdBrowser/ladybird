/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Streams Standard, https://streams.spec.whatwg.org/

#[cfg(not(test))]
pub(crate) mod byte_length_queuing_strategy;
#[cfg(not(test))]
pub(crate) mod count_queuing_strategy;
#[cfg(not(test))]
pub(crate) mod generic_transform_stream;
pub(crate) mod queuing_strategy_init;
