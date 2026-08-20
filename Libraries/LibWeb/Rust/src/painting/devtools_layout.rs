/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::{
    FlexLayoutClampState, FlexLayoutData, FlexLayoutGrowthState, GridLayoutArea, GridLayoutData, GridLayoutDimension,
    GridLayoutFragment, GridLayoutLine, GridLayoutTrack, GridTrackState, GridTrackType,
};
use serde_json::{Value, json};

fn grid_track_type_name(type_: GridTrackType) -> &'static str {
    match type_ {
        GridTrackType::Explicit => "explicit",
        GridTrackType::Implicit => "implicit",
    }
}

fn grid_track_state_name(state: GridTrackState) -> &'static str {
    match state {
        GridTrackState::Static => "static",
        GridTrackState::Repeat => "repeat",
        GridTrackState::Removed => "removed",
    }
}

fn serialize_grid_line(line: &GridLayoutLine) -> Value {
    json!({
        "breadth": line.breadth.to_double(),
        "names": line.names,
        "negativeNumber": line.negative_number,
        "number": line.number,
        "start": line.start.to_double(),
        "type": grid_track_type_name(line.type_),
    })
}

fn serialize_grid_track(track: &GridLayoutTrack) -> Value {
    json!({
        "breadth": track.breadth.to_double(),
        "start": track.start.to_double(),
        "state": grid_track_state_name(track.state),
        "type": grid_track_type_name(track.type_),
    })
}

fn serialize_grid_area(area: &GridLayoutArea) -> Value {
    json!({
        "columnEnd": area.column_end,
        "columnStart": area.column_start,
        "name": area.name,
        "rowEnd": area.row_end,
        "rowStart": area.row_start,
        "type": grid_track_type_name(area.type_),
    })
}

fn serialize_grid_dimension(dimension: &GridLayoutDimension) -> Value {
    json!({
        "lines": dimension.lines.iter().map(serialize_grid_line).collect::<Vec<_>>(),
        "tracks": dimension.tracks.iter().map(serialize_grid_track).collect::<Vec<_>>(),
    })
}

fn serialize_grid_fragment(fragment: &GridLayoutFragment) -> Value {
    json!({
        "areas": fragment.areas.iter().map(serialize_grid_area).collect::<Vec<_>>(),
        "cols": serialize_grid_dimension(&fragment.columns),
        "rows": serialize_grid_dimension(&fragment.rows),
    })
}

pub(crate) fn serialize_grid_layout(data: &GridLayoutData, container_node_id: i64) -> Vec<u8> {
    serde_json::to_vec(&json!({
        "containerNodeId": container_node_id,
        "direction": crate::css::css_enums::direction::NAMES[data.direction as usize],
        "gridFragments": data.fragments.iter().map(serialize_grid_fragment).collect::<Vec<_>>(),
        "isSubgrid": data.is_subgrid,
        "writingMode": crate::css::css_enums::writing_mode::NAMES[data.writing_mode as usize],
    }))
    .expect("grid layout data serializes to JSON")
}

fn flex_layout_growth_state_name(state: FlexLayoutGrowthState) -> &'static str {
    match state {
        FlexLayoutGrowthState::Growing => "growing",
        FlexLayoutGrowthState::Shrinking => "shrinking",
    }
}

fn flex_layout_clamp_state_name(state: FlexLayoutClampState) -> &'static str {
    match state {
        FlexLayoutClampState::Unclamped => "unclamped",
        FlexLayoutClampState::ClampedToMin => "clamped_to_min",
        FlexLayoutClampState::ClampedToMax => "clamped_to_max",
    }
}

fn axis_direction_name(direction: u8) -> &'static str {
    match direction {
        0 => "horizontal-lr",
        1 => "horizontal-rl",
        2 => "vertical-tb",
        3 => "vertical-bt",
        _ => unreachable!("invalid flex axis direction"),
    }
}

pub(crate) fn serialize_flex_layout(data: &FlexLayoutData, container_node_id: i64) -> Vec<u8> {
    let main_axis_direction = axis_direction_name(data.main_axis_direction);
    let cross_axis_direction = axis_direction_name(data.cross_axis_direction);
    let main_size_property_name = if data.main_axis_direction <= 1 {
        "width"
    } else {
        "height"
    };
    let items = data
        .lines
        .iter()
        .flat_map(|line| {
            line.items.iter().filter_map(|item| {
                let node_id = item.node_id?;
                let sizing = json!({
                    "clampState": flex_layout_clamp_state_name(item.clamp_state),
                    "crossAxisDirection": cross_axis_direction,
                    "crossMaxSize": item.cross_max_size.to_double(),
                    "crossMinSize": item.cross_min_size.to_double(),
                    "lineGrowthState": flex_layout_growth_state_name(line.growth_state),
                    "mainAxisDirection": main_axis_direction,
                    "mainBaseSize": item.main_base_size.to_double(),
                    "mainDeltaSize": item.main_delta_size.to_double(),
                    "mainMaxSize": item.main_max_size.to_double(),
                    "mainMinSize": item.main_min_size.to_double(),
                });
                let mut properties = serde_json::Map::new();
                properties.insert("flex-basis".into(), item.flex_basis.clone().into());
                properties.insert("flex-grow".into(), item.flex_grow.into());
                properties.insert("flex-shrink".into(), item.flex_shrink.into());
                properties.insert(main_size_property_name.into(), item.main_size_property.clone().into());
                properties.insert(
                    format!("min-{main_size_property_name}"),
                    item.main_min_size_property.clone().into(),
                );
                properties.insert(
                    format!("max-{main_size_property_name}"),
                    item.main_max_size_property.clone().into(),
                );
                Some(json!({
                    "nodeId": node_id,
                    "flexItemSizing": sizing,
                    "properties": properties,
                    "computedStyle": {
                        "flexGrow": item.flex_grow,
                        "flexShrink": item.flex_shrink,
                    },
                }))
            })
        })
        .collect::<Vec<_>>();

    serde_json::to_vec(&json!({
        "containerNodeId": container_node_id,
        "properties": {
            "align-content": crate::css::css_enums::align_content::NAMES[data.align_content as usize],
            "align-items": crate::css::css_enums::align_items::NAMES[data.align_items as usize],
            "flex-direction": crate::css::css_enums::flex_direction::NAMES[data.flex_direction as usize],
            "flex-wrap": crate::css::css_enums::flex_wrap::NAMES[data.flex_wrap as usize],
            "justify-content": crate::css::css_enums::justify_content::NAMES[data.justify_content as usize],
        },
        "items": items,
    }))
    .expect("flex layout data serializes to JSON")
}
