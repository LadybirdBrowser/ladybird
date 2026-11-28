/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The DOM, whose node state this crate defines the layout of. C++ embeds the structures in its DOM objects, which
//! include them from the generated header LibWeb/DOM/RustFFI.h.

pub mod node;
pub mod node_core;
pub mod pre_insert;
#[cfg(test)]
pub(crate) mod test_tree;
mod tree;
