/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Decides when a context asks its document's render clock for a display list where the compositor has scrolled to.
//! A display list holds what shows in a window around where its scrollers were when it was recorded, and WebContent
//! records one for a new window once its own frame scrolled half the window's margin from there. The context asks
//! once its scrolling does that: WebContent's event loop may be too busy to record one soon. It asks once for a display
//! list, and again once a scroller has moved half a viewport further.

use libcompositing_rust::display_list::window::RecordedWindow;
use libcompositing_rust::scrolling::ScrollStateSnapshot;
use libcompositing_rust::visual_context::VisualContextTree;
use libgfx_rust::{FloatPoint, FloatSize, IntSize};

const VIEWPORTS_SCROLLED_BEFORE_ASKING_AGAIN: f32 = 0.5;

pub struct ShownOfDisplayList<'a> {
    pub display_list_id: u64,
    pub window: &'a RecordedWindow,
    pub visual_context_tree: &'a VisualContextTree,
    pub scroll_state_snapshot: &'a ScrollStateSnapshot,
    pub viewport_size: IntSize,
}

struct Request {
    display_list_id: u64,
    device_scroll_offsets: Vec<FloatPoint>,
}

#[derive(Default)]
pub struct DisplayListCoverageWatch {
    last_request: Option<Request>,
    last_checked: Option<Request>,
}

/// Whether the window of the display list still holds what the viewport shows with the scrollers where the compositor
/// has them, with half the window's margin to spare.
fn window_still_covers(shown: &ShownOfDisplayList<'_>) -> bool {
    let window = shown.window;
    let tree = shown.visual_context_tree;
    let margin = window.margin();
    window.still_covers(
        tree,
        tree,
        shown.viewport_size,
        window.device_pixels_per_css_pixel,
        |node, _| shown.scroll_state_snapshot.device_offset_for_index(node),
        FloatSize {
            width: margin.width / 2.0,
            height: margin.height / 2.0,
        },
    )
}

fn scrolled_on_since(asked_at: &[FloatPoint], now: &[FloatPoint], viewport_size: IntSize) -> bool {
    let offset_at = |offsets: &[FloatPoint], index: usize| offsets.get(index).copied().unwrap_or_default();
    let horizontally = viewport_size.width as f32 * VIEWPORTS_SCROLLED_BEFORE_ASKING_AGAIN;
    let vertically = viewport_size.height as f32 * VIEWPORTS_SCROLLED_BEFORE_ASKING_AGAIN;
    (0..asked_at.len().max(now.len())).any(|index| {
        let (asked_at, now) = (offset_at(asked_at, index), offset_at(now, index));
        (now.x - asked_at.x).abs() >= horizontally || (now.y - asked_at.y).abs() >= vertically
    })
}

