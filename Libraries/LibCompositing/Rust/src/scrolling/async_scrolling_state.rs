/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The scrolling state a display list carries for the compositor: scroll nodes, snap containers, wheel regions and
//! scrollbars.

use super::scroll_snap_selection::{SnapAlign, SnapAreaGeometry, SnapAxes, SnapContainerGeometry, SnapStrictness};
use super::{
    AsyncScrollNodeKind, AsyncScrollNodeStableID, AsyncScrollOffset, CompositorContextId, CompositorScrollNodeKind,
    ContextRef, DisplayList, InputRoutingDecision, SnapAreaIdentity, SpatialNodeIndex, UniqueNodeId,
};
use crate::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::display_list::builder::read_command;
use crate::display_list::commands::*;
use crate::visual_context::{
    CompositorScrollNodeRecord, CompositorSnapAreaRecord, CompositorSnapContainerRecord, VISUAL_VIEWPORT_NODE_INDEX,
    VisualContextTree,
};
use libgfx_rust::{Color, CornerRadii, FloatPoint, FloatRect, IntRect};

pub type AsyncScrollOperationID = u64;

impl AsyncScrollOffset {
    pub fn merge_later_scroll(&mut self, later: &Self) {
        self.compositor_scroll_offset = later.compositor_scroll_offset;
        // The last of the main thread's scroll requests for the box that the compositor applied before it reported this.
        self.applied_scroll_request_id = later.applied_scroll_request_id;
        // How many times the compositor had scrolled the box for user input of its own when it reported this.
        self.user_input_scroll_count = later.user_input_scroll_count;
        // How many times the compositor had scrolled any box of the context for user input of its own when it reported
        // this. Not every such scroll is reported, as one the next input turns back moves no box.
        self.context_user_input_scroll_count = later.context_user_input_scroll_count;
        // The scale the visual viewport was zoomed to, which only its scroll node reports.
        if later.visual_viewport_scale.is_some() {
            self.visual_viewport_scale = later.visual_viewport_scale;
        }
        self.unadopted_scroll_delta.translate_by(later.unadopted_scroll_delta);
        // The most recent nonzero delta of a relative scroll in each axis, such as a wheel or panning scroll. Absolute
        // scrolls, such as dragging a scrollbar thumb, leave it as it is.
        if later.last_relative_scroll_delta.x != 0.0 {
            self.last_relative_scroll_delta.x = later.last_relative_scroll_delta.x;
        }
        if later.last_relative_scroll_delta.y != 0.0 {
            self.last_relative_scroll_delta.y = later.last_relative_scroll_delta.y;
        }
    }
}

// One scrollable area from the paint snapshot, or a scroll container with nothing to scroll that keeps its scrolls.
#[derive(Clone, Copy)]
pub struct AsyncScrollNode {
    pub scroll_node_index: SpatialNodeIndex,
    pub stable_node_id: AsyncScrollNodeStableID,
    pub parent_scroll_node_index: Option<SpatialNodeIndex>,
    pub scrollport_rect: IntRect,
    pub min_scroll_offset: FloatPoint,
    pub max_scroll_offset: FloatPoint,
    pub is_viewport: bool,
    pub can_be_wheel_scrolled_horizontally: bool,
    pub can_be_wheel_scrolled_vertically: bool,
    // Whether a scroll the scroller cannot scroll by is passed on to the scrollers around it.
    pub chains_scroll_to_ancestors_horizontally: bool,
    pub chains_scroll_to_ancestors_vertically: bool,
    // Whether a touchpad may pull the scroller past the edges of its scroll range.
    pub shows_overscroll_effect_horizontally: bool,
    pub shows_overscroll_effect_vertically: bool,
}

// A region with a non-passive wheel listener. Wheels inside it must stay on the main thread because script may cancel.
#[derive(Clone, Copy, Default)]
pub struct BlockingWheelEventRegion {
    pub context: ContextRef,
    pub rect: FloatRect,
}

#[derive(Clone, Copy, Default)]
pub struct WheelHitTestTarget {
    pub context: ContextRef,
    pub rect: FloatRect,
    pub corner_radii: CornerRadii,
    pub target_scroll_node_index: Option<SpatialNodeIndex>,
    // Position among the wheel hit test targets and scrollbars of the display list, which are recorded in paint order.
    pub paint_order_index: u32,
}

// A region that must always use main-thread wheel routing even without a blocking listener, such as a nested navigable.
#[derive(Clone, Copy, Default)]
pub struct MainThreadWheelEventRegion {
    pub context: ContextRef,
    pub rect: FloatRect,
}

