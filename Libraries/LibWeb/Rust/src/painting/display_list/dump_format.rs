/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The values a display list dump prints, and the names it gives what the tape names by ids that differ from one run of
//! the process to the next.

use super::builder::read_command;
use super::commands::{
    ClipNodeIndex, DisplayListDataSpan, DisplayListGlyph, EffectNodeIndex, SpatialNodeIndex, TextShadowLayer,
};
use super::nested_records::span_bytes;
use super::record_fields::{RecordFieldValue, RecordFields, ResourceKind};
use crate::css::color_resolution::format_to_8bit_compatible;
use crate::painting::dump::{
    push_affine_transform, push_float_like_ak, push_float_point, push_float_rect, push_float_size, push_int_point,
    push_int_rect, push_int_size,
};
use libgfx_rust::filter::Filter;
use libgfx_rust::path::Path;
use libgfx_rust::{
    AffineTransform, Color, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, IntPoint, IntRect, IntSize,
};
use std::collections::HashMap;
use std::fmt::Write;

/// Names the resources of a dump by the order it first prints them in, since their ids count up across the process,
/// and its visual context nodes either the same way, per kind, or by their indices in the tree.
#[derive(Default)]
pub(crate) struct DumpNames {
    resources: HashMap<(ResourceKind, u64), u32>,
    resource_counts: HashMap<ResourceKind, u32>,
    times_of_this_process: HashMap<i64, u32>,
    numbered_nodes: Option<[HashMap<u32, u32>; 3]>,
}

#[derive(Clone, Copy)]
enum NodeKind {
    Spatial,
    Clip,
    Effect,
}

fn resource_kind_name(kind: ResourceKind) -> &'static str {
    match kind {
        ResourceKind::Font => "font",
        ResourceKind::ImageFrame => "image",
        ResourceKind::VideoSink => "video",
        ResourceKind::VectorImage => "svg",
        ResourceKind::Canvas => "canvas",
        ResourceKind::CompositorContext => "context",
    }
}

impl DumpNames {
    pub(crate) fn numbering_nodes_in_print_order() -> Self {
        Self {
            numbered_nodes: Some(Default::default()),
            ..Self::default()
        }
    }

    fn node_number(&mut self, kind: NodeKind, index: u32) -> u32 {
        let Some(numbered_nodes) = &mut self.numbered_nodes else {
            return index;
        };
        let numbers = &mut numbered_nodes[kind as usize];
        let next_number = numbers.len() as u32;
        *numbers.entry(index).or_insert(next_number)
    }

    pub(crate) fn resource(&mut self, kind: ResourceKind, raw: u64) -> String {
        let count = self.resource_counts.entry(kind).or_default();
        let number = *self.resources.entry((kind, raw)).or_insert_with(|| {
            *count += 1;
            *count
        });
        format!("{}{number}", resource_kind_name(kind))
    }

    /// A point in time of the process that recorded the list, which only another such point can be compared with.
    pub(crate) fn time_of_this_process(&mut self, raw: i64) -> String {
        let count = self.times_of_this_process.len() as u32;
        format!("t{}", self.times_of_this_process.entry(raw).or_insert(count + 1))
    }

    pub(crate) fn spatial(&mut self, index: SpatialNodeIndex) -> String {
        format!("s{}", self.node_number(NodeKind::Spatial, index.0))
    }

    pub(crate) fn clip(&mut self, index: ClipNodeIndex) -> String {
        format!("c{}", self.node_number(NodeKind::Clip, index.0))
    }

    pub(crate) fn effect(&mut self, index: EffectNodeIndex) -> String {
        format!("e{}", self.node_number(NodeKind::Effect, index.0))
    }
}

