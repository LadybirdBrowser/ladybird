/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelPoint;
use crate::layout::node_data::{DomPaintFact, NodeKind, NodeSlotId};
use crate::painting::chrome_geometry::{PhysicalAxes, maximum_scroll_offset, minimum_scroll_offset};
use crate::painting::display_list::commands::UniqueNodeId;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::paint_passes::FfiCompositorScrollRefreshOutcome;
use crate::painting::paint_read::PaintRead;
use crate::painting::{paintable_geometry, scroll_snap, style_queries};
use libcompositing_rust::visual_context::{
    CompositorScrollNodeKind, CompositorScrollNodeRecord, CompositorSnapAreaRecord, CompositorSnapContainerRecord,
    SpatialData, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
};
use libgfx_rust::{FloatPoint, IntRect, IntSize};

pub(crate) fn css_point_to_device_point(point: CssPixelPoint, device_pixels_per_css_pixel: f64) -> FloatPoint {
    let scale = device_pixels_per_css_pixel as f32;
    FloatPoint {
        x: point.x.to_float() * scale,
        y: point.y.to_float() * scale,
    }
}

pub(crate) fn compositor_scroll_node_kind(
    source: &impl PaintRead,
    paintable: NodeSlotId,
    mut wheel_scrollable_axes: impl FnMut(NodeSlotId) -> PhysicalAxes,
) -> Option<CompositorScrollNodeKind> {
    if source.node_has_dom_paint_fact(paintable, DomPaintFact::NestedNavigableContainer)
        || source.paintable_data(paintable).own_scroll_node_index == VISUAL_VIEWPORT_NODE_INDEX
    {
        return None;
    }
    let is_viewport = source.node_kind_if_live(paintable) == Some(NodeKind::Viewport);
    let wheel_scrollable_axes = wheel_scrollable_axes(paintable);
    if !(wheel_scrollable_axes.horizontal
        || wheel_scrollable_axes.vertical
        || minimum_scroll_offset(source, paintable) != maximum_scroll_offset(source, paintable)
        || (!is_viewport && crate::painting::chrome_geometry::keeps_scrolls_along_some_axis(source, paintable)))
    {
        return None;
    }
    if is_viewport {
        Some(CompositorScrollNodeKind::Viewport)
    } else if source.node_generated_for(paintable) != 0 {
        Some(CompositorScrollNodeKind::PseudoElement)
    } else if source.node_is_element_backed(paintable) {
        Some(CompositorScrollNodeKind::Element)
    } else {
        None
    }
}

pub(crate) fn nearest_ancestor_with_compositor_scroll_node(
    source: &impl PaintRead,
    paintable: NodeSlotId,
    mut wheel_scrollable_axes: impl FnMut(NodeSlotId) -> PhysicalAxes,
) -> Option<NodeSlotId> {
    let mut candidate = source.paintable_data(paintable).containing_block;
    while !candidate.is_invalid() && source.paintable_row_is_populated(candidate) {
        if compositor_scroll_node_kind(source, candidate, &mut wheel_scrollable_axes).is_some() {
            return Some(candidate);
        }
        if style_queries::is_fixed_position(source, candidate) {
            return None;
        }
        candidate = source.paintable_data(candidate).containing_block;
    }
    None
}