// Where a nested context's content is painted. A wheel inside it that no wheel hit test target painted later covers
// can be handed to the nested context.
#[derive(Clone, Copy, Default)]
pub struct NestedContextWheelRegion {
    pub context: ContextRef,
    pub rect: FloatRect,
    pub corner_radii: CornerRadii,
    pub child_context_id: CompositorContextId,
    pub paint_order_index: u32,
}

#[derive(Clone, Copy, Default)]
pub struct AsyncScrollbar {
    pub scroller_stable_node_id: Option<AsyncScrollNodeStableID>,
    pub scroll_node_index: SpatialNodeIndex,
    // The rects of a scrollbar the display list paints are in the space of this context.
    pub context: ContextRef,
    pub paint_order_index: u32,
    pub gutter_rect: IntRect,
    pub thumb_rect: IntRect,
    pub track_rect: IntRect,
    pub expanded_gutter_rect: IntRect,
    pub expanded_thumb_rect: IntRect,
    pub scroll_size: f64,
    pub expanded_scroll_size: f64,
    pub min_scroll_offset: f32,
    pub max_scroll_offset: f32,
    pub thumb_color: Color,
    pub thumb_outline_color: Color,
    pub track_color: Color,
    pub vertical: bool,
    // The compositor paints the viewport's scrollbars itself. The display list paints every other scrollbar thin, which
    // the compositor presents growing as it is hovered, as it does its own.
    pub is_painted_by_compositor: bool,
}

// A scroll node that is a snap container, with the geometry snap positions are selected from.
pub struct AsyncSnapContainer {
    pub scroll_node_index: SpatialNodeIndex,
    pub geometry: SnapContainerGeometry,
    pub areas: Vec<SnapAreaGeometry>,
}

pub struct AsyncScrollingState {
    pub document_id: Option<UniqueNodeId>,
    pub scroll_nodes: Vec<AsyncScrollNode>,
    pub snap_containers: Vec<AsyncSnapContainer>,
    pub wheel_hit_test_targets: Vec<WheelHitTestTarget>,
    pub main_thread_wheel_event_regions: Vec<MainThreadWheelEventRegion>,
    pub nested_context_wheel_regions: Vec<NestedContextWheelRegion>,
    pub scrollbars: Vec<AsyncScrollbar>,

    // Non-passive wheel listeners can cancel scrolling, so async scrolling must treat them as hard barriers.
    // Viewport-wide barriers cover listeners on the root targets; element regions let input hit-testing accept
    // async scrolling elsewhere.
    pub blocking_wheel_event_regions: Vec<BlockingWheelEventRegion>,
    pub viewport_rect: IntRect,

    // Bumped whenever wheel listener state changes so queued compositor snapshots
    // cannot re-enable async wheel routing after a non-passive listener has been
    // added.
    pub wheel_event_listener_state_generation: u64,
    pub has_blocking_wheel_event_listeners: bool,
    pub has_blocking_wheel_event_region_covering_viewport: bool,
    pub viewport_keeps_horizontal_scrolls: bool,
    pub viewport_keeps_vertical_scrolls: bool,
    pub device_pixels_per_css_pixel: f64,
}

impl Default for AsyncScrollingState {
    fn default() -> Self {
        Self {
            document_id: Default::default(),
            scroll_nodes: Default::default(),
            snap_containers: Default::default(),
            wheel_hit_test_targets: Default::default(),
            main_thread_wheel_event_regions: Default::default(),
            nested_context_wheel_regions: Default::default(),
            scrollbars: Default::default(),
            blocking_wheel_event_regions: Default::default(),
            viewport_rect: Default::default(),
            wheel_event_listener_state_generation: Default::default(),
            has_blocking_wheel_event_listeners: false,
            has_blocking_wheel_event_region_covering_viewport: false,
            viewport_keeps_horizontal_scrolls: false,
            viewport_keeps_vertical_scrolls: false,
            device_pixels_per_css_pixel: 1.0,
        }
    }
}

