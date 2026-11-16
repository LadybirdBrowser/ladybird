/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The window a recording is made for, and where the parts of the paint order lie against it (see [`RecordedWindow`]).
//!
//! A recording holds what shows in the viewport and for a margin around it, with every scroller where it is, instead
//! of the whole document. The window stays where it was placed while it still covers what the viewport shows with half
//! the margin to spare, so scrolling a little changes nothing a recording reads. Once it does not, the next recording
//! places the window again: what came into it is recorded, and what left it is dropped.

use crate::css::css_pixels::CssPixelPoint;
use crate::painting::display_list::commands::{ClipMode, ContextRef, SpatialNodeIndex};
use crate::painting::visual_context::{ClipNodeData, ScrollData, SpatialData, VisualContextTree};
use libgfx_rust::{FloatPoint, FloatRect, FloatSize, IntSize};
use smallvec::SmallVec;
use std::sync::Arc;

pub(crate) use libcompositing_rust::display_list::window::{PlacedSpaces, RecordedWindow};

/// A bound keeps the rects of this many spaces apart before it gives up on bounding what it holds.
const MOST_SPACES_A_BOUND_KEEPS_APART: usize = 4;

/// Bounds what a part of a plan paints: rects in device pixels, each in the space of a node of the visual context
/// tree. What lies in one space is bounded by one rect.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct InkBound {
    rects: SmallVec<[(SpatialNodeIndex, FloatRect); 2]>,
}

impl InkBound {
    /// What bounds an item that paints nothing.
    pub(crate) fn nothing() -> Self {
        Self { rects: SmallVec::new() }
    }

    pub(crate) fn in_space(space: SpatialNodeIndex, rect: FloatRect) -> Self {
        Self {
            rects: smallvec::smallvec![(space, rect)],
        }
    }

    /// The rect of each space the bound has things in.
    #[cfg(test)]
    pub(crate) fn rects(&self) -> &[(SpatialNodeIndex, FloatRect)] {
        &self.rects
    }

    /// One bound for what both bound, or none where they lie in more spaces than a bound keeps apart.
    pub(crate) fn united(mut self, other: &InkBound) -> Option<Self> {
        for &(space, rect) in &other.rects {
            if let Some((_, kept)) = self.rects.iter_mut().find(|(kept, _)| *kept == space) {
                *kept = kept.united(rect);
            } else if self.rects.len() < MOST_SPACES_A_BOUND_KEEPS_APART {
                self.rects.push((space, rect));
            } else {
                return None;
            }
        }
        Some(self)
    }
}

/// How far each scroller of `tree` had moved what it holds, as a window keeps it: in device pixels by the index of its
/// node, from `scroll_own_offsets`, by scroll state slot.
pub(crate) fn device_pixel_offsets(
    tree: &VisualContextTree,
    scroll_own_offsets: &[CssPixelPoint],
    device_pixels_per_css_pixel: f64,
) -> Arc<[FloatPoint]> {
    let offset_of = scroll_offset_of_node(scroll_own_offsets, device_pixels_per_css_pixel);
    tree.spatial_nodes
        .iter()
        .enumerate()
        .map(|(index, node)| match &node.data {
            SpatialData::Scroll(scroll) => offset_of(SpatialNodeIndex(index as u32), scroll),
            _ => FloatPoint::default(),
        })
        .collect()
}

/// How far a scroller moves what it holds, in device pixels, from `scroll_own_offsets`, by scroll state slot.
fn scroll_offset_of_node(
    scroll_own_offsets: &[CssPixelPoint],
    device_pixels_per_css_pixel: f64,
) -> impl Fn(SpatialNodeIndex, &ScrollData) -> FloatPoint + '_ {
    let scale = device_pixels_per_css_pixel as f32;
    move |_, scroll| {
        scroll_own_offsets
            .get(scroll.state_slot)
            .map_or_else(FloatPoint::default, |offset| FloatPoint {
                x: offset.x.to_float() * scale,
                y: offset.y.to_float() * scale,
            })
    }
}

/// What a window keeps ahead of what the viewport shows before a recording places it again.
pub(crate) fn kept_ahead(window: &RecordedWindow) -> FloatSize {
    let margin = window.margin();
    FloatSize {
        width: margin.width / 2.0,
        height: margin.height / 2.0,
    }
}

/// Whether `window`, of a recording made against `tree_of_recording`, still covers what a viewport of
/// `device_viewport_size` shows of `tree_now` with its scrollers at `scroll_own_offsets`, half the margin to spare.
pub(crate) fn still_covers(
    window: &RecordedWindow,
    tree_of_recording: &VisualContextTree,
    tree_now: &VisualContextTree,
    device_viewport_size: IntSize,
    device_pixels_per_css_pixel: f64,
    scroll_own_offsets: &[CssPixelPoint],
) -> bool {
    window.still_covers(
        tree_of_recording,
        tree_now,
        device_viewport_size,
        device_pixels_per_css_pixel,
        scroll_offset_of_node(scroll_own_offsets, device_pixels_per_css_pixel),
        kept_ahead(window),
    )
}

