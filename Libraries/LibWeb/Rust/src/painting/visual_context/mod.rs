/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The builder that keeps a document's visual context tree up to date, on top of the tree the
//! compositor shares.

pub mod basic_shapes;
pub mod box_build;
pub mod build;
pub mod delta;
pub mod dirty;
pub mod dump;
pub mod incremental;
pub mod node_values;
pub mod reconcile;
pub mod records;
pub mod refresh;
pub mod shape;

pub use dump::VisualContextTreeDump;
pub use libcompositing_rust::visual_context::*;
pub use records::*;
