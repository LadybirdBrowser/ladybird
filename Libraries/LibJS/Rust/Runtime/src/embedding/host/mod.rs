/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Host objects: objects whose internal methods an embedder implements, described by the host class tables of
//! LibJS/HostObjectABI.h.

pub mod class_table;
pub mod console_client;
pub mod host_array;
pub mod host_function;
pub mod host_module;
pub mod host_object;
pub mod registry;