/// The window a recording of a frame of `tree` is made for: the window of `published`, the recording published last
/// with the tree it was made against, where that still covers what the viewport shows with the scrollers at
/// `scroll_own_offsets`, or else one placed there.
pub(crate) fn window_of_frame(
    published: Option<(&Arc<RecordedWindow>, &VisualContextTree)>,
    tree: &VisualContextTree,
    device_viewport_size: IntSize,
    device_pixels_per_css_pixel: f64,
    scroll_own_offsets: &[CssPixelPoint],
) -> Arc<RecordedWindow> {
    if let Some((published, tree_of_recording)) = published
        && still_covers(
            published,
            tree_of_recording,
            tree,
            device_viewport_size,
            device_pixels_per_css_pixel,
            scroll_own_offsets,
        )
    {
        return Arc::clone(published);
    }
    Arc::new(RecordedWindow::placed_at(
        device_viewport_size,
        device_pixels_per_css_pixel,
        device_pixel_offsets(tree, scroll_own_offsets, device_pixels_per_css_pixel),
    ))
}

/// The spaces of a frame's visual context tree against the window of its recording.
pub(crate) struct WindowSpaces<'a> {
    window: Arc<RecordedWindow>,
    spaces: PlacedSpaces<'a>,
}

impl<'a> WindowSpaces<'a> {
    pub(crate) fn new(window: Arc<RecordedWindow>, tree: &'a VisualContextTree) -> Self {
        let spaces = window.placed_spaces(tree);
        Self { window, spaces }
    }

    /// The rects of the bound in the space of the visual viewport, or none where one lies in a space that moves
    /// without a recording.
    fn rects_in_root(&self, bound: &InkBound) -> Option<SmallVec<[FloatRect; 2]>> {
        bound
            .rects
            .iter()
            .map(|&(space, rect)| self.spaces.to_root(space, rect))
            .collect()
    }

    /// Whether nothing inside the bound shows in the window. A bound that cannot be placed is not outside it.
    pub(crate) fn is_outside(&self, bound: &InkBound) -> bool {
        // What paints nothing is as cheap to keep as to leave out.
        if bound.rects.is_empty() {
            return false;
        }
        // One device pixel of slack keeps rounding from leaving out what touches the window's edge.
        self.rects_in_root(bound).is_some_and(|rects| {
            rects
                .iter()
                .all(|rect| rect.inflated(2.0, 2.0).intersected(self.window.rect).is_empty())
        })
    }

    /// Whether everything inside the bound shows in the window.
    pub(crate) fn is_wholly_inside(&self, bound: &InkBound) -> bool {
        self.rects_in_root(bound).is_some_and(|rects| {
            rects
                .iter()
                .all(|rect| self.window.rect.contains_rect(rect.inflated(2.0, 2.0)))
        })
    }

