/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Filter graphs as a painter builds them: every node after the nodes it reads, and every color filter lowered to the
//! color matrix it applies.

use crate::fast_hash::fast_hasher;
use ak::Span;
use libgfx_rust::filter::{ChannelSelector, Filter, TurbulenceType};
use libgfx_rust::{
    Color, ColorFilterType, CompositingAndBlendingOperator, IntRect, IntSize, InterpolationColorSpace, ScalingMode,
};
use std::hash::Hasher;

/// The input of a node that names no other node: the image the graph filters.
pub const FILTER_SOURCE_GRAPHIC: u32 = 0xFFFF_FFFF;

/// One node of a filter graph. Each input is the index of an earlier node, or FILTER_SOURCE_GRAPHIC. A color table
/// holds 256 bytes, or is null for a channel the node leaves as it is. The inputs of a merge are `input_count` entries
/// of the graph's merge inputs, from `first_input`.
#[derive(Clone, Copy, Debug)]
#[repr(C, u8)]
#[rustfmt::skip]
pub enum FilterNode {
    Arithmetic { background: u32, foreground: u32, k1: f32, k2: f32, k3: f32, k4: f32 },
    Compose { outer: u32, inner: u32 },
    Blend { background: u32, foreground: u32, mode: CompositingAndBlendingOperator },
    Flood { color: Color, opacity: f32 },
    DisplacementMap {
        color: u32, displacement: u32, scale: f32, x_channel: ChannelSelector, y_channel: ChannelSelector,
    },
    DropShadow { input: u32, offset_x: f32, offset_y: f32, radius: f32, color: Color },
    Blur { input: u32, radius_x: f32, radius_y: f32, mirrors_at_backdrop_edge: bool },
    ColorMatrix { input: u32, matrix: [f32; 20], clamps_to_unit_range: bool },
    ColorTable { input: u32, alpha: *const u8, red: *const u8, green: *const u8, blue: *const u8 },
    Saturate { input: u32, value: f32 },
    Image { frame_id: u64, src_rect: IntRect, dest_rect: IntRect, scaling_mode: ScalingMode },
    Merge { first_input: u32, input_count: u32 },
    Offset { input: u32, dx: f32, dy: f32 },
    Erode { input: u32, radius_x: f32, radius_y: f32 },
    Dilate { input: u32, radius_x: f32, radius_y: f32 },
    Turbulence {
        kind: TurbulenceType, base_frequency_x: f32, base_frequency_y: f32, num_octaves: i32, seed: f32,
        tile_stitch_size: IntSize,
    },
    ColorSpaceConversion { input: u32, source: InterpolationColorSpace, destination: InterpolationColorSpace },
}

/// A filter graph: its nodes, with the one whose output is the graph's last, or none for no filter. Two graphs with
/// the same content hash are the same graph.
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct FilterGraph<'a> {
    pub nodes: Span<'a, FilterNode>,
    pub merge_inputs: Span<'a, u32>,
    pub content_hash: u64,
}

/// The nodes of a serialized graph, which holds what they point to.
#[derive(Default)]
pub struct LoweredFilterGraph {
    // The color tables of the nodes point into it.
    filter: Option<Filter>,
    nodes: Vec<FilterNode>,
    merge_inputs: Vec<u32>,
    content_hash: u64,
}

impl LoweredFilterGraph {
    /// Lowers the serialized graph in `bytes`, which are empty for no filter.
    pub fn read(bytes: &[u8]) -> Self {
        let mut graph = Self::default();
        if bytes.is_empty() {
            return graph;
        }
        graph.filter = Filter::deserialize(bytes);
        debug_assert!(
            graph.filter.is_some(),
            "validation lets only well-formed filter graphs through"
        );
        if let Some(filter) = graph.filter.take() {
            graph.lower(&filter);
            graph.filter = Some(filter);
        }
        let mut hasher = fast_hasher();
        hasher.write(bytes);
        graph.content_hash = hasher.finish();
        graph
    }

