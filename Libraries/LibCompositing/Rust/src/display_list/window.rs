/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The part of a document a recording of a window holds, and whether it still holds what a viewport shows.
//!
//! A recording of a window holds what shows in a rect around the viewport, with every scroller where it was when the
//! window was placed. Whether part of the document shows there is decided in the space of the visual viewport: a rect
//! in the space of a spatial node is taken there through the tree, with the scrollers where they were. That holds for
//! every node that moves only when the document is recorded again. A node that moves on its own, as a sticky box, an
//! anchor's shift, a perspective or a transform the compositor animates does, holds what is below it wherever it is,
//! so nothing below it is left out.

use crate::display_list::commands::SpatialNodeIndex;
use crate::host::FfiVisualAnimationTargetKind;
use crate::ipc_types::Compositing_DisplayListWindow;
use crate::visual_context::{ScrollData, SpatialData, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree, WorldTransforms};
use libgfx_rust::{AffineTransform, FloatPoint, FloatRect, FloatSize, IntSize, multiply_affine};
use std::sync::Arc;

/// How far a window reaches past the viewport on each side, in viewports.
const MARGIN_IN_VIEWPORTS: f32 = 2.0;
/// The margin of a viewport smaller than this is the margin of one this size, in CSS pixels.
const SMALLEST_VIEWPORT_THE_MARGIN_FOLLOWS: f32 = 500.0;

#[derive(Clone, Debug, PartialEq)]
pub struct RecordedWindow {
    /// The viewport with the margin around it, in device pixels in the space of the visual viewport.
    pub rect: FloatRect,
    pub device_viewport_size: IntSize,
    pub device_pixels_per_css_pixel: f64,
    /// How far each scroller had moved what it holds when the window was placed, in device pixels, by the index of its
    /// node in the visual context tree the window was placed in. Only a tree of the same structure is ever placed
    /// against the window, which names its nodes by the same indices in every process.
    pub scroll_offsets: Arc<[FloatPoint]>,
}

impl RecordedWindow {
    /// The window around a viewport of `device_viewport_size` with the scrollers at `scroll_offsets`.
    pub fn placed_at(
        device_viewport_size: IntSize,
        device_pixels_per_css_pixel: f64,
        scroll_offsets: Arc<[FloatPoint]>,
    ) -> Self {
        let margin = Self::margin_of(device_viewport_size, device_pixels_per_css_pixel);
        Self {
            rect: FloatRect::new(
                -margin.width,
                -margin.height,
                device_viewport_size.width as f32 + 2.0 * margin.width,
                device_viewport_size.height as f32 + 2.0 * margin.height,
            ),
            device_viewport_size,
            device_pixels_per_css_pixel,
            scroll_offsets,
        }
    }

    fn margin_of(device_viewport_size: IntSize, device_pixels_per_css_pixel: f64) -> FloatSize {
        let scale = device_pixels_per_css_pixel as f32;
        let margin = |device_extent: i32| {
            (device_extent as f32 / scale).max(SMALLEST_VIEWPORT_THE_MARGIN_FOLLOWS) * MARGIN_IN_VIEWPORTS * scale
        };
        FloatSize {
            width: margin(device_viewport_size.width),
            height: margin(device_viewport_size.height),
        }
    }

    /// How far the window reaches past the viewport on each side, in device pixels.
    pub fn margin(&self) -> FloatSize {
        Self::margin_of(self.device_viewport_size, self.device_pixels_per_css_pixel)
    }

    pub fn to_ipc(&self) -> Compositing_DisplayListWindow {
        Compositing_DisplayListWindow {
            rect: self.rect,
            device_viewport_size: self.device_viewport_size,
            device_pixels_per_css_pixel: self.device_pixels_per_css_pixel,
            scroll_offsets: self.scroll_offsets.to_vec(),
        }
    }

    /// The window a display list says it was recorded for, or none where no recording could have placed it.
    pub fn from_ipc(window: &Compositing_DisplayListWindow) -> Option<Self> {
        let rect = window.rect;
        let is_finite = |point: &FloatPoint| point.x.is_finite() && point.y.is_finite();
        let could_be_placed = [rect.x, rect.y, rect.width, rect.height]
            .iter()
            .all(|value| value.is_finite())
            && rect.width >= 0.0
            && rect.height >= 0.0
            && window.device_viewport_size.width > 0
            && window.device_viewport_size.height > 0
            && window.device_pixels_per_css_pixel.is_finite()
            && window.device_pixels_per_css_pixel > 0.0
            && window.scroll_offsets.iter().all(is_finite);
        could_be_placed.then(|| Self {
            rect,
            device_viewport_size: window.device_viewport_size,
            device_pixels_per_css_pixel: window.device_pixels_per_css_pixel,
            scroll_offsets: window.scroll_offsets.as_slice().into(),
        })
    }