impl AsyncScrollingState {
    // Scroll offsets and the durations of scroll animations are calculated from these, and a number that is not finite,
    // or a scale that is not positive, leaves them with no value.
    pub fn has_only_numbers_scrolls_can_be_calculated_with(&self) -> bool {
        let is_finite = |point: FloatPoint| point.x.is_finite() && point.y.is_finite();
        self.device_pixels_per_css_pixel.is_finite()
            && self.device_pixels_per_css_pixel > 0.0
            && self
                .scroll_nodes
                .iter()
                .all(|node| is_finite(node.min_scroll_offset) && is_finite(node.max_scroll_offset))
            && self.scrollbars.iter().all(|scrollbar| {
                scrollbar.scroll_size.is_finite()
                    && scrollbar.expanded_scroll_size.is_finite()
                    && scrollbar.min_scroll_offset.is_finite()
                    && scrollbar.max_scroll_offset.is_finite()
            })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WheelRoutingAdmission {
    Accepted,
    NoAsyncScrollingState,
    BlockingWheelEventListeners,
    NoScrollNode,
    StaleWheelEventListeners,
}

pub fn async_scroll_node_kind_for(kind: CompositorScrollNodeKind) -> AsyncScrollNodeKind {
    match kind {
        CompositorScrollNodeKind::Viewport => AsyncScrollNodeKind::Viewport,
        CompositorScrollNodeKind::Element => AsyncScrollNodeKind::Element,
        CompositorScrollNodeKind::PseudoElement => AsyncScrollNodeKind::PseudoElement,
    }
}

fn stable_scroll_node_id_for(
    scrollable_node_id: UniqueNodeId,
    kind: CompositorScrollNodeKind,
    pseudo_element_type: u8,
) -> AsyncScrollNodeStableID {
    AsyncScrollNodeStableID {
        node_id: scrollable_node_id,
        kind: async_scroll_node_kind_for(kind),
        pseudo_element_type,
    }
}

fn async_scroll_node_from(node: &CompositorScrollNodeRecord) -> AsyncScrollNode {
    let parent_scroll_node_index =
        (node.parent_scroll_node_index != VISUAL_VIEWPORT_NODE_INDEX).then_some(node.parent_scroll_node_index);
    AsyncScrollNode {
        scroll_node_index: node.scroll_node_index,
        stable_node_id: stable_scroll_node_id_for(
            node.scrollable_node_id,
            node.scroll_node_kind,
            node.pseudo_element_type,
        ),
        parent_scroll_node_index,
        scrollport_rect: node.scrollport_rect,
        min_scroll_offset: node.min_scroll_offset,
        max_scroll_offset: node.max_scroll_offset,
        is_viewport: node.is_viewport,
        can_be_wheel_scrolled_horizontally: node.can_be_wheel_scrolled_horizontally,
        can_be_wheel_scrolled_vertically: node.can_be_wheel_scrolled_vertically,
        chains_scroll_to_ancestors_horizontally: node.chains_scroll_to_ancestors_horizontally,
        chains_scroll_to_ancestors_vertically: node.chains_scroll_to_ancestors_vertically,
        shows_overscroll_effect_horizontally: node.shows_overscroll_effect_horizontally,
        shows_overscroll_effect_vertically: node.shows_overscroll_effect_vertically,
    }
}

fn snap_strictness_from(value: u8) -> Option<SnapStrictness> {
    match value {
        0 => Some(SnapStrictness::None),
        1 => Some(SnapStrictness::Proximity),
        2 => Some(SnapStrictness::Mandatory),
        _ => None,
    }
}

fn snap_align_from(value: u8) -> Option<SnapAlign> {
    match value {
        0 => Some(SnapAlign::None),
        1 => Some(SnapAlign::Start),
        2 => Some(SnapAlign::End),
        3 => Some(SnapAlign::Center),
        _ => None,
    }
}

fn async_snap_container_from(container: &CompositorSnapContainerRecord) -> Option<AsyncSnapContainer> {
    Some(AsyncSnapContainer {
        scroll_node_index: container.scroll_node_index,
        geometry: SnapContainerGeometry {
            snapport: container.snapport.into(),
            min_scroll_offset: container.min_scroll_offset.into(),
            max_scroll_offset: container.max_scroll_offset.into(),
            strictness: snap_strictness_from(container.strictness)?,
            axes: SnapAxes {
                x: container.snaps_x,
                y: container.snaps_y,
            },
            horizontal_writing_mode: container.horizontal_writing_mode,
        },
        areas: Vec::new(),
    })
}

fn snap_area_geometry_from(area: &CompositorSnapAreaRecord) -> Option<SnapAreaGeometry> {
    Some(SnapAreaGeometry {
        identity: SnapAreaIdentity {
            node_id: area.area_node_id,
            pseudo_element_type: area.pseudo_element_type,
        },
        rect: area.rect.into(),
        align_x: snap_align_from(area.align_x)?,
        align_y: snap_align_from(area.align_y)?,
        always_stop: area.always_stop,
    })
}

pub fn async_scrolling_state_from(
    display_list: &DisplayList,
    visual_context_tree: &VisualContextTree,
) -> AsyncScrollingState {
    let mut async_scrolling_state = AsyncScrollingState::default();
    let mut next_paint_order_index = 0u32;
    if let Some(metadata) = display_list.async_scrolling_metadata() {
        async_scrolling_state.document_id = metadata.document_id;
        async_scrolling_state.viewport_rect = metadata.viewport_rect;
        async_scrolling_state.wheel_event_listener_state_generation = metadata.wheel_event_listener_state_generation;
        async_scrolling_state.has_blocking_wheel_event_listeners = metadata.has_blocking_wheel_event_listeners;
        async_scrolling_state.has_blocking_wheel_event_region_covering_viewport =
            metadata.has_blocking_wheel_event_region_covering_viewport;
        async_scrolling_state.viewport_keeps_horizontal_scrolls = metadata.viewport_keeps_horizontal_scrolls;
        async_scrolling_state.viewport_keeps_vertical_scrolls = metadata.viewport_keeps_vertical_scrolls;
        async_scrolling_state.device_pixels_per_css_pixel = metadata.device_pixels_per_css_pixel;
    }
    let scroll_table = visual_context_tree.compositor_scroll_table();
    async_scrolling_state
        .scroll_nodes
        .extend(scroll_table.nodes.iter().map(async_scroll_node_from));
    for container in scroll_table.snap_containers.iter() {
        let Some(mut snap_container) = async_snap_container_from(container) else {
            continue;
        };
        let first_area_index = container.first_area_index as usize;
        let areas = &scroll_table.snap_areas[first_area_index..first_area_index + container.area_count as usize];
        snap_container
            .areas
            .extend(areas.iter().filter_map(snap_area_geometry_from));
        async_scrolling_state.snap_containers.push(snap_container);
    }
    display_list.for_each_compositor_metadata(|context, command_type, payload| {
        let mut append_wheel_hit_test_target = |target_scroll_node_index: SpatialNodeIndex, rect, corner_radii| {
            let target_scroll_node_index = if target_scroll_node_index.0 != 0 {
                Some(target_scroll_node_index)
            } else {
                None
            };
            async_scrolling_state.wheel_hit_test_targets.push(WheelHitTestTarget {
                context,
                rect,
                corner_radii,
                target_scroll_node_index,
                paint_order_index: next_paint_order_index,
            });
            next_paint_order_index = next_paint_order_index.wrapping_add(1);
        };
        match command_type {
            DisplayListCommandType::CompositorBlockingWheelEventRegion => {
                let command = read_command::<CompositorBlockingWheelEventRegion>(payload);
                async_scrolling_state.has_blocking_wheel_event_listeners = true;
                async_scrolling_state
                    .blocking_wheel_event_regions
                    .push(BlockingWheelEventRegion {
                        context,
                        rect: command.rect,
                    });
            }
            DisplayListCommandType::CompositorWheelHitTestTarget => {
                let command = read_command::<CompositorWheelHitTestTarget>(payload);
                append_wheel_hit_test_target(command.target_scroll_node_index, command.rect, CornerRadii::default());
            }
            DisplayListCommandType::CompositorWheelHitTestTargetWithCornerRadii => {
                let command = read_command::<CompositorWheelHitTestTargetWithCornerRadii>(payload);
                append_wheel_hit_test_target(command.target_scroll_node_index, command.rect, command.corner_radii);
            }
            DisplayListCommandType::CompositorMainThreadWheelEventRegion => {
                let command = read_command::<CompositorMainThreadWheelEventRegion>(payload);
                async_scrolling_state
                    .main_thread_wheel_event_regions
                    .push(MainThreadWheelEventRegion {
                        context,
                        rect: command.rect,
                    });
            }
            DisplayListCommandType::CompositorNestedContextWheelRegion => {
                let command = read_command::<CompositorNestedContextWheelRegion>(payload);
                async_scrolling_state
                    .nested_context_wheel_regions
                    .push(NestedContextWheelRegion {
                        context,
                        rect: command.rect,
                        corner_radii: command.corner_radii,
                        child_context_id: command.child_context_id,
                        paint_order_index: next_paint_order_index,
                    });
                next_paint_order_index = next_paint_order_index.wrapping_add(1);
            }
            DisplayListCommandType::CompositorScrollbar => {
                let command = read_command::<CompositorScrollbar>(payload);
                async_scrolling_state.scrollbars.push(AsyncScrollbar {
                    scroller_stable_node_id: None,
                    scroll_node_index: command.scroll_node_index,
                    context,
                    paint_order_index: next_paint_order_index,
                    gutter_rect: command.gutter_rect,
                    thumb_rect: command.thumb_rect,
                    track_rect: command.track_rect,
                    expanded_gutter_rect: command.expanded_gutter_rect,
                    expanded_thumb_rect: command.expanded_thumb_rect,
                    scroll_size: command.scroll_size,
                    expanded_scroll_size: command.expanded_scroll_size,
                    min_scroll_offset: command.min_scroll_offset,
                    max_scroll_offset: command.max_scroll_offset,
                    thumb_color: command.thumb_color,
                    thumb_outline_color: command.thumb_outline_color,
                    track_color: command.track_color,
                    vertical: command.vertical,
                    is_painted_by_compositor: command.is_painted_by_compositor,
                });
                next_paint_order_index = next_paint_order_index.wrapping_add(1);
            }
            _ => {}
        }
    });
    for scrollbar in &mut async_scrolling_state.scrollbars {
        for scroll_node in &async_scrolling_state.scroll_nodes {
            if scroll_node.scroll_node_index == scrollbar.scroll_node_index {
                scrollbar.scroller_stable_node_id = Some(scroll_node.stable_node_id);
                break;
            }
        }
    }
    async_scrolling_state
}

fn snap_strictness_name(strictness: SnapStrictness) -> &'static str {
    match strictness {
        SnapStrictness::None => "none",
        SnapStrictness::Proximity => "proximity",
        SnapStrictness::Mandatory => "mandatory",
    }
}

fn snap_align_name(align: SnapAlign) -> &'static str {
    match align {
        SnapAlign::None => "none",
        SnapAlign::Start => "start",
        SnapAlign::End => "end",
        SnapAlign::Center => "center",
    }
}

