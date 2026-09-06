/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Builds the filter graph an SVG `<filter>` element describes from its flattened primitives.
//!
//! The host walks the element's children and hands each supported primitive over as attribute
//! facts; the graph semantics live here: how `in` names resolve, which color space each primitive
//! operates in and where conversions go, how feDropShadow decomposes, and how user units scale to
//! device pixels.

use std::collections::HashMap;

use libgfx_rust::filter::{ChannelSelector, ChannelTable, Filter, TurbulenceType};
use libgfx_rust::{CompositingAndBlendingOperator, IntSize, InterpolationColorSpace};

use crate::painting::host::visual_context::{FfiSvgFilterPrimitiveKind, SvgFilterPrimitiveValues};

// https://drafts.fxtf.org/filter-effects-1/#InterfaceSVGFEColorMatrixElement
const COLOR_MATRIX_TYPE_MATRIX: u16 = 1;
const COLOR_MATRIX_TYPE_SATURATE: u16 = 2;
const COLOR_MATRIX_TYPE_HUE_ROTATE: u16 = 3;
const COLOR_MATRIX_TYPE_LUMINANCE_TO_ALPHA: u16 = 4;

// https://drafts.fxtf.org/filter-effects-1/#InterfaceSVGFECompositeElement
const COMPOSITE_OPERATOR_IN: u8 = 2;
const COMPOSITE_OPERATOR_OUT: u8 = 3;
const COMPOSITE_OPERATOR_ATOP: u8 = 4;
const COMPOSITE_OPERATOR_XOR: u8 = 5;
const COMPOSITE_OPERATOR_ARITHMETIC: u8 = 6;
const COMPOSITE_OPERATOR_LIGHTER: u8 = 7;

// Gfx::MorphologyOperator.
const MORPHOLOGY_OPERATOR_ERODE: u8 = 1;
const MORPHOLOGY_OPERATOR_DILATE: u8 = 2;

// https://drafts.fxtf.org/filter-effects-1/#InterfaceSVGFETurbulenceElement
const TURBULENCE_TYPE_FRACTAL_NOISE: u16 = 1;

// https://drafts.fxtf.org/filter-effects-1/#InterfaceSVGFEDisplacementMapElement
const CHANNEL_SELECTOR_RED: u16 = 1;
const CHANNEL_SELECTOR_GREEN: u16 = 2;
const CHANNEL_SELECTOR_BLUE: u16 = 3;

const SOURCE_GRAPHIC: [u16; 13] = utf16(b"SourceGraphic");
const SOURCE_ALPHA: [u16; 11] = utf16(b"SourceAlpha");

const fn utf16<const N: usize>(ascii: &[u8; N]) -> [u16; N] {
    let mut code_units = [0; N];
    let mut index = 0;
    while index < N {
        code_units[index] = ascii[index] as u16;
        index += 1;
    }
    code_units
}

/// One `<filter>` child as the host hands it over, with the names, lists and tables it borrowed
/// from the element copied so the builder owns what ends up in the graph.
#[derive(Default)]
pub(crate) struct SvgFilterPrimitive {
    pub values: SvgFilterPrimitiveValues,
    pub in1: Vec<u16>,
    pub in2: Vec<u16>,
    pub result: Vec<u16>,
    pub merge_inputs: Vec<Vec<u16>>,
    pub color_matrix_values: Vec<f32>,
    pub component_transfer_tables: [Option<ChannelTable>; 4],
}

/// An intermediate result of the graph: the filter producing it, or the source graphic when
/// absent, and the color space its pixels are in. The default is the source graphic itself.
#[derive(Clone, Default)]
struct FilterResult {
    filter: Option<Filter>,
    color_space: InterpolationColorSpace,
}

impl FilterResult {
    fn new(filter: Filter, color_space: InterpolationColorSpace) -> Self {
        Self {
            filter: Some(filter),
            color_space,
        }
    }

    /// The result's filter with its output converted to `color_space`.
    fn converted_to(self, color_space: InterpolationColorSpace) -> Option<Filter> {
        if self.color_space == color_space {
            return self.filter;
        }
        Some(Filter::ColorSpaceConversion {
            source_color_space: self.color_space,
            destination_color_space: color_space,
            input: boxed(self.filter),
        })
    }
}

