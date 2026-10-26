/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The constructs that script does not see: databases, object stores, indexes, keys, and their algorithms.

pub(crate) mod algorithms;
pub(crate) mod database;
pub(crate) mod function;
pub(crate) mod index;
pub(crate) mod key;
pub(crate) mod key_generator;
pub(crate) mod mutation_log;
pub(crate) mod object_store;
pub(crate) mod record_range;
pub(crate) mod request_list;
pub(crate) mod storage;