    pub fn placed_offset_of(&self, scroll_node: SpatialNodeIndex) -> FloatPoint {
        self.scroll_offsets
            .get(scroll_node.0 as usize)
            .copied()
            .unwrap_or_default()
    }

    /// The spaces of `tree` with the scrollers where they were when the window was placed.
    pub fn placed_spaces<'a>(&self, tree: &'a VisualContextTree) -> PlacedSpaces<'a> {
        PlacedSpaces::new(tree, |node, _| self.placed_offset_of(node))
    }

    /// Whether the window of a recording made against `tree_of_recording` still holds what a viewport of
    /// `device_viewport_size` shows of `tree_now`, with each scroller moving what it holds by `live_scroll_offset` of
    /// its node, and `keep_ahead` more on each side. In each space content can be left out in, what
    /// shows of it now is taken back to where it was placed when the recording was made, through the scrollers and the
    /// transforms of both trees, and must lie inside the window.
    pub fn still_covers(
        &self,
        tree_of_recording: &VisualContextTree,
        tree_now: &VisualContextTree,
        device_viewport_size: IntSize,
        device_pixels_per_css_pixel: f64,
        live_scroll_offset: impl Fn(SpatialNodeIndex, &ScrollData) -> FloatPoint,
        keep_ahead: FloatSize,
    ) -> bool {
        if self.device_viewport_size != device_viewport_size
            || self.device_pixels_per_css_pixel != device_pixels_per_css_pixel
            || tree_of_recording.structural_epoch != tree_now.structural_epoch
            || tree_of_recording.spatial_nodes.len() != tree_now.spatial_nodes.len()
        {
            return false;
        }
        let shown = FloatRect::new(
            -keep_ahead.width,
            -keep_ahead.height,
            device_viewport_size.width as f32 + 2.0 * keep_ahead.width,
            device_viewport_size.height as f32 + 2.0 * keep_ahead.height,
        );
        let placed = self.placed_spaces(tree_of_recording);
        let live = PlacedSpaces::new(tree_now, live_scroll_offset);
        (0..tree_now.spatial_nodes.len()).all(|index| {
            let node = SpatialNodeIndex(index as u32);
            // The recording left nothing out below a node that moved on its own then.
            let Some(then) = placed.affine(node) else {
                return true;
            };
            // What a node that moves on its own now moves may come from anywhere the recording left out.
            let Some(now) = live.affine(node) else {
                return false;
            };
            // A space flattened to nothing shows nothing.
            let Some(now_inverse) = now.inverse() else {
                return true;
            };
            // Taken through both transforms at once: a box around what shows in a rotated space, taken back out of it,
            // would be larger than what shows, though the space did not move.
            self.rect
                .contains_rect(multiply_affine(then, now_inverse).map_rect(shown))
        })
    }
}

/// Whether content in each space of `first` that moves only when it is recorded again lies where it does in `second`:
/// both have the same nodes, and their transforms take the same values.
pub fn spaces_place_alike(first: &VisualContextTree, second: &VisualContextTree) -> bool {
    first.structural_epoch == second.structural_epoch
        && first.spatial_nodes.len() == second.spatial_nodes.len()
        && first
            .spatial_nodes
            .iter()
            .zip(&second.spatial_nodes)
            .all(|(first, second)| match (&first.data, &second.data) {
                (SpatialData::Transform(first), SpatialData::Transform(second)) => {
                    first.matrix == second.matrix && first.origin == second.origin
                }
                (first, second) => std::mem::discriminant(first) == std::mem::discriminant(second),
            })
}

/// The spaces of a tree, with its scrollers where a window was placed or where they are now.
pub struct PlacedSpaces<'a> {
    tree: &'a VisualContextTree,
    world: WorldTransforms,
    moves_without_a_recording: Vec<bool>,
}