fn rect_to_json(rect: CssPixelRect) -> String {
    format!(
        r#"{{"x":{},"y":{},"width":{},"height":{}}}"#,
        rect.x.to_double(),
        rect.y.to_double(),
        rect.width.to_double(),
        rect.height.to_double()
    )
}

fn point_to_json(point: CssPixelPoint) -> String {
    format!(r#"{{"x":{},"y":{}}}"#, point.x.to_double(), point.y.to_double())
}

fn int_rect_to_json(rect: IntRect) -> String {
    format!(
        r#"{{"x":{},"y":{},"width":{},"height":{}}}"#,
        rect.x, rect.y, rect.width, rect.height
    )
}

fn float_point_to_json(point: FloatPoint) -> String {
    format!(r#"{{"x":{},"y":{}}}"#, point.x, point.y)
}

pub fn async_scrolling_state_to_json(state: &AsyncScrollingState) -> String {
    let mut scroll_nodes = Vec::new();
    for scroll_node in &state.scroll_nodes {
        scroll_nodes.push(format!(
            r#"{{"scrollNodeIndex":{},"parentScrollNodeIndex":{},"isViewport":{},"scrollportRect":{},"minScrollOffset":{},"maxScrollOffset":{}}}"#,
            scroll_node.scroll_node_index.0,
            scroll_node.parent_scroll_node_index.map_or(0, |index| index.0),
            scroll_node.is_viewport,
            int_rect_to_json(scroll_node.scrollport_rect),
            float_point_to_json(scroll_node.min_scroll_offset),
            float_point_to_json(scroll_node.max_scroll_offset)
        ));
    }
    let mut snap_containers = Vec::new();
    for snap_container in &state.snap_containers {
        let mut areas = Vec::new();
        for snap_area in &snap_container.areas {
            areas.push(format!(
                r#"{{"nodeID":{},"pseudoElementType":{},"rect":{},"alignX":"{}","alignY":"{}","alwaysStop":{}}}"#,
                snap_area.identity.node_id.0,
                snap_area.identity.pseudo_element_type,
                rect_to_json(snap_area.rect),
                snap_align_name(snap_area.align_x),
                snap_align_name(snap_area.align_y),
                snap_area.always_stop
            ));
        }
        snap_containers.push(format!(r#"{{"scrollNodeIndex":{},"snapport":{},"minScrollOffset":{},"maxScrollOffset":{},"strictness":"{}","snapsX":{},"snapsY":{},"horizontalWritingMode":{},"areas":[{}]}}"#,
            snap_container.scroll_node_index.0, rect_to_json(snap_container.geometry.snapport), point_to_json(snap_container.geometry.min_scroll_offset),
            point_to_json(snap_container.geometry.max_scroll_offset), snap_strictness_name(snap_container.geometry.strictness), snap_container.geometry.axes.x,
            snap_container.geometry.axes.y, snap_container.geometry.horizontal_writing_mode, areas.join(",")));
    }
    format!(
        r#"{{"documentID":{},"scrollNodeCount":{},"scrollNodes":[{}],"snapContainers":[{}],"devicePixelsPerCssPixel":{},"hasBlockingWheelEventListeners":{},"blockingWheelEventRegionCount":{},"mainThreadWheelEventRegionCount":{},"nestedContextWheelRegionCount":{},"wheelEventListenerStateGeneration":{},"hasBlockingWheelEventRegionCoveringViewport":{}}}"#,
        state
            .document_id
            .map_or_else(|| "null".to_owned(), |id| id.0.to_string()),
        state.scroll_nodes.len(),
        scroll_nodes.join(","),
        snap_containers.join(","),
        if state.device_pixels_per_css_pixel.is_finite() {
            state.device_pixels_per_css_pixel.to_string()
        } else {
            "null".to_owned()
        },
        state.has_blocking_wheel_event_listeners,
        state.blocking_wheel_event_regions.len(),
        state.main_thread_wheel_event_regions.len(),
        state.nested_context_wheel_regions.len(),
        state.wheel_event_listener_state_generation,
        state.has_blocking_wheel_event_region_covering_viewport
    )
}

pub fn wheel_routing_admission_for(state: &AsyncScrollingState) -> WheelRoutingAdmission {
    if state.has_blocking_wheel_event_region_covering_viewport {
        return WheelRoutingAdmission::BlockingWheelEventListeners;
    }
    if state.scroll_nodes.is_empty() {
        return WheelRoutingAdmission::NoScrollNode;
    }
    WheelRoutingAdmission::Accepted
}

// Why a wheel step that the admission does not let the compositor scroll before the page dispatches it is left to the
// page.
pub fn input_routing_decision_for_wheel_step_not_admitted_by(admission: WheelRoutingAdmission) -> InputRoutingDecision {
    match admission {
        WheelRoutingAdmission::Accepted => unreachable!(),
        WheelRoutingAdmission::NoAsyncScrollingState => InputRoutingDecision::NoAsyncScrollingState,
        WheelRoutingAdmission::BlockingWheelEventListeners => InputRoutingDecision::BlockingWheelEventListeners,
        WheelRoutingAdmission::NoScrollNode => InputRoutingDecision::NoScrollNode,
        WheelRoutingAdmission::StaleWheelEventListeners => InputRoutingDecision::StaleWheelEventListeners,
    }
}

// The admission of a scrolling state recorded before the newest change to the page's blocking wheel event listeners is
// stale, as the state may not have the regions that change made.
pub fn wheel_routing_admission_for_with_generation(
    state: &AsyncScrollingState,
    newest_wheel_event_listener_state_generation: u64,
) -> WheelRoutingAdmission {
    if state.wheel_event_listener_state_generation < newest_wheel_event_listener_state_generation {
        return WheelRoutingAdmission::StaleWheelEventListeners;
    }
    wheel_routing_admission_for(state)
}

impl crate::ipc_types::Compositing_RequestedRelativeScroll {
    pub fn applied_to(&self, offset: FloatPoint) -> FloatPoint {
        let clamp = |value: f32, min: f32, max: f32| {
            if value > max {
                max
            } else if value < min {
                min
            } else {
                value
            }
        };
        FloatPoint {
            x: clamp(offset.x + self.delta.x, self.lower_bound.x, self.upper_bound.x),
            y: clamp(offset.y + self.delta.y, self.lower_bound.y, self.upper_bound.y),
        }
    }
}

impl crate::ipc_types::Compositing_RequestedSmoothScroll {
    pub fn relative_scroll(&self) -> crate::ipc_types::Compositing_RequestedRelativeScroll {
        assert!(self.destination_is_relative);
        crate::ipc_types::Compositing_RequestedRelativeScroll {
            delta: self.destination_offset,
            lower_bound: self.relative_lower_bound,
            upper_bound: self.relative_upper_bound,
        }
    }
}