    /// Whether what a command draws in `rect` under `context` shows in the window as the tree stands, with the
    /// scrollers where the window was placed and the clips of the context applied, or none where a node on the way to
    /// the root moves what it holds without a recording. A recording of a window that lacks the command was right to
    /// leave it out where this is false.
    pub(crate) fn shows_in_window(&self, rect: FloatRect, context: ContextRef) -> Option<bool> {
        let tree = self.spaces.tree();
        let mut shown = self.spaces.to_root(context.spatial, rect)?;
        let mut clip = context.clip;
        while !clip.is_none() {
            let clip_node = &tree.clip_nodes[clip.0 as usize];
            let clip_rect = match &clip_node.data {
                ClipNodeData::Rect(data) if data.mode == ClipMode::Intersect => Some(data.rect),
                ClipNodeData::Path(path) => Some(path.bounding_rect.to_float()),
                _ => None,
            };
            if let Some(clip_rect) = clip_rect
                && let Some(clip_in_root) = self.spaces.to_root(clip_node.spatial, clip_rect)
            {
                shown = shown.intersected(clip_in_root);
            }
            clip = clip_node.parent;
        }
        Some(!shown.intersected(self.window.rect).is_empty())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_pixels::CssPixels;
    use crate::layout::node_data::NodeSlotId;
    use crate::painting::visual_context::{
        ScrollData, SpatialData, StickyData, TransformData, TransformDataRole, VISUAL_VIEWPORT_NODE_INDEX,
    };
    use libgfx_rust::FloatMatrix4x4;

    fn transform(matrix: FloatMatrix4x4) -> SpatialData {
        SpatialData::Transform(TransformData {
            matrix,
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        })
    }

    fn scroller(state_slot: usize) -> SpatialData {
        SpatialData::Scroll(ScrollData {
            state_slot,
            owner_paintable: NodeSlotId::INVALID,
        })
    }

    fn empty_tree() -> VisualContextTree {
        let SpatialData::Transform(identity) = transform(FloatMatrix4x4::identity()) else {
            unreachable!()
        };
        VisualContextTree::create(identity)
    }

    fn offsets(offsets: &[(i64, i64)]) -> Vec<CssPixelPoint> {
        offsets
            .iter()
            .map(|(x, y)| CssPixelPoint::new(CssPixels::from_integer(*x), CssPixels::from_integer(*y)))
            .collect()
    }

    const VIEWPORT: IntSize = IntSize {
        width: 800,
        height: 600,
    };

    // A window of an 800 by 600 viewport, which reaches 1200 pixels above and below it.
    fn window_at(tree: &VisualContextTree, placed_offsets: &[(i64, i64)]) -> Arc<RecordedWindow> {
        window_of_frame(None, tree, VIEWPORT, 1.0, &offsets(placed_offsets))
    }

    fn bound(space: SpatialNodeIndex, y: f32) -> InkBound {
        InkBound::in_space(space, FloatRect::new(0.0, y, 100.0, 100.0))
    }

    #[test]
    fn a_frame_keeps_the_published_window_while_it_covers_what_shows() {
        let mut tree = empty_tree();
        tree.append_spatial(scroller(0), VISUAL_VIEWPORT_NODE_INDEX);
        let published = window_at(&tree, &[(0, -1000)]);
        let window_of =
            |placed: &[(i64, i64)]| window_of_frame(Some((&published, &tree)), &tree, VIEWPORT, 1.0, &offsets(placed));
        assert!(Arc::ptr_eq(&window_of(&[(0, -1600)]), &published));
        assert!(!Arc::ptr_eq(&window_of(&[(0, -1601)]), &published));
        assert_eq!(*window_of(&[(0, -1601)]), *window_at(&tree, &[(0, -1601)]));
    }

    #[test]
    fn what_a_scroller_holds_is_placed_where_the_scroller_was_when_the_window_was_placed() {
        let mut tree = empty_tree();
        let page = tree.append_spatial(scroller(0), VISUAL_VIEWPORT_NODE_INDEX);
        // The page is scrolled down by 5000.
        let spaces = WindowSpaces::new(window_at(&tree, &[(0, -5000)]), &tree);

        // The window reaches from 3800 to 6800 down the page.
        assert!(spaces.is_outside(&bound(page, 0.0)));
        assert!(spaces.is_outside(&bound(page, 3600.0)));
        assert!(!spaces.is_outside(&bound(page, 3750.0)));
        assert!(spaces.is_wholly_inside(&bound(page, 5000.0)));
        assert!(!spaces.is_wholly_inside(&bound(page, 3750.0)));
        assert!(spaces.is_outside(&bound(page, 6900.0)));
        // What the page does not scroll stays where the viewport is.
        assert!(!spaces.is_outside(&bound(VISUAL_VIEWPORT_NODE_INDEX, 0.0)));
        assert!(spaces.is_outside(&bound(VISUAL_VIEWPORT_NODE_INDEX, 5000.0)));
    }

    #[test]
    fn what_a_static_transform_moves_is_placed_where_it_moves_it() {
        let mut tree = empty_tree();
        let page = tree.append_spatial(scroller(0), VISUAL_VIEWPORT_NODE_INDEX);
        let pulled = tree.append_spatial(transform(libgfx_rust::translation_matrix(0.0, -4900.0, 0.0)), page);
        let spaces = WindowSpaces::new(window_at(&tree, &[(0, 0)]), &tree);
        assert!(!spaces.is_outside(&bound(pulled, 5000.0)));
        assert!(spaces.is_outside(&bound(page, 5000.0)));
    }

    #[test]
    fn nothing_below_a_node_that_moves_on_its_own_is_outside() {
        let mut tree = empty_tree();
        let page = tree.append_spatial(scroller(0), VISUAL_VIEWPORT_NODE_INDEX);
        let sticky = tree.append_spatial(
            SpatialData::Sticky(StickyData::unconstrained(page, None, 1, NodeSlotId::INVALID)),
            page,
        );
        let spaces = WindowSpaces::new(window_at(&tree, &[(0, 0), (0, 0)]), &tree);
        assert!(!spaces.is_outside(&bound(sticky, 100_000.0)));
        assert!(!spaces.is_wholly_inside(&bound(sticky, 0.0)));
        // A bound with a part that cannot be placed is not outside, wherever its other parts are.
        let mixed = bound(page, 100_000.0).united(&bound(sticky, 100_000.0)).unwrap();
        assert!(!spaces.is_outside(&mixed));
    }

    #[test]
    fn a_bound_keeps_the_rects_of_a_few_spaces_apart() {
        let space = |index| SpatialNodeIndex(index);
        let united = bound(space(1), 0.0).united(&bound(space(1), 500.0)).unwrap();
        assert_eq!(
            united,
            InkBound::in_space(space(1), FloatRect::new(0.0, 0.0, 100.0, 600.0))
        );
        let mut many = bound(space(1), 0.0);
        for index in 2..=MOST_SPACES_A_BOUND_KEEPS_APART as u32 {
            many = many.united(&bound(space(index), 0.0)).unwrap();
        }
        assert!(many.clone().united(&bound(space(1), 10.0)).is_some());
        assert!(many.united(&bound(space(99), 0.0)).is_none());
    }
}