/// A value of a dumped field, with what its bytes mean read out of the payload.
pub(crate) enum DumpValue {
    Bool(bool),
    Integer(i64),
    Float(f32),
    Double(f64),
    Color(Color),
    IntPoint(IntPoint),
    FloatPoint(FloatPoint),
    IntSize(IntSize),
    FloatSize(FloatSize),
    IntRect(IntRect),
    FloatRect(FloatRect),
    CornerRadii(CornerRadii),
    AffineTransform(AffineTransform),
    Name(String),
    SpatialNode(SpatialNodeIndex),
    EffectNode(EffectNodeIndex),
    Resource(ResourceKind, u64),
    TimeOfThisProcess(i64),
    Path(String),
    Filter(Box<Filter>),
    Unreadable(usize),
    Count(usize),
    List(Vec<DumpValue>),
    GradientColorStop(Color, f32),
    Fields(Vec<(&'static str, DumpValue)>),
}

/// The fields of a record or of a struct it holds that hold more than their neutral value, in declaration order. The
/// records nested in a command are left to the dump of those records.
pub(crate) fn dumped_fields(fields: &dyn RecordFields, payload: &[u8]) -> Vec<(&'static str, DumpValue)> {
    let mut dumped = Vec::new();
    fields.visit_record_fields(&mut |name, value, holds_neutral_value| {
        if holds_neutral_value {
            return;
        }
        if let Some(name_and_value) = dumped_record_field(name, value, payload) {
            dumped.push(name_and_value);
        }
    });
    dumped
}

fn is_time_of_this_process(name: &str) -> bool {
    name == "blink_cycle_start_time_ns"
}

fn is_span_of_nested_records(name: &str) -> bool {
    matches!(name, "content" | "mask" | "tile" | "records" | "pattern_tile")
}

pub(crate) fn dumped_record_field(
    name: &'static str,
    value: RecordFieldValue<'_>,
    payload: &[u8],
) -> Option<(&'static str, DumpValue)> {
    let value = match value {
        RecordFieldValue::Bool(value) => DumpValue::Bool(value),
        RecordFieldValue::Integer(value) if is_time_of_this_process(name) => DumpValue::TimeOfThisProcess(value),
        RecordFieldValue::Integer(value) => DumpValue::Integer(value),
        RecordFieldValue::Float(value) => DumpValue::Float(value),
        RecordFieldValue::Double(value) => DumpValue::Double(value),
        RecordFieldValue::Color(value) => DumpValue::Color(value),
        RecordFieldValue::IntPoint(value) => DumpValue::IntPoint(value),
        RecordFieldValue::FloatPoint(value) => DumpValue::FloatPoint(value),
        RecordFieldValue::IntSize(value) => DumpValue::IntSize(value),
        RecordFieldValue::FloatSize(value) => DumpValue::FloatSize(value),
        RecordFieldValue::IntRect(value) => DumpValue::IntRect(value),
        RecordFieldValue::FloatRect(value) => DumpValue::FloatRect(value),
        RecordFieldValue::CornerRadii(value) => DumpValue::CornerRadii(value),
        RecordFieldValue::AffineTransform(value) => DumpValue::AffineTransform(value),
        RecordFieldValue::Enum(value) => DumpValue::Name(format!("{value:?}")),
        RecordFieldValue::SpatialNode(value) => DumpValue::SpatialNode(value),
        RecordFieldValue::EffectNode(value) => DumpValue::EffectNode(value),
        RecordFieldValue::Resource(kind, raw) => DumpValue::Resource(kind, raw),
        RecordFieldValue::Span(_) if is_span_of_nested_records(name) => return None,
        RecordFieldValue::Span(span) => dumped_span(name, span, payload),
        RecordFieldValue::GradientColorStops(stops) => {
            let colors = span_bytes(payload, stops.colors)
                .as_chunks::<{ size_of::<u32>() }>()
                .0
                .iter()
                .map(|bytes| Color(u32::from_ne_bytes(*bytes)));
            let positions = span_bytes(payload, stops.positions)
                .as_chunks::<{ size_of::<f32>() }>()
                .0
                .iter()
                .map(|bytes| f32::from_ne_bytes(*bytes));
            let stops_list = colors
                .zip(positions)
                .map(|(color, position)| DumpValue::GradientColorStop(color, position))
                .collect();
            let name = if stops.repeating { "repeating_color_stops" } else { name };
            return Some((name, DumpValue::List(stops_list)));
        }
        RecordFieldValue::Fields(fields) => DumpValue::Fields(dumped_fields(fields, payload)),
    };
    Some((name, value))
}

fn dumped_span(name: &str, span: DisplayListDataSpan, payload: &[u8]) -> DumpValue {
    let bytes = span_bytes(payload, span);
    match name {
        "glyphs" => DumpValue::Count(bytes.len() / size_of::<DisplayListGlyph>()),
        "layers" => DumpValue::List(
            bytes
                .as_chunks::<{ size_of::<TextShadowLayer>() }>()
                .0
                .iter()
                .map(|layer| DumpValue::Fields(dumped_fields(&read_command::<TextShadowLayer>(layer), payload)))
                .collect(),
        ),
        "runs" => DumpValue::List(
            bytes
                .as_chunks::<{ size_of::<super::commands::Canvas2DGlyphRun>() }>()
                .0
                .iter()
                .map(|run| {
                    DumpValue::Fields(dumped_fields(
                        &read_command::<super::commands::Canvas2DGlyphRun>(run),
                        payload,
                    ))
                })
                .collect(),
        ),
        "path_data" | "shape_path_data" => DumpValue::Path(path_description(&Path::from_serialized_bytes(bytes))),
        "dash_array" => DumpValue::List(
            bytes
                .as_chunks::<{ size_of::<f32>() }>()
                .0
                .iter()
                .map(|value| DumpValue::Float(f32::from_ne_bytes(*value)))
                .collect(),
        ),
        "filter" => match Filter::deserialize(bytes) {
            Some(filter) => DumpValue::Filter(Box::new(filter)),
            None => DumpValue::Unreadable(bytes.len()),
        },
        _ => DumpValue::Unreadable(bytes.len()),
    }
}

/// A path as an SVG path, or as the number of its commands where it holds curves: their control points depend on how
/// the host builds the shapes they round.
pub(crate) fn path_description(path: &Path) -> String {
    let svg_path = path.to_svg_string();
    if svg_path.contains('Q') || svg_path.contains('C') {
        let command_count = svg_path
            .bytes()
            .filter(|byte| matches!(byte, b'M' | b'L' | b'Q' | b'C' | b'Z'))
            .count();
        format!("curved path: {command_count} commands")
    } else {
        format!("path: {svg_path}")
    }
}

pub(crate) fn push_color(output: &mut String, color: Color) {
    match color.alpha() {
        0 => write!(output, "rgba({}, {}, {}, 0)", color.red(), color.green(), color.blue()).unwrap(),
        255 => write!(output, "rgb({}, {}, {})", color.red(), color.green(), color.blue()).unwrap(),
        alpha => {
            let (digits, length) = format_to_8bit_compatible(alpha);
            let fraction = std::str::from_utf8(&digits[..length]).expect("digits are ASCII");
            write!(
                output,
                "rgba({}, {}, {}, 0.{fraction})",
                color.red(),
                color.green(),
                color.blue()
            )
            .unwrap();
        }
    }
}

pub(crate) fn push_corner_radii(output: &mut String, radii: CornerRadii) {
    write!(
        output,
        "[{}x{},{}x{},{}x{},{}x{}]",
        radii.top_left.horizontal_radius,
        radii.top_left.vertical_radius,
        radii.top_right.horizontal_radius,
        radii.top_right.vertical_radius,
        radii.bottom_right.horizontal_radius,
        radii.bottom_right.vertical_radius,
        radii.bottom_left.horizontal_radius,
        radii.bottom_left.vertical_radius
    )
    .unwrap();
}

/// The radii of a clip, by their horizontal radius where every corner is circular.
pub(crate) fn push_clip_radii(output: &mut String, radii: CornerRadii) {
    let corners = [radii.top_left, radii.top_right, radii.bottom_right, radii.bottom_left];
    output.push('(');
    for (index, corner) in corners.iter().enumerate() {
        if index > 0 {
            output.push(',');
        }
        if corners
            .iter()
            .all(|corner| corner.horizontal_radius == corner.vertical_radius)
        {
            write!(output, "{}", corner.horizontal_radius).unwrap();
        } else {
            write!(output, "{}x{}", corner.horizontal_radius, corner.vertical_radius).unwrap();
        }
    }
    output.push(')');
}

pub(crate) fn push_matrix(output: &mut String, matrix: &FloatMatrix4x4) {
    output.push('[');
    for (index, value) in matrix.elements.iter().flatten().enumerate() {
        if index > 0 {
            output.push(',');
        }
        push_float_like_ak(output, *value);
    }
    output.push(']');
}

fn push_float_list(output: &mut String, values: &[f32]) {
    for (index, value) in values.iter().enumerate() {
        if index > 0 {
            output.push(',');
        }
        push_float_like_ak(output, *value);
    }
}

/// A filter graph in the order it applies: a composition as its inner filter then its outer one, each other node as a
/// function of its arguments and its inputs, of which an absent one is what the filter is applied to.
pub(crate) fn push_filter(output: &mut String, filter: &Filter, names: &mut DumpNames) {
    let push_input = |output: &mut String, label: &str, input: &Option<Box<Filter>>, names: &mut DumpNames| {
        if let Some(input) = input {
            write!(output, " {label}=[").unwrap();
            push_filter(output, input, names);
            output.push(']');
        }
    };
    match filter {
        Filter::Compose { outer, inner } => {
            push_filter(output, inner, names);
            output.push(' ');
            push_filter(output, outer, names);
        }
        Filter::Arithmetic {
            background,
            foreground,
            k1,
            k2,
            k3,
            k4,
        } => {
            output.push_str("arithmetic(");
            push_float_list(output, &[*k1, *k2, *k3, *k4]);
            push_input(output, "background", background, names);
            push_input(output, "foreground", foreground, names);
            output.push(')');
        }
        Filter::Blend {
            background,
            foreground,
            mode,
        } => {
            write!(output, "blend({mode:?}").unwrap();
            push_input(output, "background", background, names);
            push_input(output, "foreground", foreground, names);
            output.push(')');
        }
        Filter::Flood { color, opacity } => {
            output.push_str("flood(");
            push_color(output, *color);
            output.push(',');
            push_float_like_ak(output, *opacity);
            output.push(')');
        }
        Filter::DisplacementMap {
            color,
            displacement,
            scale,
            x_channel_selector,
            y_channel_selector,
        } => {
            output.push_str("displacement-map(");
            push_float_like_ak(output, *scale);
            write!(output, ",{x_channel_selector:?},{y_channel_selector:?}").unwrap();
            push_input(output, "color", color, names);
            push_input(output, "displacement", displacement, names);
            output.push(')');
        }
        Filter::DropShadow {
            offset_x,
            offset_y,
            radius,
            color,
            input,
        } => {
            output.push_str("drop-shadow(");
            push_float_list(output, &[*offset_x, *offset_y, *radius]);
            output.push(',');
            push_color(output, *color);
            push_input(output, "input", input, names);
            output.push(')');
        }
        Filter::Blur {
            radius_x,
            radius_y,
            mirrors_at_backdrop_edge,
            input,
        } => {
            output.push_str(if *mirrors_at_backdrop_edge {
                "blur("
            } else {
                "gaussian-blur("
            });
            push_float_list(output, &[*radius_x, *radius_y]);
            push_input(output, "input", input, names);
            output.push(')');
        }
        Filter::ColorFilter { kind, amount, input } => {
            write!(output, "{kind:?}(").unwrap();
            push_float_like_ak(output, *amount);
            push_input(output, "input", input, names);
            output.push(')');
        }
        Filter::ColorMatrix { matrix, input } => {
            output.push_str("color-matrix(");
            push_float_list(output, matrix);
            push_input(output, "input", input, names);
            output.push(')');
        }
        Filter::ColorTable { a, r, g, b, input } => {
            output.push_str("color-table(");
            for (channel, table) in [("a", a), ("r", r), ("g", g), ("b", b)] {
                if let Some(table) = table {
                    write!(output, " {channel}=").unwrap();
                    for value in table.iter() {
                        write!(output, "{value:02x}").unwrap();
                    }
                }
            }
            push_input(output, "input", input, names);
            output.push(')');
        }
        Filter::Saturate { value, input } => {
            output.push_str("saturate(");
            push_float_like_ak(output, *value);
            push_input(output, "input", input, names);
            output.push(')');
        }
        Filter::HueRotate { angle_degrees, input } => {
            output.push_str("hue-rotate(");
            push_float_like_ak(output, *angle_degrees);
            push_input(output, "input", input, names);
            output.push(')');
        }
        Filter::Image {
            frame_id,
            src_rect,
            dest_rect,
            scaling_mode,
        } => {
            write!(output, "image({}", names.resource(ResourceKind::ImageFrame, *frame_id)).unwrap();
            output.push_str(" src_rect=");
            push_int_rect(output, *src_rect);
            output.push_str(" dest_rect=");
            push_float_rect(output, *dest_rect);
            write!(output, " {scaling_mode:?})").unwrap();
        }
        Filter::Merge { inputs } => {
            output.push_str("merge(");
            for (index, input) in inputs.iter().enumerate() {
                if index > 0 {
                    output.push(',');
                }
                match input {
                    Some(input) => {
                        output.push('[');
                        push_filter(output, input, names);
                        output.push(']');
                    }
                    None => output.push_str("source"),
                }
            }
            output.push(')');
        }
        Filter::Offset { dx, dy, input } => {
            output.push_str("offset(");
            push_float_list(output, &[*dx, *dy]);
            push_input(output, "input", input, names);
            output.push(')');
        }
        Filter::Crop { rect, input } => {
            output.push_str("crop(");
            push_float_rect(output, *rect);
            push_input(output, "input", input, names);
            output.push(')');
        }
        Filter::Erode {
            radius_x,
            radius_y,
            input,
        } => {
            output.push_str("erode(");
            push_float_list(output, &[*radius_x, *radius_y]);
            push_input(output, "input", input, names);
            output.push(')');
        }
        Filter::Dilate {
            radius_x,
            radius_y,
            input,
        } => {
            output.push_str("dilate(");
            push_float_list(output, &[*radius_x, *radius_y]);
            push_input(output, "input", input, names);
            output.push(')');
        }
        Filter::Turbulence {
            kind,
            base_frequency_x,
            base_frequency_y,
            num_octaves,
            seed,
            tile_stitch_size,
        } => {
            write!(output, "turbulence({kind:?},").unwrap();
            push_float_list(output, &[*base_frequency_x, *base_frequency_y]);
            write!(output, ",{num_octaves},").unwrap();
            push_float_like_ak(output, *seed);
            output.push(',');
            push_int_size(output, *tile_stitch_size);
            output.push(')');
        }
        Filter::ColorSpaceConversion {
            source_color_space,
            destination_color_space,
            input,
        } => {
            write!(
                output,
                "color-space-conversion({source_color_space:?},{destination_color_space:?}"
            )
            .unwrap();
            push_input(output, "input", input, names);
            output.push(')');
        }
    }
}

/// A filter as its serialized bytes describe it, or the count of bytes that describe none.
pub(crate) fn push_filter_bytes(output: &mut String, bytes: &[u8], names: &mut DumpNames) {
    match Filter::deserialize(bytes) {
        Some(filter) => push_filter(output, &filter, names),
        None => write!(output, "unreadable {} bytes", bytes.len()).unwrap(),
    }
}

pub(crate) fn push_value(output: &mut String, value: &DumpValue, names: &mut DumpNames) {
    match value {
        DumpValue::Bool(value) => write!(output, "{value}").unwrap(),
        DumpValue::Integer(value) => write!(output, "{value}").unwrap(),
        DumpValue::Float(value) => push_float_like_ak(output, *value),
        DumpValue::Double(value) => write!(output, "{value}").unwrap(),
        DumpValue::Color(value) => push_color(output, *value),
        DumpValue::IntPoint(value) => push_int_point(output, *value),
        DumpValue::FloatPoint(value) => push_float_point(output, *value),
        DumpValue::IntSize(value) => push_int_size(output, *value),
        DumpValue::FloatSize(value) => push_float_size(output, *value),
        DumpValue::IntRect(value) => push_int_rect(output, *value),
        DumpValue::FloatRect(value) => push_float_rect(output, *value),
        DumpValue::CornerRadii(value) => push_corner_radii(output, *value),
        DumpValue::AffineTransform(value) => push_affine_transform(output, *value),
        DumpValue::Name(value) => output.push_str(value),
        DumpValue::SpatialNode(index) => output.push_str(&names.spatial(*index)),
        DumpValue::EffectNode(index) => output.push_str(&names.effect(*index)),
        DumpValue::Resource(kind, raw) => output.push_str(&names.resource(*kind, *raw)),
        DumpValue::TimeOfThisProcess(raw) => output.push_str(&names.time_of_this_process(*raw)),
        DumpValue::Path(description) => write!(output, "[{description}]").unwrap(),
        DumpValue::Filter(filter) => {
            output.push('[');
            push_filter(output, filter, names);
            output.push(']');
        }
        DumpValue::Unreadable(byte_count) => write!(output, "[unreadable {byte_count} bytes]").unwrap(),
        DumpValue::Count(count) => write!(output, "{count}").unwrap(),
        DumpValue::List(values) => {
            output.push('[');
            for (index, value) in values.iter().enumerate() {
                if index > 0 {
                    output.push_str(", ");
                }
                push_value(output, value, names);
            }
            output.push(']');
        }
        DumpValue::GradientColorStop(color, position) => {
            push_color(output, *color);
            output.push(' ');
            push_float_like_ak(output, *position);
        }
        DumpValue::Fields(fields) => {
            output.push('{');
            for (index, (name, value)) in fields.iter().enumerate() {
                if index > 0 {
                    output.push(' ');
                }
                write!(output, "{name}=").unwrap();
                push_value(output, value, names);
            }
            output.push('}');
        }
    }
}

/// Appends ` name=value` for each field.
pub(crate) fn push_fields(output: &mut String, fields: &[(&'static str, DumpValue)], names: &mut DumpNames) {
    for (name, value) in fields {
        write!(output, " {name}=").unwrap();
        push_value(output, value, names);
    }
}

/// A float as the shortest decimal that reads back as it, which JSON holds as a number, or as text where it is not
/// finite.
pub(crate) fn json_float(value: f32) -> serde_json::Value {
    let text = crate::painting::dump::format_float_like_ak(value);
    match text.parse::<f64>().ok().and_then(serde_json::Number::from_f64) {
        Some(number) => serde_json::Value::Number(number),
        None => serde_json::Value::String(text),
    }
}

pub(crate) fn json_color(color: Color) -> serde_json::Value {
    serde_json::Value::String(format!(
        "#{:02x}{:02x}{:02x}{:02x}",
        color.red(),
        color.green(),
        color.blue(),
        color.alpha()
    ))
}

pub(crate) fn json_int_rect(rect: IntRect) -> serde_json::Value {
    serde_json::json!([rect.x, rect.y, rect.width, rect.height])
}

pub(crate) fn json_float_rect(rect: FloatRect) -> serde_json::Value {
    serde_json::json!([
        json_float(rect.x),
        json_float(rect.y),
        json_float(rect.width),
        json_float(rect.height)
    ])
}

pub(crate) fn json_float_point(point: FloatPoint) -> serde_json::Value {
    serde_json::json!([json_float(point.x), json_float(point.y)])
}

pub(crate) fn json_corner_radii(radii: CornerRadii) -> serde_json::Value {
    serde_json::Value::Array(
        [radii.top_left, radii.top_right, radii.bottom_right, radii.bottom_left]
            .iter()
            .map(|corner| serde_json::json!([corner.horizontal_radius, corner.vertical_radius]))
            .collect(),
    )
}

/// A name the dump gives what the tape names by an index or id, with that index or id alongside.
pub(crate) fn json_name(name: String, raw: impl Into<serde_json::Value>) -> serde_json::Value {
    serde_json::json!({ "name": name, "raw": raw.into() })
}

pub(crate) fn json_value(value: &DumpValue, names: &mut DumpNames) -> serde_json::Value {
    use serde_json::{Value, json};
    match value {
        DumpValue::Bool(value) => json!(value),
        DumpValue::Integer(value) => json!(value),
        DumpValue::Float(value) => json_float(*value),
        DumpValue::Double(value) => {
            serde_json::Number::from_f64(*value).map_or_else(|| Value::String(value.to_string()), Value::Number)
        }
        DumpValue::Color(value) => json_color(*value),
        DumpValue::IntPoint(value) => json!([value.x, value.y]),
        DumpValue::FloatPoint(value) => json_float_point(*value),
        DumpValue::IntSize(value) => json!([value.width, value.height]),
        DumpValue::FloatSize(value) => json!([json_float(value.width), json_float(value.height)]),
        DumpValue::IntRect(value) => json_int_rect(*value),
        DumpValue::FloatRect(value) => json_float_rect(*value),
        DumpValue::CornerRadii(value) => json_corner_radii(*value),
        DumpValue::AffineTransform(value) => {
            Value::Array(value.values.iter().map(|value| json_float(*value)).collect())
        }
        DumpValue::Name(value) => json!(value),
        DumpValue::SpatialNode(index) => json_name(names.spatial(*index), index.0),
        DumpValue::EffectNode(index) => json_name(names.effect(*index), index.0),
        DumpValue::Resource(kind, raw) => json_name(names.resource(*kind, *raw), *raw),
        DumpValue::TimeOfThisProcess(raw) => json_name(names.time_of_this_process(*raw), *raw),
        DumpValue::Path(description) => json!(description),
        DumpValue::Filter(filter) => {
            let mut text = String::new();
            push_filter(&mut text, filter, names);
            json!(text)
        }
        DumpValue::Unreadable(byte_count) => json!({ "unreadable_bytes": byte_count }),
        DumpValue::Count(count) => json!(count),
        DumpValue::List(values) => Value::Array(values.iter().map(|value| json_value(value, names)).collect()),
        DumpValue::GradientColorStop(color, position) => {
            json!({ "color": json_color(*color), "position": json_float(*position) })
        }
        DumpValue::Fields(fields) => json_fields(fields, names),
    }
}

pub(crate) fn json_fields(fields: &[(&'static str, DumpValue)], names: &mut DumpNames) -> serde_json::Value {
    let mut object = serde_json::Map::new();
    for (name, value) in fields {
        object.insert((*name).to_string(), json_value(value, names));
    }
    serde_json::Value::Object(object)
}