impl<'a> PlacedSpaces<'a> {
    /// The spaces of `tree` with each scroller moving what it holds by `scroll_offset` of its node, in device pixels.
    pub fn new(
        tree: &'a VisualContextTree,
        scroll_offset: impl Fn(SpatialNodeIndex, &ScrollData) -> FloatPoint,
    ) -> Self {
        let scroll_offsets: Vec<FloatPoint> = tree
            .spatial_nodes
            .iter()
            .enumerate()
            .map(|(index, node)| match &node.data {
                SpatialData::Scroll(scroll) => scroll_offset(SpatialNodeIndex(index as u32), scroll),
                _ => FloatPoint::default(),
            })
            .collect();
        let world = WorldTransforms::new(tree, &scroll_offsets, None);
        let mut animated = vec![false; tree.spatial_nodes.len()];
        for animation in tree.visual_animations() {
            if animation.target_kind != FfiVisualAnimationTargetKind::Transform {
                continue;
            }
            for &index in &animation.node_indices {
                if let Some(flag) = animated.get_mut(index as usize) {
                    *flag = true;
                }
            }
        }
        let mut moves_without_a_recording = vec![false; tree.spatial_nodes.len()];
        for index in 0..tree.spatial_nodes.len() {
            let node = &tree.spatial_nodes[index];
            let parent_moves = index != VISUAL_VIEWPORT_NODE_INDEX.0 as usize
                && moves_without_a_recording
                    .get(node.parent.0 as usize)
                    .copied()
                    .unwrap_or(true);
            let moves_on_its_own = match &node.data {
                SpatialData::Sticky(_) | SpatialData::AnchorScrollShift(_) | SpatialData::Perspective(_) => true,
                SpatialData::Transform(_) => {
                    animated[index] || !world.draw_matrix(SpatialNodeIndex(index as u32)).is_2d_affine()
                }
                SpatialData::Scroll(_) | SpatialData::BackfaceVisibility(_) | SpatialData::Dead => false,
            };
            moves_without_a_recording[index] = parent_moves || moves_on_its_own;
        }
        Self {
            tree,
            world,
            moves_without_a_recording,
        }
    }