    pub fn graph(&self) -> FilterGraph<'_> {
        FilterGraph {
            nodes: Span::new(&self.nodes),
            merge_inputs: Span::new(&self.merge_inputs),
            content_hash: self.content_hash,
        }
    }

    #[expect(
        clippy::ref_option,
        reason = "every optional input of a Filter is an Option<Box<Filter>>"
    )]
    fn input(&mut self, input: &Option<Box<Filter>>) -> u32 {
        input
            .as_deref()
            .map_or(FILTER_SOURCE_GRAPHIC, |input| self.lower(input))
    }

    // Lowers the inputs of `filter`, then the filter itself, and returns the index of its node.
    #[rustfmt::skip]
    fn lower(&mut self, filter: &Filter) -> u32 {
        let node = match filter {
            Filter::Arithmetic { background, foreground, k1, k2, k3, k4 } => FilterNode::Arithmetic {
                background: self.input(background), foreground: self.input(foreground),
                k1: *k1, k2: *k2, k3: *k3, k4: *k4,
            },
            Filter::Compose { outer, inner } => {
                FilterNode::Compose { outer: self.lower(outer), inner: self.lower(inner) }
            }
            Filter::Blend { background, foreground, mode } => FilterNode::Blend {
                background: self.input(background), foreground: self.input(foreground), mode: *mode,
            },
            Filter::Flood { color, opacity } => FilterNode::Flood { color: *color, opacity: *opacity },
            Filter::DisplacementMap { color, displacement, scale, x_channel_selector, y_channel_selector } => {
                FilterNode::DisplacementMap {
                    color: self.input(color), displacement: self.input(displacement), scale: *scale,
                    x_channel: *x_channel_selector, y_channel: *y_channel_selector,
                }
            }
            Filter::DropShadow { offset_x, offset_y, radius, color, input } => FilterNode::DropShadow {
                input: self.input(input), offset_x: *offset_x, offset_y: *offset_y, radius: *radius, color: *color,
            },
            Filter::Blur { radius_x, radius_y, mirrors_at_backdrop_edge, input } => FilterNode::Blur {
                input: self.input(input), radius_x: *radius_x, radius_y: *radius_y,
                mirrors_at_backdrop_edge: *mirrors_at_backdrop_edge,
            },
            Filter::ColorFilter { kind, amount, input } => {
                let (matrix, clamps_to_unit_range) = color_filter_matrix(*kind, *amount);
                FilterNode::ColorMatrix { input: self.input(input), matrix, clamps_to_unit_range }
            }
            Filter::ColorMatrix { matrix, input } => FilterNode::ColorMatrix {
                input: self.input(input), matrix: *matrix, clamps_to_unit_range: true,
            },
            Filter::ColorTable { a, r, g, b, input } => {
                let table = |table: &Option<Box<[u8; 256]>>| table.as_ref().map_or(std::ptr::null(), |t| t.as_ptr());
                let input = self.input(input);
                FilterNode::ColorTable { input, alpha: table(a), red: table(r), green: table(g), blue: table(b) }
            }
            Filter::Saturate { value, input } => FilterNode::Saturate { input: self.input(input), value: *value },
            Filter::HueRotate { angle_degrees, input } => FilterNode::ColorMatrix {
                input: self.input(input), matrix: hue_rotation_matrix(*angle_degrees), clamps_to_unit_range: false,
            },
            Filter::Image { frame_id, src_rect, dest_rect, scaling_mode } => FilterNode::Image {
                frame_id: *frame_id, src_rect: *src_rect, dest_rect: *dest_rect, scaling_mode: *scaling_mode,
            },
            Filter::Merge { inputs } => {
                let inputs: Vec<u32> = inputs
                    .iter()
                    .map(|input| input.as_ref().map_or(FILTER_SOURCE_GRAPHIC, |input| self.lower(input)))
                    .collect();
                let first_input = self.merge_inputs.len() as u32;
                self.merge_inputs.extend(&inputs);
                FilterNode::Merge { first_input, input_count: inputs.len() as u32 }
            }
            Filter::Offset { dx, dy, input } => FilterNode::Offset { input: self.input(input), dx: *dx, dy: *dy },
            Filter::Erode { radius_x, radius_y, input } => FilterNode::Erode {
                input: self.input(input), radius_x: *radius_x, radius_y: *radius_y,
            },
            Filter::Dilate { radius_x, radius_y, input } => FilterNode::Dilate {
                input: self.input(input), radius_x: *radius_x, radius_y: *radius_y,
            },
            Filter::Turbulence { kind, base_frequency_x, base_frequency_y, num_octaves, seed, tile_stitch_size } => {
                FilterNode::Turbulence {
                    kind: *kind, base_frequency_x: *base_frequency_x, base_frequency_y: *base_frequency_y,
                    num_octaves: *num_octaves, seed: *seed, tile_stitch_size: *tile_stitch_size,
                }
            }
            Filter::ColorSpaceConversion { source_color_space, destination_color_space, input } => {
                FilterNode::ColorSpaceConversion {
                    input: self.input(input), source: *source_color_space, destination: *destination_color_space,
                }
            }
        };
        self.nodes.push(node);
        (self.nodes.len() - 1) as u32
    }
}

// The color matrix that maps red, green and blue through `rgb` and adds `offset` to each, keeping alpha.
fn rgb_matrix(rgb: [[f32; 3]; 3], offset: f32) -> [f32; 20] {
    let [r, g, b] = rgb;
    #[rustfmt::skip]
    let matrix = [
        r[0], r[1], r[2], 0.0, offset,
        g[0], g[1], g[2], 0.0, offset,
        b[0], b[1], b[2], 0.0, offset,
        0.0, 0.0, 0.0, 1.0, 0.0,
    ];
    matrix
}

fn scaling_rgb_by(scale: f32) -> [[f32; 3]; 3] {
    [[scale, 0.0, 0.0], [0.0, scale, 0.0], [0.0, 0.0, scale]]
}

