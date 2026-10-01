/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Rust side of LibGC: the cells the runtime defines are allocated from, marked by and swept by the C++ heap
//! through the C interface in Libraries/LibGC/CAPI.h.

pub mod capi;
pub mod class;
pub mod class_id;
pub mod gc_ref_cell;
pub mod heap;
pub mod interpreter_buffer;
pub mod root;
pub mod visitor;
pub mod weak;
