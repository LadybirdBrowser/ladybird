/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The layout engine sources compose into one flat namespace: the files below
// are include!d rather than declared as submodules, and the re-exports here
// make the css-module types they use resolve without per-file imports.
pub(crate) use crate::abort_on_panic;
pub(crate) use crate::css::computed_value_types::*;
pub(crate) use crate::css::computed_value_views::*;
pub(crate) use crate::css::css_enums::*;
pub(crate) use crate::css::css_pixels::*;
pub(crate) use crate::css::display::*;

include!("node_facts.rs");
include!("fragment_tree.rs");

include!("formatting_context.rs");
include!("commit.rs");
include!("sizing_context.rs");
include!("abspos_inputs.rs");
include!("abspos_engine.rs");
include!("block_formatting_context.rs");
include!("flex_formatting_context.rs");
include!("grid_formatting_context.rs");
include!("svg_formatting_context.rs");
include!("inline_level_iterator.rs");
include!("line_box.rs");
include!("line_box_fragment.rs");
include!("line_builder.rs");
include!("inline_formatting_context.rs");
include!("font.rs");
include!("text_chunker.rs");
include!("replaced_with_children_formatting_context.rs");
include!("table_formatting_context.rs");
include!("geometry.rs");
include!("fc_run_cache.rs");
mod layout_node_arena;
pub mod node_data;
mod tree_mutation;
include!("run_records.rs");
include!("style_values.rs");

mod tree_builder;

include!("used_values.rs");

use crate::layout::layout_node_arena::IntrinsicBlockSizeMeasurement;
use crate::layout::layout_node_arena::IntrinsicInlineSizeMeasurement;
use crate::layout::layout_node_arena::IntrinsicSizeCacheKey;
use crate::layout::layout_node_arena::IntrinsicSizeCacheKind;
pub(crate) use crate::layout::layout_node_arena::{LayoutNodeArena, RenderedTextBoundary};
use crate::layout::layout_node_arena::{TableCellMeasurement, TableCellMeasurementKey};
pub use crate::layout::node_data::FfiReplacedContentFacts;
pub use crate::layout::node_data::FfiStylePayloads;
use crate::layout::node_data::NodeData;
use crate::layout::node_data::NodeFlag;
use crate::layout::node_data::NodeKind;
use crate::layout::node_data::NodeSlotId;
pub use crate::layout::node_data::STYLE_GROUP_COUNT;
use std::cell::Cell;
use std::cell::OnceCell;
use std::cell::Ref;
use std::cell::RefCell;
use std::cell::RefMut;
use std::collections::HashMap;
use std::collections::HashSet;
use std::ffi::c_void;