const ALPHA_ONLY_MATRIX: [f32; 20] = [
    0.0, 0.0, 0.0, 0.0, 0.0, //
    0.0, 0.0, 0.0, 0.0, 0.0, //
    0.0, 0.0, 0.0, 0.0, 0.0, //
    0.0, 0.0, 0.0, 1.0, 0.0,
];

const LUMINANCE_TO_ALPHA_MATRIX: [f32; 20] = [
    0.0, 0.0, 0.0, 0.0, 0.0, //
    0.0, 0.0, 0.0, 0.0, 0.0, //
    0.0, 0.0, 0.0, 0.0, 0.0, //
    0.2126, 0.7152, 0.0722, 0.0, 0.0,
];

fn boxed(filter: Option<Filter>) -> Option<Box<Filter>> {
    filter.map(Box::new)
}

/// Assembles a `<filter>`'s graph one primitive at a time, in document order.
// https://drafts.fxtf.org/filter-effects-1/#ColorInterpolationFiltersProperty
pub(crate) struct SvgFilterGraphBuilder {
    /// User units to device pixels; the filter's lengths come in the former and Skia works in the
    /// latter. The layer replaying the filter applies the accumulated transform, so only the
    /// device pixel ratio converts here.
    scale: f32,
    results_by_name: HashMap<Vec<u16>, FilterResult>,
    /// The output of the last primitive, which is what an unnamed `in` refers to and what the
    /// filter as a whole produces.
    last_result: FilterResult,
}

impl SvgFilterGraphBuilder {
    pub(crate) fn new(device_pixels_per_css_pixel: f64) -> Self {
        Self {
            scale: device_pixels_per_css_pixel as f32,
            results_by_name: HashMap::new(),
            last_result: FilterResult::default(),
        }
    }

    pub(crate) fn push(&mut self, mut primitive: SvgFilterPrimitive) {
        let result_name = std::mem::take(&mut primitive.result);
        let Some(result) = self.primitive_result(primitive) else {
            return;
        };
        if !result_name.is_empty() {
            self.results_by_name.insert(result_name, result.clone());
        }
        self.last_result = result;
    }

    /// The graph, with its output in sRGB for compositing onto the canvas, or `None` when no
    /// primitive produced anything.
    pub(crate) fn finish(self) -> Option<Filter> {
        self.last_result.converted_to(InterpolationColorSpace::SRGB)
    }

    // https://www.w3.org/TR/filter-effects-1/#element-attrdef-filter-primitive-in
    fn resolve_input(&self, name: &[u16]) -> FilterResult {
        // FIXME: Add missing inputs (BackgroundImage, BackgroundAlpha, FillPaint and StrokePaint).
        if name == SOURCE_GRAPHIC {
            return FilterResult::default();
        }
        if name == SOURCE_ALPHA {
            return FilterResult::new(
                Filter::ColorMatrix {
                    matrix: ALPHA_ONLY_MATRIX,
                    input: None,
                },
                InterpolationColorSpace::SRGB,
            );
        }
        self.results_by_name
            .get(name)
            .cloned()
            .unwrap_or_else(|| self.last_result.clone())
    }

    fn resolve_input_in(&self, name: &[u16], color_space: InterpolationColorSpace) -> Option<Filter> {
        self.resolve_input(name).converted_to(color_space)
    }

