/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The DOM, whose node and element state this crate defines the layout of. C++ embeds the structures in its DOM
//! objects, which include them from the generated header LibWeb/DOM/RustFFI.h.

pub mod adopt;
pub mod attributes;
pub mod character_data;
pub mod child_index;
pub mod collection;
pub mod dispatch;
pub mod element;
pub mod element_core;
pub mod event;
pub mod event_target;
pub mod hooks;
pub mod insert;
pub mod links;
pub mod move_node;
pub mod mutation_observers;
pub mod node;
pub mod node_core;
pub mod node_ref;
pub mod pre_insert;
pub mod qualified_name;
pub mod range;
pub mod remove;
pub mod replace;
mod steps;
pub mod strings;
#[cfg(test)]
pub(crate) mod test_tree;
pub mod traversal;
mod tree;
pub mod tree_version;
pub mod weak_cells;