// https://drafts.fxtf.org/filter-effects-1/#FilterPrimitiveRepresentation
// The matrix of a color filter function, and whether its results are clamped to the unit range.
fn color_filter_matrix(kind: ColorFilterType, amount: f32) -> ([f32; 20], bool) {
    let inverse = 1.0 - amount;
    match kind {
        ColorFilterType::Grayscale => {
            let rgb = [
                [
                    0.2126 + 0.7874 * inverse,
                    0.7152 - 0.7152 * inverse,
                    0.0722 - 0.0722 * inverse,
                ],
                [
                    0.2126 - 0.2126 * inverse,
                    0.7152 + 0.2848 * inverse,
                    0.0722 - 0.0722 * inverse,
                ],
                [
                    0.2126 - 0.2126 * inverse,
                    0.7152 - 0.7152 * inverse,
                    0.0722 + 0.9278 * inverse,
                ],
            ];
            (rgb_matrix(rgb, 0.0), true)
        }
        ColorFilterType::Brightness => (rgb_matrix(scaling_rgb_by(amount), 0.0), false),
        ColorFilterType::Contrast => (rgb_matrix(scaling_rgb_by(amount), -(0.5 * amount) + 0.5), false),
        ColorFilterType::Invert => (rgb_matrix(scaling_rgb_by(1.0 - 2.0 * amount), amount), true),
        ColorFilterType::Opacity => {
            let mut matrix = rgb_matrix(scaling_rgb_by(1.0), 0.0);
            matrix[18] = amount;
            (matrix, true)
        }
        ColorFilterType::Sepia => {
            let rgb = [
                [
                    0.393 + 0.607 * inverse,
                    0.769 - 0.769 * inverse,
                    0.189 - 0.189 * inverse,
                ],
                [
                    0.349 - 0.349 * inverse,
                    0.686 + 0.314 * inverse,
                    0.168 - 0.168 * inverse,
                ],
                [
                    0.272 - 0.272 * inverse,
                    0.534 - 0.534 * inverse,
                    0.131 + 0.869 * inverse,
                ],
            ];
            (rgb_matrix(rgb, 0.0), true)
        }
        ColorFilterType::Saturate => {
            let rgb = [
                [0.213 + 0.787 * amount, 0.715 - 0.715 * amount, 0.072 - 0.072 * amount],
                [0.213 - 0.213 * amount, 0.715 + 0.285 * amount, 0.072 - 0.072 * amount],
                [0.213 - 0.213 * amount, 0.715 - 0.715 * amount, 0.072 + 0.928 * amount],
            ];
            (rgb_matrix(rgb, 0.0), false)
        }
    }
}

// https://drafts.fxtf.org/filter-effects-1/#feColorMatrixElement
fn hue_rotation_matrix(angle_degrees: f32) -> [f32; 20] {
    let radians = angle_degrees * std::f32::consts::PI / 180.0;
    let (cos, sin) = (ak::math::cos(radians), ak::math::sin(radians));
    rgb_matrix(
        [
            [
                0.213 + cos * 0.787 - sin * 0.213,
                0.715 - cos * 0.715 - sin * 0.715,
                0.072 - cos * 0.072 + sin * 0.928,
            ],
            [
                0.213 - cos * 0.213 + sin * 0.143,
                0.715 + cos * 0.285 + sin * 0.140,
                0.072 - cos * 0.072 - sin * 0.283,
            ],
            [
                0.213 - cos * 0.213 - sin * 0.787,
                0.715 - cos * 0.715 + sin * 0.715,
                0.072 + cos * 0.928 + sin * 0.072,
            ],
        ],
        0.0,
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lowered(filter: &Filter) -> Vec<FilterNode> {
        LoweredFilterGraph::read(&filter.serialize()).nodes
    }

    #[test]
    fn inputs_come_before_the_nodes_that_read_them() {
        let filter = Filter::compose(
            Filter::blur(2.0, 3.0, None),
            Filter::color(ColorFilterType::Grayscale, 1.0, None),
        );
        let nodes = lowered(&filter);
        assert_eq!(nodes.len(), 3);
        assert!(matches!(
            nodes[0],
            FilterNode::Blur {
                input: FILTER_SOURCE_GRAPHIC,
                ..
            }
        ));
        assert!(matches!(
            nodes[1],
            FilterNode::ColorMatrix {
                input: FILTER_SOURCE_GRAPHIC,
                clamps_to_unit_range: true,
                ..
            }
        ));
        assert!(matches!(nodes[2], FilterNode::Compose { outer: 0, inner: 1 }));
    }

    #[test]
    fn a_merge_lists_its_inputs_in_order() {
        let filter = Filter::Merge {
            inputs: vec![Some(Filter::blur(1.0, 1.0, None)), None],
        };
        let LoweredFilterGraph {
            nodes, merge_inputs, ..
        } = LoweredFilterGraph::read(&filter.serialize());
        assert!(matches!(
            nodes[1],
            FilterNode::Merge {
                first_input: 0,
                input_count: 2
            }
        ));
        assert_eq!(merge_inputs, [0, FILTER_SOURCE_GRAPHIC]);
    }
}