    /// The result of one primitive, or `None` for a primitive that produces nothing and leaves the
    /// graph as it was.
    fn primitive_result(&self, primitive: SvgFilterPrimitive) -> Option<FilterResult> {
        let values = primitive.values;
        let space = values.operating_color_space;
        let in_space = |filter| FilterResult::new(filter, space);
        let result = match values.kind {
            FfiSvgFilterPrimitiveKind::Flood => FilterResult::new(
                Filter::Flood {
                    color: values.flood_color,
                    opacity: values.flood_opacity,
                },
                InterpolationColorSpace::SRGB,
            ),
            FfiSvgFilterPrimitiveKind::Blend => in_space(Filter::Blend {
                background: boxed(self.resolve_input_in(&primitive.in2, space)),
                foreground: boxed(self.resolve_input_in(&primitive.in1, space)),
                mode: values.blend_mode,
            }),
            FfiSvgFilterPrimitiveKind::ComponentTransfer => {
                let [a, r, g, b] = primitive.component_transfer_tables;
                in_space(Filter::ColorTable {
                    a,
                    r,
                    g,
                    b,
                    input: boxed(self.resolve_input_in(&primitive.in1, space)),
                })
            }
            FfiSvgFilterPrimitiveKind::Composite => {
                let foreground = boxed(self.resolve_input_in(&primitive.in1, space));
                let background = boxed(self.resolve_input_in(&primitive.in2, space));
                if values.composite_operator == COMPOSITE_OPERATOR_ARITHMETIC {
                    in_space(Filter::Arithmetic {
                        background,
                        foreground,
                        k1: values.k1,
                        k2: values.k2,
                        k3: values.k3,
                        k4: values.k4,
                    })
                } else {
                    in_space(Filter::Blend {
                        background,
                        foreground,
                        mode: composite_operator_to_blend_mode(values.composite_operator),
                    })
                }
            }
            FfiSvgFilterPrimitiveKind::GaussianBlur => in_space(Filter::Blur {
                radius_x: values.std_deviation_x * self.scale,
                radius_y: values.std_deviation_y * self.scale,
                input: boxed(self.resolve_input_in(&primitive.in1, space)),
            }),
            FfiSvgFilterPrimitiveKind::ColorMatrix => {
                let input = self.resolve_input_in(&primitive.in1, space);
                let matrix_values = primitive.color_matrix_values;
                match values.color_matrix_type {
                    COLOR_MATRIX_TYPE_MATRIX => {
                        if let Ok(matrix) = <[f32; 20]>::try_from(matrix_values.as_slice()) {
                            in_space(Filter::ColorMatrix {
                                matrix,
                                input: boxed(input),
                            })
                        } else {
                            // Without a full 4x5 matrix the primitive passes its input through; with no
                            // input of its own it produces nothing.
                            FilterResult::new(input?, space)
                        }
                    }
                    COLOR_MATRIX_TYPE_SATURATE => in_space(Filter::Saturate {
                        value: matrix_values.first().copied().unwrap_or(1.0),
                        input: boxed(input),
                    }),
                    COLOR_MATRIX_TYPE_HUE_ROTATE => in_space(Filter::HueRotate {
                        angle_degrees: matrix_values.first().copied().unwrap_or(0.0),
                        input: boxed(input),
                    }),
                    COLOR_MATRIX_TYPE_LUMINANCE_TO_ALPHA => in_space(Filter::ColorMatrix {
                        matrix: LUMINANCE_TO_ALPHA_MATRIX,
                        input: boxed(input),
                    }),
                    _ => return None,
                }
            }
            FfiSvgFilterPrimitiveKind::Image => FilterResult::new(
                Filter::Image {
                    frame_id: values.image_frame_id,
                    src_rect: values.image_src_rect,
                    dest_rect: values.image_dest_rect,
                    scaling_mode: values.image_scaling_mode,
                },
                InterpolationColorSpace::SRGB,
            ),
            FfiSvgFilterPrimitiveKind::Merge => in_space(Filter::Merge {
                inputs: primitive
                    .merge_inputs
                    .iter()
                    .map(|name| self.resolve_input_in(name, space))
                    .collect(),
            }),
            FfiSvgFilterPrimitiveKind::Morphology => {
                let radius_x = values.radius_x * self.scale;
                let radius_y = values.radius_y * self.scale;
                let input = boxed(self.resolve_input_in(&primitive.in1, space));
                match values.morphology_operator {
                    MORPHOLOGY_OPERATOR_ERODE => in_space(Filter::Erode {
                        radius_x,
                        radius_y,
                        input,
                    }),
                    MORPHOLOGY_OPERATOR_DILATE => in_space(Filter::Dilate {
                        radius_x,
                        radius_y,
                        input,
                    }),
                    _ => unreachable!("the host hands over morphology primitives with a known operator"),
                }
            }
            FfiSvgFilterPrimitiveKind::Offset => {
                // An offset moves pixels without touching their values, so it stays in the color
                // space of its input.
                let input = self.resolve_input(&primitive.in1);
                FilterResult::new(
                    Filter::Offset {
                        dx: values.dx * self.scale,
                        dy: values.dy * self.scale,
                        input: boxed(input.filter),
                    },
                    input.color_space,
                )
            }
            // https://drafts.csswg.org/filter-effects-1/#elementdef-fedropshadow
            FfiSvgFilterPrimitiveKind::DropShadow => {
                let input = self.resolve_input_in(&primitive.in1, space);
                // 1. Take the alpha channel of the input to the feDropShadow filter primitive and the
                //    stdDeviation on the feDropShadow and do processing as if the following
                //    feGaussianBlur was applied:
                //
                // <feGaussianBlur in="alpha-channel-of-feDropShadow-in" stdDeviation="stdDeviation-of-feDropShadow"/>
                let alpha = Filter::ColorMatrix {
                    matrix: ALPHA_ONLY_MATRIX,
                    input: boxed(input.clone()),
                };
                let blurred = Filter::Blur {
                    radius_x: values.std_deviation_x * self.scale,
                    radius_y: values.std_deviation_y * self.scale,
                    input: Some(Box::new(alpha)),
                };
                // 2. Offset the result of step 1 by dx and dy as specified on the feDropShadow element,
                //    equivalent to applying an feOffset with these parameters:
                //
                // <feOffset dx="dx-of-feDropShadow" dy="dy-of-feDropShadow" result="offsetblur"/>
                let offset_blur = Filter::Offset {
                    dx: values.dx * self.scale,
                    dy: values.dy * self.scale,
                    input: Some(Box::new(blurred)),
                };
                // 3. Do processing as if an feFlood element with flood-color and flood-opacity as
                //    specified on the feDropShadow was applied:
                //
                // <feFlood flood-color="flood-color-of-feDropShadow" flood-opacity="flood-opacity-of-feDropShadow"/>
                // NB: The flood color is specified in the sRGB color space and must be converted to
                //     the operating space.
                let shadow_color = FilterResult::new(
                    Filter::Flood {
                        color: values.flood_color,
                        opacity: values.flood_opacity,
                    },
                    InterpolationColorSpace::SRGB,
                )
                .converted_to(space);
                // 4. Composite the result of the feFlood in step 3 with the result of the feOffset in
                //    step 2 as if an feComposite filter primitive with operator="in" was applied:
                //
                // <feComposite in2="offsetblur" operator="in"/>
                let colored_shadow = Filter::Blend {
                    background: Some(Box::new(offset_blur)),
                    foreground: boxed(shadow_color),
                    mode: CompositingAndBlendingOperator::SourceIn,
                };
                // 5. Finally merge the result of the previous step, doing processing as if the
                //    following feMerge was performed:
                //
                // <feMerge>
                //   <feMergeNode/>
                //   <feMergeNode in="in-of-feDropShadow"/>
                // </feMerge>
                in_space(Filter::Merge {
                    inputs: vec![Some(colored_shadow), input],
                })
            }
            FfiSvgFilterPrimitiveKind::Turbulence => in_space(Filter::Turbulence {
                kind: if values.turbulence_type == TURBULENCE_TYPE_FRACTAL_NOISE {
                    TurbulenceType::FractalNoise
                } else {
                    TurbulenceType::Turbulence
                },
                base_frequency_x: values.base_frequency_x / self.scale,
                base_frequency_y: values.base_frequency_y / self.scale,
                num_octaves: values.num_octaves,
                seed: values.seed,
                tile_stitch_size: IntSize {
                    width: (values.stitch_tile_width * self.scale).round() as i32,
                    height: (values.stitch_tile_height * self.scale).round() as i32,
                },
            }),
            FfiSvgFilterPrimitiveKind::DisplacementMap => {
                // The displaced pixels keep the values of the color input, so the result stays in
                // that input's color space.
                let color = self.resolve_input(&primitive.in1);
                let displacement = self.resolve_input_in(&primitive.in2, space);
                FilterResult::new(
                    Filter::DisplacementMap {
                        color: boxed(color.filter),
                        displacement: boxed(displacement),
                        // FIXME: Skia's displacement map takes a single scale factor, so the
                        //        horizontal scale applies to both axes.
                        scale: values.scale * self.scale,
                        x_channel_selector: channel_selector(values.x_channel_selector),
                        y_channel_selector: channel_selector(values.y_channel_selector),
                    },
                    color.color_space,
                )
            }
        };
        Some(result)
    }
}

