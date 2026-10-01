/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The data of every structure that the interpreter reads or writes directly, and of every structure that embeds
//! one. build.rs includes this module as well to compute the interpreter layout, so it must not name anything in the
//! rest of the crate except through `crate::layout_forward`, which build.rs replaces with stand-ins of the same size.

pub mod accessor;
pub mod buffer;
pub mod cell;
pub mod environment;
pub mod executable;
pub mod execution_context;
pub mod function_object;
pub mod object;
pub mod primitive_string;
pub mod property_lookup_cache;
pub mod realm;
pub mod shape;
pub mod value;
pub mod vm;