impl DisplayListCoverageWatch {
    pub fn asks_for_display_list_where_scrolled(&mut self, shown: &ShownOfDisplayList<'_>) -> bool {
        let device_scroll_offsets = shown.scroll_state_snapshot.device_offsets();
        let checked_already = self.last_checked.as_ref().is_some_and(|checked| {
            checked.display_list_id == shown.display_list_id && checked.device_scroll_offsets == device_scroll_offsets
        });
        if checked_already {
            return false;
        }
        self.last_checked = Some(Request {
            display_list_id: shown.display_list_id,
            device_scroll_offsets: device_scroll_offsets.to_vec(),
        });
        let request_is_outstanding = self.last_request.as_ref().is_some_and(|request| {
            request.display_list_id == shown.display_list_id
                && !scrolled_on_since(
                    &request.device_scroll_offsets,
                    device_scroll_offsets,
                    shown.viewport_size,
                )
        });
        if request_is_outstanding || window_still_covers(shown) {
            return false;
        }
        self.last_request = Some(Request {
            display_list_id: shown.display_list_id,
            device_scroll_offsets: device_scroll_offsets.to_vec(),
        });
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libcompositing_rust::display_list::commands::SpatialNodeIndex;
    use libcompositing_rust::node_slot_id::NodeSlotId;
    use libcompositing_rust::visual_context::scroll_state::NO_SCROLL_STATE_SLOT;
    use libcompositing_rust::visual_context::{ScrollData, SpatialData, VISUAL_VIEWPORT_NODE_INDEX};
    use std::sync::Arc;

    const VIEWPORT: IntSize = IntSize {
        width: 800,
        height: 600,
    };

    struct PageWithAScrollerInIt {
        tree: VisualContextTree,
        page: SpatialNodeIndex,
        inner_scroller: SpatialNodeIndex,
    }

    impl PageWithAScrollerInIt {
        fn new() -> Self {
            let scroller = || {
                SpatialData::Scroll(ScrollData {
                    state_slot: NO_SCROLL_STATE_SLOT,
                    owner_paintable: NodeSlotId::INVALID,
                })
            };
            let mut tree = VisualContextTree::create_identity();
            let page = tree.append_spatial(scroller(), VISUAL_VIEWPORT_NODE_INDEX);
            let inner_scroller = tree.append_spatial(scroller(), page);
            Self {
                tree,
                page,
                inner_scroller,
            }
        }

        // The scrollers scrolled down by each amount, in device pixels.
        fn device_offsets(&self, scrolled: &[(SpatialNodeIndex, f32)]) -> Vec<FloatPoint> {
            let mut offsets = vec![FloatPoint::default(); self.tree.spatial_nodes.len()];
            for &(scroller, by) in scrolled {
                offsets[scroller.0 as usize] = FloatPoint { x: 0.0, y: -by };
            }
            offsets
        }

        fn window_placed_with(&self, scrolled: &[(SpatialNodeIndex, f32)]) -> RecordedWindow {
            RecordedWindow::placed_at(VIEWPORT, 1.0, Arc::from(self.device_offsets(scrolled)))
        }

        fn asks(
            &self,
            watch: &mut DisplayListCoverageWatch,
            display_list_id: u64,
            window: &RecordedWindow,
            scrolled: &[(SpatialNodeIndex, f32)],
        ) -> bool {
            let snapshot = ScrollStateSnapshot::from_device_offsets(self.device_offsets(scrolled));
            watch.asks_for_display_list_where_scrolled(&ShownOfDisplayList {
                display_list_id,
                window,
                visual_context_tree: &self.tree,
                scroll_state_snapshot: &snapshot,
                viewport_size: VIEWPORT,
            })
        }
    }

    #[test]
    fn a_context_asks_once_what_shows_comes_within_half_a_margin_of_the_window_edge() {
        let page = PageWithAScrollerInIt::new();
        let window = page.window_placed_with(&[]);
        let mut watch = DisplayListCoverageWatch::default();
        // The window reaches 1200 below the viewport, and half of that is kept ahead.
        assert!(!page.asks(&mut watch, 1, &window, &[(page.page, 600.0)]));
        assert!(page.asks(&mut watch, 1, &window, &[(page.page, 601.0)]));
    }

    #[test]
    fn scrollers_in_one_chain_run_out_of_the_window_together() {
        let page = PageWithAScrollerInIt::new();
        let window = page.window_placed_with(&[]);
        let mut watch = DisplayListCoverageWatch::default();
        assert!(!page.asks(&mut watch, 1, &window, &[(page.inner_scroller, 400.0)]));
        assert!(page.asks(
            &mut watch,
            1,
            &window,
            &[(page.page, 400.0), (page.inner_scroller, 400.0)]
        ));
    }

    #[test]
    fn a_context_asks_again_after_half_a_viewport_more_or_for_a_newer_list_that_falls_short() {
        let page = PageWithAScrollerInIt::new();
        let window = page.window_placed_with(&[]);
        let mut watch = DisplayListCoverageWatch::default();
        assert!(page.asks(&mut watch, 1, &window, &[(page.page, 700.0)]));
        assert!(!page.asks(&mut watch, 1, &window, &[(page.page, 999.0)]));
        assert!(page.asks(&mut watch, 1, &window, &[(page.page, 1000.0)]));
        // A list recorded where the page is now covers it, and a newer one recorded where it was does not.
        let window_where_scrolled = page.window_placed_with(&[(page.page, 1000.0)]);
        assert!(!page.asks(&mut watch, 2, &window_where_scrolled, &[(page.page, 1000.0)]));
        assert!(page.asks(&mut watch, 3, &window, &[(page.page, 1000.0)]));
    }
}