fn composite_operator_to_blend_mode(composite_operator: u8) -> CompositingAndBlendingOperator {
    match composite_operator {
        COMPOSITE_OPERATOR_IN => CompositingAndBlendingOperator::SourceIn,
        COMPOSITE_OPERATOR_OUT => CompositingAndBlendingOperator::DestinationOut,
        COMPOSITE_OPERATOR_ATOP => CompositingAndBlendingOperator::SourceATop,
        COMPOSITE_OPERATOR_XOR => CompositingAndBlendingOperator::Xor,
        COMPOSITE_OPERATOR_LIGHTER => CompositingAndBlendingOperator::Lighter,
        _ => CompositingAndBlendingOperator::SourceOver,
    }
}

fn channel_selector(idl_value: u16) -> ChannelSelector {
    match idl_value {
        CHANNEL_SELECTOR_RED => ChannelSelector::Red,
        CHANNEL_SELECTOR_GREEN => ChannelSelector::Green,
        CHANNEL_SELECTOR_BLUE => ChannelSelector::Blue,
        _ => ChannelSelector::Alpha,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libgfx_rust::Color;

    fn name(ascii: &str) -> Vec<u16> {
        ascii.encode_utf16().collect()
    }

    fn primitive(
        kind: FfiSvgFilterPrimitiveKind,
        operating_color_space: InterpolationColorSpace,
    ) -> SvgFilterPrimitive {
        SvgFilterPrimitive {
            values: SvgFilterPrimitiveValues {
                kind,
                operating_color_space,
                ..Default::default()
            },
            ..Default::default()
        }
    }

    fn blur(input: &str, std_deviation: f32, result: &str) -> SvgFilterPrimitive {
        let mut blur = primitive(FfiSvgFilterPrimitiveKind::GaussianBlur, InterpolationColorSpace::SRGB);
        blur.in1 = name(input);
        blur.result = name(result);
        blur.values.std_deviation_x = std_deviation;
        blur.values.std_deviation_y = std_deviation;
        blur
    }

    fn graph(
        primitives: impl IntoIterator<Item = SvgFilterPrimitive>,
        device_pixels_per_css_pixel: f64,
    ) -> Option<Filter> {
        let mut builder = SvgFilterGraphBuilder::new(device_pixels_per_css_pixel);
        for primitive in primitives {
            builder.push(primitive);
        }
        builder.finish()
    }

    #[test]
    fn an_empty_filter_produces_nothing() {
        assert_eq!(graph([], 1.0), None);
    }

    #[test]
    fn the_last_primitive_is_the_output_and_is_converted_back_to_srgb() {
        let mut blur = blur("SourceGraphic", 2.0, "");
        blur.values.operating_color_space = InterpolationColorSpace::LinearRGB;
        assert_eq!(
            graph([blur], 2.0),
            Some(Filter::ColorSpaceConversion {
                source_color_space: InterpolationColorSpace::LinearRGB,
                destination_color_space: InterpolationColorSpace::SRGB,
                input: Some(Box::new(Filter::Blur {
                    radius_x: 4.0,
                    radius_y: 4.0,
                    input: Some(Box::new(Filter::ColorSpaceConversion {
                        source_color_space: InterpolationColorSpace::SRGB,
                        destination_color_space: InterpolationColorSpace::LinearRGB,
                        input: None,
                    })),
                })),
            })
        );
    }

    #[test]
    fn named_results_resolve_and_unnamed_inputs_take_the_previous_result() {
        let mut merge = primitive(FfiSvgFilterPrimitiveKind::Merge, InterpolationColorSpace::SRGB);
        merge.merge_inputs = vec![name("first"), name("SourceAlpha"), name("missing")];

        let first_blur = Filter::blur(1.0, 1.0, None);
        let second_blur = Filter::blur(2.0, 2.0, Some(first_blur.clone()));
        assert_eq!(
            graph([blur("SourceGraphic", 1.0, "first"), blur("", 2.0, ""), merge], 1.0),
            Some(Filter::Merge {
                inputs: vec![
                    Some(first_blur),
                    Some(Filter::ColorMatrix {
                        matrix: ALPHA_ONLY_MATRIX,
                        input: None
                    }),
                    Some(second_blur),
                ],
            })
        );
    }

    #[test]
    fn a_color_matrix_without_a_full_matrix_passes_its_input_through() {
        let matrix = || {
            let mut matrix = primitive(FfiSvgFilterPrimitiveKind::ColorMatrix, InterpolationColorSpace::SRGB);
            matrix.values.color_matrix_type = COLOR_MATRIX_TYPE_MATRIX;
            matrix.color_matrix_values = vec![1.0, 2.0];
            matrix
        };
        assert_eq!(graph([matrix()], 1.0), None);
        assert_eq!(
            graph([blur("SourceGraphic", 1.0, ""), matrix()], 1.0),
            Some(Filter::blur(1.0, 1.0, None))
        );
    }

    #[test]
    fn a_drop_shadow_decomposes_into_the_spec_primitives() {
        let mut drop_shadow = primitive(FfiSvgFilterPrimitiveKind::DropShadow, InterpolationColorSpace::SRGB);
        drop_shadow.in1 = name("SourceGraphic");
        drop_shadow.values.dx = 1.0;
        drop_shadow.values.dy = 2.0;
        drop_shadow.values.std_deviation_x = 3.0;
        drop_shadow.values.std_deviation_y = 3.0;
        drop_shadow.values.flood_color = Color::from_rgb(0, 0, 0);
        drop_shadow.values.flood_opacity = 0.5;

        let shadow = Filter::Blend {
            background: Some(Box::new(Filter::Offset {
                dx: 1.0,
                dy: 2.0,
                input: Some(Box::new(Filter::Blur {
                    radius_x: 3.0,
                    radius_y: 3.0,
                    input: Some(Box::new(Filter::ColorMatrix {
                        matrix: ALPHA_ONLY_MATRIX,
                        input: None,
                    })),
                })),
            })),
            foreground: Some(Box::new(Filter::Flood {
                color: Color::from_rgb(0, 0, 0),
                opacity: 0.5,
            })),
            mode: CompositingAndBlendingOperator::SourceIn,
        };
        assert_eq!(
            graph([drop_shadow], 1.0),
            Some(Filter::Merge {
                inputs: vec![Some(shadow), None]
            })
        );
    }

    #[test]
    fn turbulence_frequencies_scale_inversely_to_lengths() {
        let mut turbulence = primitive(FfiSvgFilterPrimitiveKind::Turbulence, InterpolationColorSpace::SRGB);
        turbulence.values.turbulence_type = TURBULENCE_TYPE_FRACTAL_NOISE;
        turbulence.values.base_frequency_x = 0.5;
        turbulence.values.base_frequency_y = 1.0;
        turbulence.values.num_octaves = 2;
        turbulence.values.seed = 3.0;
        turbulence.values.stitch_tile_width = 10.5;
        turbulence.values.stitch_tile_height = 4.0;
        assert_eq!(
            graph([turbulence], 2.0),
            Some(Filter::Turbulence {
                kind: TurbulenceType::FractalNoise,
                base_frequency_x: 0.25,
                base_frequency_y: 0.5,
                num_octaves: 2,
                seed: 3.0,
                tile_stitch_size: IntSize { width: 21, height: 8 },
            })
        );
    }
}