pub(crate) fn compositor_scroll_node_record(
    source: &impl PaintRead,
    paintable: NodeSlotId,
    scroll_node_kind: CompositorScrollNodeKind,
    device_pixels_per_css_pixel: f64,
    device_viewport_size: IntSize,
    mut wheel_scrollable_axes: impl FnMut(NodeSlotId) -> PhysicalAxes,
) -> CompositorScrollNodeRecord {
    let generated_for = source.node_generated_for(paintable);
    let node_identity = source.paintable_data(paintable).node_identity;
    debug_assert!(
        node_identity != 0,
        "a scroll node's identity is resolved by the visual context update"
    );
    let parent_scroll_node_index =
        match nearest_ancestor_with_compositor_scroll_node(source, paintable, &mut wheel_scrollable_axes) {
            Some(ancestor) => source.paintable_data(ancestor).own_scroll_node_index,
            None => VISUAL_VIEWPORT_NODE_INDEX,
        };
    let is_viewport = source.node_kind_if_live(paintable) == Some(NodeKind::Viewport);
    let scrollport_rect = if is_viewport {
        IntRect::new(0, 0, device_viewport_size.width, device_viewport_size.height)
    } else {
        DevicePixelConverter::new(device_pixels_per_css_pixel)
            .rounded_device_rect(paintable_geometry::absolute_padding_box_rect(source, paintable))
    };
    let scale = device_pixels_per_css_pixel;
    let wheel_scrollable_axes = wheel_scrollable_axes(paintable);
    let scroll_boundary_behavior = crate::painting::chrome_geometry::scroll_boundary_behavior(source, paintable);
    CompositorScrollNodeRecord {
        scrollable_node_id: UniqueNodeId(node_identity),
        scroll_node_index: source.paintable_data(paintable).own_scroll_node_index,
        parent_scroll_node_index,
        scrollport_rect,
        min_scroll_offset: css_point_to_device_point(minimum_scroll_offset(source, paintable), scale),
        max_scroll_offset: css_point_to_device_point(maximum_scroll_offset(source, paintable), scale),
        scroll_node_kind,
        pseudo_element_type: generated_for.saturating_sub(1),
        is_viewport,
        can_be_wheel_scrolled_horizontally: wheel_scrollable_axes.horizontal,
        can_be_wheel_scrolled_vertically: wheel_scrollable_axes.vertical,
        chains_scroll_to_ancestors_horizontally: scroll_boundary_behavior.chains_to_ancestors.horizontal,
        chains_scroll_to_ancestors_vertically: scroll_boundary_behavior.chains_to_ancestors.vertical,
        shows_overscroll_effect_horizontally: scroll_boundary_behavior.shows_overscroll_effect.horizontal,
        shows_overscroll_effect_vertically: scroll_boundary_behavior.shows_overscroll_effect.vertical,
    }
}