    pub fn tree(&self) -> &'a VisualContextTree {
        self.tree
    }

    /// Whether something above `node`, or `node` itself, moves what it holds without the document being recorded again.
    pub fn moves_without_a_recording(&self, node: SpatialNodeIndex) -> bool {
        self.moves_without_a_recording
            .get(node.0 as usize)
            .copied()
            .unwrap_or(true)
    }

    /// What takes the space of `node` to the space of the visual viewport, or none for a node that moves without a
    /// recording.
    pub fn affine(&self, node: SpatialNodeIndex) -> Option<AffineTransform> {
        if self.moves_without_a_recording(node) {
            return None;
        }
        let matrix = self.world.draw_matrix(node);
        matrix.is_2d_affine().then(|| matrix.extract_2d_affine())
    }

    /// Where `rect` in the space of `node` lands in the space of the visual viewport, or none for a node that moves
    /// without a recording.
    pub fn to_root(&self, node: SpatialNodeIndex, rect: FloatRect) -> Option<FloatRect> {
        self.affine(node).map(|affine| affine.map_rect(rect))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::node_slot_id::NodeSlotId;
    use crate::visual_context::{StickyData, TransformData, TransformDataRole};
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

    const VIEWPORT: IntSize = IntSize {
        width: 800,
        height: 600,
    };

    // The offsets of the scrollers of `tree` by node, from `offsets` by scroll state slot.
    fn offsets(tree: &VisualContextTree, offsets: &[(f32, f32)]) -> Arc<[FloatPoint]> {
        tree.spatial_nodes
            .iter()
            .map(|node| match &node.data {
                SpatialData::Scroll(scroll) => offsets
                    .get(scroll.state_slot)
                    .map(|&(x, y)| FloatPoint { x, y })
                    .unwrap_or_default(),
                _ => FloatPoint::default(),
            })
            .collect()
    }

    fn live(offsets: &[(f32, f32)]) -> impl Fn(SpatialNodeIndex, &ScrollData) -> FloatPoint + '_ {
        move |_, scroll| {
            offsets
                .get(scroll.state_slot)
                .map(|&(x, y)| FloatPoint { x, y })
                .unwrap_or_default()
        }
    }

    // Keeping half the margin ahead, as the main thread does.
    fn covers(window: &RecordedWindow, tree: &VisualContextTree, now: &[(f32, f32)]) -> bool {
        covers_with(window, tree, tree, now)
    }

    fn covers_with(
        window: &RecordedWindow,
        tree_of_recording: &VisualContextTree,
        tree_now: &VisualContextTree,
        now: &[(f32, f32)],
    ) -> bool {
        let margin = window.margin();
        window.still_covers(
            tree_of_recording,
            tree_now,
            VIEWPORT,
            1.0,
            live(now),
            FloatSize {
                width: margin.width / 2.0,
                height: margin.height / 2.0,
            },
        )
    }

    #[test]
    fn a_window_reaches_two_viewports_past_the_viewport() {
        let window = RecordedWindow::placed_at(VIEWPORT, 1.0, Arc::default());
        assert_eq!(window.rect, FloatRect::new(-1600.0, -1200.0, 4000.0, 3000.0));
        let small = RecordedWindow::placed_at(
            IntSize {
                width: 100,
                height: 100,
            },
            2.0,
            Arc::default(),
        );
        assert_eq!(
            small.margin(),
            FloatSize {
                width: 2000.0,
                height: 2000.0
            }
        );
    }

    #[test]
    fn a_window_holds_while_what_shows_stays_half_a_margin_inside_it() {
        let mut tree = empty_tree();
        tree.append_spatial(scroller(0), VISUAL_VIEWPORT_NODE_INDEX);
        let window = RecordedWindow::placed_at(VIEWPORT, 1.0, offsets(&tree, &[(0.0, -1000.0)]));
        assert!(covers(&window, &tree, &[(0.0, -1000.0)]));
        assert!(covers(&window, &tree, &[(0.0, -1600.0)]));
        assert!(covers(&window, &tree, &[(0.0, -400.0)]));
        assert!(!covers(&window, &tree, &[(0.0, -1601.0)]));
        assert!(!covers(&window, &tree, &[(0.0, -399.0)]));
        // Another viewport or another scale is another window.
        let taller = IntSize {
            width: 800,
            height: 601,
        };
        assert!(!window.still_covers(&tree, &tree, taller, 1.0, live(&[(0.0, -1000.0)]), FloatSize::default()));
        assert!(!window.still_covers(
            &tree,
            &tree,
            VIEWPORT,
            2.0,
            live(&[(0.0, -1000.0)]),
            FloatSize::default()
        ));
    }

    #[test]
    fn scrollers_in_one_chain_move_what_the_innermost_holds_by_their_sum() {
        // An 800 by 600 viewport, the page scroller A in it and a scroller B in A. Each moved by 600 since the window
        // was placed stays within the slack a scroller has alone, but what B holds moved by 1200.
        let mut tree = empty_tree();
        let page = tree.append_spatial(scroller(0), VISUAL_VIEWPORT_NODE_INDEX);
        let a = tree.append_spatial(scroller(1), page);
        tree.append_spatial(scroller(2), a);
        let window = RecordedWindow::placed_at(VIEWPORT, 1.0, offsets(&tree, &[(0.0, 0.0), (0.0, 0.0), (0.0, 0.0)]));
        assert!(covers(&window, &tree, &[(0.0, -600.0), (0.0, 0.0), (0.0, 0.0)]));
        assert!(!covers(&window, &tree, &[(0.0, -600.0), (0.0, -600.0), (0.0, 0.0)]));
        assert!(!covers(&window, &tree, &[(0.0, -600.0), (0.0, -600.0), (0.0, -600.0)]));
        // Scrolling one way and back the other leaves what B holds where it was.
        assert!(covers(&window, &tree, &[(0.0, -600.0), (0.0, 600.0), (0.0, 0.0)]));
    }

    #[test]
    fn a_static_transform_scales_how_far_a_scroller_below_it_moves_what_it_holds() {
        let mut tree = empty_tree();
        let page = tree.append_spatial(scroller(0), VISUAL_VIEWPORT_NODE_INDEX);
        let scaled = tree.append_spatial(transform(libgfx_rust::scale_matrix(2.0, 2.0, 1.0)), page);
        let inner = tree.append_spatial(scroller(1), scaled);
        let window = RecordedWindow::placed_at(VIEWPORT, 1.0, offsets(&tree, &[(0.0, 0.0), (0.0, 0.0)]));
        // 400 in the scroller is 800 on screen.
        assert!(covers(&window, &tree, &[(0.0, 0.0), (0.0, -300.0)]));
        assert!(!covers(&window, &tree, &[(0.0, 0.0), (0.0, -400.0)]));
        let spaces = window.placed_spaces(&tree);
        assert_eq!(
            spaces.to_root(inner, FloatRect::new(0.0, 100.0, 10.0, 10.0)),
            Some(FloatRect::new(0.0, 200.0, 20.0, 20.0))
        );
    }

    #[test]
    fn a_rotated_space_covers_what_it_covered_while_its_rotation_holds() {
        // A space rotated by 60 degrees far below what shows. What shows, taken back to that space and placed again
        // as it was, is where it was: not the box around the box around it, which is taller than the window.
        let rotated_by = |angle: f32| {
            let mut tree = empty_tree();
            let page = tree.append_spatial(scroller(0), VISUAL_VIEWPORT_NODE_INDEX);
            tree.append_spatial(
                transform(
                    libgfx_rust::translation_matrix(0.0, 50_000.0, 0.0)
                        .multiplied(libgfx_rust::rotation_matrix([0.0, 0.0, 1.0], angle)),
                ),
                page,
            );
            tree
        };
        let tree = rotated_by(std::f32::consts::FRAC_PI_3);
        let window = RecordedWindow::placed_at(VIEWPORT, 1.0, offsets(&tree, &[(0.0, 0.0)]));
        assert!(covers(&window, &tree, &[(0.0, 0.0)]));
        assert!(covers(&window, &tree, &[(0.0, -500.0)]));
        assert!(!covers(&window, &tree, &[(0.0, -700.0)]));
        // Rotating it further turns what it holds about a point far from what shows.
        assert!(!covers_with(
            &window,
            &tree,
            &rotated_by(std::f32::consts::FRAC_PI_2),
            &[(0.0, 0.0)]
        ));
    }

    #[test]
    fn a_transform_that_changed_since_the_recording_moves_what_it_holds_as_scrolling_does() {
        let mut tree_of_recording = empty_tree();
        let page = tree_of_recording.append_spatial(scroller(0), VISUAL_VIEWPORT_NODE_INDEX);
        let moved = tree_of_recording.append_spatial(transform(FloatMatrix4x4::identity()), page);
        let window = RecordedWindow::placed_at(VIEWPORT, 1.0, offsets(&tree_of_recording, &[(0.0, 0.0)]));
        let moved_by = |y: f32| {
            let mut tree = tree_of_recording.clone();
            tree.spatial_nodes[moved.0 as usize].data = transform(libgfx_rust::translation_matrix(0.0, y, 0.0));
            tree
        };
        assert!(covers_with(
            &window,
            &tree_of_recording,
            &moved_by(-600.0),
            &[(0.0, 0.0)]
        ));
        assert!(!covers_with(
            &window,
            &tree_of_recording,
            &moved_by(-3900.0),
            &[(0.0, 0.0)]
        ));
        assert!(spaces_place_alike(&tree_of_recording, &tree_of_recording.clone()));
        assert!(!spaces_place_alike(&tree_of_recording, &moved_by(-600.0)));
        // Another tree structure is never covered.
        let mut rebuilt = tree_of_recording.clone();
        rebuilt.structural_epoch += 1;
        assert!(!covers_with(&window, &tree_of_recording, &rebuilt, &[(0.0, 0.0)]));
    }

    #[test]
    fn nothing_below_a_node_that_moves_on_its_own_is_placed() {
        let mut tree = empty_tree();
        let page = tree.append_spatial(scroller(0), VISUAL_VIEWPORT_NODE_INDEX);
        let sticky = tree.append_spatial(
            SpatialData::Sticky(StickyData::unconstrained(page, None, 1, NodeSlotId::INVALID)),
            page,
        );
        let below_sticky = tree.append_spatial(scroller(1), sticky);
        let moved = tree.append_spatial(transform(libgfx_rust::translation_matrix(0.0, 50.0, 0.0)), page);
        let window = RecordedWindow::placed_at(VIEWPORT, 1.0, offsets(&tree, &[(0.0, 0.0), (0.0, 0.0)]));
        let spaces = window.placed_spaces(&tree);
        let rect = FloatRect::new(0.0, 0.0, 10.0, 10.0);
        assert_eq!(spaces.to_root(sticky, rect), None);
        assert_eq!(spaces.to_root(below_sticky, rect), None);
        assert_eq!(spaces.to_root(moved, rect), Some(FloatRect::new(0.0, 50.0, 10.0, 10.0)));
        // A scroller below a node that moves on its own leaves nothing out, so scrolling it never runs out.
        assert!(covers(&window, &tree, &[(0.0, 0.0), (0.0, -100_000.0)]));
    }

    #[test]
    fn a_window_from_another_process_is_one_a_recording_could_place() {
        let window = RecordedWindow::placed_at(VIEWPORT, 2.0, offsets(&empty_tree(), &[]));
        assert_eq!(RecordedWindow::from_ipc(&window.to_ipc()), Some(window.clone()));
        let with = |change: fn(&mut Compositing_DisplayListWindow)| {
            let mut sent = window.to_ipc();
            change(&mut sent);
            RecordedWindow::from_ipc(&sent)
        };
        assert_eq!(with(|sent| sent.rect.height = f32::INFINITY), None);
        assert_eq!(with(|sent| sent.rect.width = -1.0), None);
        assert_eq!(with(|sent| sent.device_viewport_size.width = 0), None);
        assert_eq!(with(|sent| sent.device_pixels_per_css_pixel = f64::NAN), None);
        assert_eq!(
            with(|sent| sent.scroll_offsets = vec![FloatPoint { x: 0.0, y: f32::NAN }]),
            None
        );
    }
}
