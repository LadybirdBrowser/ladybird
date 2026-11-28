/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The DOM, whose node and element state this crate defines the layout of. C++ embeds the structures in its DOM
//! objects, which include them from the generated header LibWeb/DOM/RustFFI.h.

pub mod element;
pub mod element_core;
pub mod hooks;
pub mod links;
pub mod node;
pub mod node_core;
pub mod node_ref;
pub mod pre_insert;
pub mod qualified_name;
pub mod remove;
pub mod strings;
#[cfg(test)]
pub(crate) mod test_tree;
mod tree;