// The compositor selects snap positions for the scrolls it performs from the same geometry the
// main thread selects from, kept in the tree beside the scroll node it belongs to.
pub(crate) fn compositor_snap_container_and_area_records(
    source: &impl PaintRead,
    paintable: NodeSlotId,
) -> Option<(CompositorSnapContainerRecord, Vec<CompositorSnapAreaRecord>)> {
    let geometry = scroll_snap::snap_container_geometry(source, paintable)?;
    let container = CompositorSnapContainerRecord {
        scroll_node_index: source.paintable_data(paintable).own_scroll_node_index,
        snapport: geometry.snapport,
        min_scroll_offset: geometry.min_scroll_offset,
        max_scroll_offset: geometry.max_scroll_offset,
        strictness: geometry.strictness,
        snaps_x: geometry.axes.x,
        snaps_y: geometry.axes.y,
        horizontal_writing_mode: geometry.horizontal_writing_mode,
        first_area_index: 0,
        area_count: 0,
    };
    let mut areas = Vec::new();
    scroll_snap::for_each_snap_area(source, paintable, |_, area| {
        areas.push(CompositorSnapAreaRecord {
            area_node_id: UniqueNodeId(area.node_id),
            pseudo_element_type: area.pseudo_element_type,
            rect: area.rect,
            align_x: area.align_x,
            align_y: area.align_y,
            always_stop: area.always_stop,
        });
    });
    Some((container, areas))
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct CompositorScrollInputs {
    pub(crate) device_pixels_per_css_pixel: f64,
    pub(crate) device_viewport_size: IntSize,
}

pub(crate) fn refresh_compositor_scroll_table(
    arena: &mut crate::layout::LayoutNodeArena,
    state: &mut super::VisualContextState,
    inputs: CompositorScrollInputs,
) -> FfiCompositorScrollRefreshOutcome {
    use crate::painting::record::damage::PaintDamage;
    use libcompositing_rust::visual_context::CompositorScrollTable;
    use std::sync::Arc;

    let Some(tree) = state.tree.as_mut() else {
        return FfiCompositorScrollRefreshOutcome::default();
    };
    let previous = tree.compositor_scroll_table();
    let inputs_changed = state.last_compositor_scroll_inputs != Some(inputs);
    let new_damage = state.compositor_snap_damage_revision != Some(arena.paint_damage_push_revision());
    let damage_everywhere = arena.paint_damage_covers_everything() || arena.scroll_metadata_damaged_everywhere();
    let source = arena.paintable_rows();
    let wheel_axes = |slot| crate::painting::chrome_geometry::wheel_scrollable_axes(&source, slot);
    let mut nodes_with_owners = Vec::new();
    for (index, spatial_node) in tree.spatial_nodes.iter().enumerate() {
        let SpatialData::Scroll(scroll) = &spatial_node.data else {
            continue;
        };
        let owner = scroll.owner_paintable;
        if !source.paintable_row_is_populated(owner) {
            continue;
        }
        if let Some(kind) = compositor_scroll_node_kind(&source, owner, wheel_axes) {
            let node = compositor_scroll_node_record(
                &source,
                owner,
                kind,
                inputs.device_pixels_per_css_pixel,
                inputs.device_viewport_size,
                wheel_axes,
            );
            debug_assert_eq!(node.scroll_node_index, SpatialNodeIndex(index as u32));
            nodes_with_owners.push((node, owner));
        }
    }
    nodes_with_owners.sort_unstable_by_key(|(node, _)| node.scroll_node_index);
    let mut containers = Vec::new();
    let mut areas = Vec::new();
    let mut damaged_subtrees = Vec::new();
    for (node, owner) in &nodes_with_owners {
        let previous_node = previous
            .nodes
            .binary_search_by_key(&node.scroll_node_index, |node| node.scroll_node_index)
            .ok()
            .map(|index| &previous.nodes[index]);
        let same_identity = previous_node.is_some_and(|before| {
            before.scrollable_node_id == node.scrollable_node_id
                && before.scroll_node_kind == node.scroll_node_kind
                && before.pseudo_element_type == node.pseudo_element_type
        });
        if !same_identity
            || previous_node.is_some_and(|before| before.parent_scroll_node_index != node.parent_scroll_node_index)
        {
            damaged_subtrees.push(*owner);
        }
        let recompute = inputs_changed
            || !same_identity
            || (new_damage && (damage_everywhere || !arena.row_paint_state(*owner).damage().is_empty()));
        let snap_records = if recompute {
            compositor_snap_container_and_area_records(&source, *owner)
        } else {
            previous
                .snap_containers
                .binary_search_by_key(&node.scroll_node_index, |container| container.scroll_node_index)
                .ok()
                .map(|index| {
                    let container = previous.snap_containers[index];
                    let first = container.first_area_index as usize;
                    let end = first + container.area_count as usize;
                    (container, previous.snap_areas[first..end].to_vec())
                })
        };
        if let Some((mut container, container_areas)) = snap_records {
            container.first_area_index = areas.len().try_into().expect("snap area index fits in u32");
            container.area_count = container_areas.len().try_into().expect("snap area count fits in u32");
            areas.extend(container_areas);
            containers.push(container);
        }
    }
    for (before, owner) in previous.nodes.iter().zip(&state.compositor_scroll_owners) {
        if !nodes_with_owners
            .binary_search_by_key(&before.scroll_node_index, |(node, _)| node.scroll_node_index)
            .is_ok_and(|index| nodes_with_owners[index].1 == *owner)
            && source.paintable_row_is_populated(*owner)
        {
            damaged_subtrees.push(*owner);
        }
    }
    state.compositor_scroll_owners = nodes_with_owners.iter().map(|(_, owner)| *owner).collect();
    let refreshed = CompositorScrollTable {
        nodes: nodes_with_owners
            .into_iter()
            .map(|(node, _)| node)
            .collect::<Vec<_>>()
            .into(),
        snap_containers: containers.into(),
        snap_areas: areas.into(),
    };
    let table_changed = *previous != refreshed;
    if table_changed {
        Arc::make_mut(tree).set_compositor_scroll_table(refreshed);
    }
    let requires_display_list_recording = !damaged_subtrees.is_empty();
    for owner in damaged_subtrees {
        arena.push_paint_damage_to_paint_subtree(owner, PaintDamage::SCROLL_METADATA);
    }
    state.last_compositor_scroll_inputs = Some(inputs);
    state.compositor_snap_damage_revision = Some(arena.paint_damage_push_revision());
    FfiCompositorScrollRefreshOutcome {
        table_changed,
        requires_display_list_recording,
        ..Default::default()
    }
}
