/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Streams Standard, https://streams.spec.whatwg.org/

#[cfg(not(test))]
pub(crate) mod abstract_operations;
#[cfg(not(test))]
pub(crate) mod algorithms;
#[cfg(not(test))]
pub(crate) mod byte_length_queuing_strategy;
#[cfg(not(test))]
pub(crate) mod count_queuing_strategy;
#[cfg(not(test))]
pub(crate) mod cpp;
#[cfg(not(test))]
pub(crate) mod cross_realm_transform;
#[cfg(not(test))]
pub(crate) mod exports;
#[cfg(not(test))]
pub(crate) mod generic_transform_stream;
#[cfg(not(test))]
pub(crate) mod queue_with_sizes;
#[cfg(not(test))]
pub(crate) mod queuing_strategy;
pub(crate) mod queuing_strategy_init;
#[cfg(not(test))]
pub(crate) mod read_request;
#[cfg(not(test))]
pub(crate) mod readable_byte_stream_controller;
#[cfg(not(test))]
pub(crate) mod readable_stream;
#[cfg(not(test))]
pub(crate) mod readable_stream_byob_reader;
#[cfg(not(test))]
pub(crate) mod readable_stream_byob_request;
#[cfg(not(test))]
pub(crate) mod readable_stream_default_controller;
#[cfg(not(test))]
pub(crate) mod readable_stream_default_reader;
#[cfg(not(test))]
pub(crate) mod readable_stream_generic_reader;
#[cfg(not(test))]
pub(crate) mod readable_stream_pipe_to;
#[cfg(not(test))]
pub(crate) mod readable_stream_tee;
#[cfg(not(test))]
pub(crate) mod transform_stream;
#[cfg(not(test))]
pub(crate) mod transform_stream_default_controller;
#[cfg(not(test))]
pub(crate) mod transformer;
#[cfg(not(test))]
pub(crate) mod underlying_sink;
#[cfg(not(test))]
pub(crate) mod underlying_source;
#[cfg(not(test))]
pub(crate) mod writable_stream;
#[cfg(not(test))]
pub(crate) mod writable_stream_default_controller;
#[cfg(not(test))]
pub(crate) mod writable_stream_default_writer;
