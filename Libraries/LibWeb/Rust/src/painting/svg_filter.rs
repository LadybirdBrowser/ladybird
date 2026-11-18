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
use libgfx_rust::{CompositingAndBlendingOperator, FloatRect, IntSize, InterpolationColorSpace};

use crate::layout::svg_formatting_context::FfiSvgNumberPercentage;
use crate::painting::host::visual_context::{
    FfiSvgFilterPrimitiveKind, FfiSvgFilterRegion, SUBREGION_SPECIFIES_HEIGHT, SUBREGION_SPECIFIES_WIDTH,
    SUBREGION_SPECIFIES_X, SUBREGION_SPECIFIES_Y, SvgFilterPrimitiveValues,
};

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
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct SvgFilterPrimitive {
    pub values: SvgFilterPrimitiveValues,
    pub in1: Vec<u16>,
    pub in2: Vec<u16>,
    pub result: Vec<u16>,
    pub merge_inputs: Vec<Vec<u16>>,
    pub color_matrix_values: Vec<f32>,
    pub component_transfer_tables: [Option<ChannelTable>; 4],
    pub image_frame: Option<libgfx_rust::image_frame::ImageFrameHandle>,
}

/// Where an element that a `<filter>` applies to puts the filter: what the filter region and the
/// primitive subregions resolve against, in the space the element paints in before the device
/// pixel scale.
#[derive(Clone, Copy, Debug)]
pub(crate) struct SvgFilterTarget {
    pub region: FfiSvgFilterRegion,
    pub object_bounding_box: FloatRect,
    /// The origin of the userSpaceOnUse coordinate system.
    pub user_space_origin: (f32, f32),
    /// The size that userSpaceOnUse percentages resolve against.
    pub user_space_viewport: (f32, f32),
}

impl SvgFilterTarget {
    /// A rect given in `units_are_object_bounding_box` units, in the target's paint space.
    // https://svgwg.org/svg2-draft/coords.html#ObjectBoundingBoxUnits
    fn resolve(
        &self,
        units_are_object_bounding_box: bool,
        x: FfiSvgNumberPercentage,
        y: FfiSvgNumberPercentage,
        width: FfiSvgNumberPercentage,
        height: FfiSvgNumberPercentage,
    ) -> (f32, f32, f32, f32) {
        if units_are_object_bounding_box {
            // A number and a percentage alike are a fraction of the bounding box.
            let bounding_box = self.object_bounding_box;
            return (
                bounding_box.x + x.value * bounding_box.width,
                bounding_box.y + y.value * bounding_box.height,
                width.value * bounding_box.width,
                height.value * bounding_box.height,
            );
        }
        let (viewport_width, viewport_height) = self.user_space_viewport;
        (
            self.user_space_origin.0 + x.resolve_relative_to(viewport_width),
            self.user_space_origin.1 + y.resolve_relative_to(viewport_height),
            width.resolve_relative_to(viewport_width),
            height.resolve_relative_to(viewport_height),
        )
    }
}

/// An intermediate result of the graph: the filter producing it, or the source graphic when
/// absent, the color space its pixels are in, and the subregion it covers, in device pixels,
/// where the graph knows its target. The default is the source graphic itself.
#[derive(Clone, Default)]
struct FilterResult {
    filter: Option<Filter>,
    color_space: InterpolationColorSpace,
    subregion: Option<FloatRect>,
}

impl FilterResult {
    fn new(filter: Filter, color_space: InterpolationColorSpace) -> Self {
        Self {
            filter: Some(filter),
            color_space,
            subregion: None,
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
    image_rendering: u8,
    /// Where an feImage draws when the graph doesn't know its target.
    image_rect_without_target: Option<FloatRect>,
    /// The filter region, in device pixels, once the graph knows its target. Without one, the graph
    /// neither clips to a region nor to subregions, and has nowhere to draw an feImage.
    filter_region: Option<FloatRect>,
    target: Option<SvgFilterTarget>,
    results_by_name: HashMap<Vec<u16>, FilterResult>,
    /// The output of the last primitive, which is what an unnamed `in` refers to and what the
    /// filter as a whole produces.
    last_result: FilterResult,
}

impl SvgFilterGraphBuilder {
    pub(crate) fn new(device_pixels_per_css_pixel: f64) -> Self {
        Self {
            scale: device_pixels_per_css_pixel as f32,
            image_rendering: 0,
            image_rect_without_target: None,
            filter_region: None,
            target: None,
            results_by_name: HashMap::new(),
            last_result: FilterResult::default(),
        }
    }

    pub(crate) fn set_target(&mut self, target: SvgFilterTarget, image_rendering: u8) {
        let region = target.region;
        let (x, y, width, height) = target.resolve(
            region.filter_units_are_object_bounding_box,
            region.x,
            region.y,
            region.width,
            region.height,
        );
        self.filter_region = Some(self.to_device_rect(x, y, width, height));
        self.target = Some(target);
        self.image_rendering = image_rendering;
    }

    /// A horizontal length of a primitive, such as a blur's x standard deviation, in device pixels.
    // https://drafts.csswg.org/filter-effects-1/#element-attrdef-filter-primitiveunits
    // If primitiveUnits="objectBoundingBox", then any length values within the filter definitions represent fractions or
    // percentages of the bounding box on the referencing element.
    fn horizontal_length(&self, length: f32) -> f32 {
        match self.target {
            Some(target) if target.region.primitive_units_are_object_bounding_box => {
                length * target.object_bounding_box.width * self.scale
            }
            _ => length * self.scale,
        }
    }

    /// A vertical length of a primitive, in device pixels; see [`Self::horizontal_length`].
    fn vertical_length(&self, length: f32) -> f32 {
        match self.target {
            Some(target) if target.region.primitive_units_are_object_bounding_box => {
                length * target.object_bounding_box.height * self.scale
            }
            _ => length * self.scale,
        }
    }

    /// Draws an feImage over `rect` for a graph that has no target to resolve subregions against.
    pub(crate) fn set_image_rect_without_target(&mut self, rect: FloatRect, image_rendering: u8) {
        self.image_rect_without_target = Some(rect);
        self.image_rendering = image_rendering;
    }

    fn to_device_rect(&self, x: f32, y: f32, width: f32, height: f32) -> FloatRect {
        FloatRect::new(x * self.scale, y * self.scale, width * self.scale, height * self.scale)
    }

    pub(crate) fn push(&mut self, mut primitive: SvgFilterPrimitive) {
        let result_name = std::mem::take(&mut primitive.result);
        let subregion = self.subregion_of(&primitive);
        let result = match subregion {
            // https://drafts.csswg.org/filter-effects-1/#FilterPrimitiveSubRegion
            // If the filter primitive subregion has a negative or zero width or height, the effect of the filter
            // primitive is disabled.
            // NB: Its result is then transparent black, which the rest of the graph still takes as an input.
            Some(subregion) if subregion.width <= 0.0 || subregion.height <= 0.0 => FilterResult {
                filter: Some(Filter::Crop {
                    rect: FloatRect::new(0.0, 0.0, 0.0, 0.0),
                    input: None,
                }),
                color_space: InterpolationColorSpace::SRGB,
                subregion: Some(subregion),
            },
            _ => {
                let Some(mut result) = self.primitive_result(primitive, subregion) else {
                    return;
                };
                // The filter primitive subregion acts as a hard clip clipping rectangle on the filter primitive result.
                // All intermediate offscreens are defined to not exceed the intersection of the filter primitive
                // subregion with the filter region.
                // NB: The result keeps the subregion as defined rather than as clipped, since that's what a later
                //     primitive's default subregion is the union of.
                if let (Some(subregion), Some(filter_region)) = (subregion, self.filter_region) {
                    result.filter = Some(Filter::Crop {
                        rect: subregion.intersected(filter_region),
                        input: boxed(result.filter),
                    });
                    result.subregion = Some(subregion);
                }
                result
            }
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

    /// The source graphic, clipped to the filter region.
    // https://drafts.csswg.org/filter-effects-1/#FilterPrimitiveSubRegion
    // The filter region acts as a hard clip clipping rectangle on the filter primitive's input image(s).
    fn source_graphic(&self) -> Option<Filter> {
        self.filter_region.map(|region| Filter::Crop {
            rect: region,
            input: None,
        })
    }

    fn is_standard_input(name: &[u16]) -> bool {
        name == SOURCE_GRAPHIC || name == SOURCE_ALPHA
    }

    // https://www.w3.org/TR/filter-effects-1/#element-attrdef-filter-primitive-in
    fn resolve_input(&self, name: &[u16]) -> FilterResult {
        // FIXME: Add missing inputs (BackgroundImage, BackgroundAlpha, FillPaint and StrokePaint).
        if name == SOURCE_GRAPHIC {
            return FilterResult {
                filter: self.source_graphic(),
                color_space: InterpolationColorSpace::SRGB,
                subregion: self.filter_region,
            };
        }
        if name == SOURCE_ALPHA {
            return FilterResult {
                filter: Some(Filter::ColorMatrix {
                    matrix: ALPHA_ONLY_MATRIX,
                    input: boxed(self.source_graphic()),
                }),
                color_space: InterpolationColorSpace::SRGB,
                subregion: self.filter_region,
            };
        }
        if let Some(result) = self.results_by_name.get(name) {
            return result.clone();
        }
        // An unnamed or unknown input takes the previous result, which before the first primitive is the source
        // graphic.
        if self.last_result.filter.is_none() && self.last_result.subregion.is_none() {
            return self.resolve_input(&SOURCE_GRAPHIC);
        }
        self.last_result.clone()
    }

    /// The names of the inputs `primitive` reads.
    fn input_names(primitive: &SvgFilterPrimitive) -> Vec<&[u16]> {
        match primitive.values.kind {
            FfiSvgFilterPrimitiveKind::Flood
            | FfiSvgFilterPrimitiveKind::Image
            | FfiSvgFilterPrimitiveKind::Turbulence => Vec::new(),
            FfiSvgFilterPrimitiveKind::Blend
            | FfiSvgFilterPrimitiveKind::Composite
            | FfiSvgFilterPrimitiveKind::DisplacementMap => vec![&primitive.in1, &primitive.in2],
            FfiSvgFilterPrimitiveKind::Merge => primitive.merge_inputs.iter().map(Vec::as_slice).collect(),
            FfiSvgFilterPrimitiveKind::ColorMatrix
            | FfiSvgFilterPrimitiveKind::ComponentTransfer
            | FfiSvgFilterPrimitiveKind::DropShadow
            | FfiSvgFilterPrimitiveKind::GaussianBlur
            | FfiSvgFilterPrimitiveKind::Morphology
            | FfiSvgFilterPrimitiveKind::Offset => vec![&primitive.in1],
        }
    }

    /// The filter primitive subregion of `primitive`, in device pixels, or `None` where the graph
    /// doesn't know its target.
    // https://drafts.csswg.org/filter-effects-1/#FilterPrimitiveSubRegion
    fn subregion_of(&self, primitive: &SvgFilterPrimitive) -> Option<FloatRect> {
        let target = self.target?;
        let filter_region = self.filter_region?;
        // x, y, width and height default to the union (i.e., tightest fitting bounding box) of the subregions defined
        // for all referenced nodes. If there are no referenced nodes (e.g., for feImage or feTurbulence), or one or
        // more of the referenced nodes is a standard input (one of SourceGraphic, SourceAlpha, BackgroundImage,
        // BackgroundAlpha, FillPaint or StrokePaint), or for feTile (which is special because its principal function
        // is to replicate the referenced node in X and Y and thereby produce a usually larger result), the default
        // subregion is 0%, 0%, 100%, 100%, where as a special-case the percentages are relative to the dimensions of
        // the filter region, thus making the default filter primitive subregion equal to the filter region.
        let input_names = Self::input_names(primitive);
        let default_subregion =
            if input_names.is_empty() || input_names.iter().any(|name| Self::is_standard_input(name)) {
                filter_region
            } else {
                input_names
                    .iter()
                    .map(|name| self.resolve_input(name).subregion.unwrap_or(filter_region))
                    .reduce(FloatRect::united)
                    .unwrap_or(filter_region)
            };

        let values = &primitive.values;
        let units_are_object_bounding_box = target.region.primitive_units_are_object_bounding_box;
        let (x, y, width, height) = target.resolve(
            units_are_object_bounding_box,
            values.subregion_x,
            values.subregion_y,
            values.subregion_width,
            values.subregion_height,
        );
        let specified = |bit| values.subregion_specified & bit != 0;
        let resolved = self.to_device_rect(x, y, width, height);
        Some(FloatRect::new(
            if specified(SUBREGION_SPECIFIES_X) {
                resolved.x
            } else {
                default_subregion.x
            },
            if specified(SUBREGION_SPECIFIES_Y) {
                resolved.y
            } else {
                default_subregion.y
            },
            if specified(SUBREGION_SPECIFIES_WIDTH) {
                resolved.width
            } else {
                default_subregion.width
            },
            if specified(SUBREGION_SPECIFIES_HEIGHT) {
                resolved.height
            } else {
                default_subregion.height
            },
        ))
    }

    fn resolve_input_in(&self, name: &[u16], color_space: InterpolationColorSpace) -> Option<Filter> {
        self.resolve_input(name).converted_to(color_space)
    }

    /// The result of one primitive, or `None` for a primitive that produces nothing and leaves the
    /// graph as it was.
    fn primitive_result(&self, primitive: SvgFilterPrimitive, subregion: Option<FloatRect>) -> Option<FilterResult> {
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
            FfiSvgFilterPrimitiveKind::GaussianBlur => in_space(Filter::gaussian_blur(
                self.horizontal_length(values.std_deviation_x),
                self.vertical_length(values.std_deviation_y),
                self.resolve_input_in(&primitive.in1, space),
            )),
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
            // https://drafts.csswg.org/filter-effects-1/#feImageElement
            // The image is rendered according to the behavior of the image element, which fits it into the filter
            // primitive subregion as its preserveAspectRatio attribute says.
            FfiSvgFilterPrimitiveKind::Image => {
                // An image that can't be rendered produces transparent black.
                let Some(frame) = primitive.image_frame.as_ref() else {
                    return Some(FilterResult::new(
                        Filter::Crop {
                            rect: FloatRect::new(0.0, 0.0, 0.0, 0.0),
                            input: None,
                        },
                        InterpolationColorSpace::SRGB,
                    ));
                };
                let src_rect = values.image_src_rect;
                let natural_size = if values.image_natural_width > 0.0 && values.image_natural_height > 0.0 {
                    (values.image_natural_width, values.image_natural_height)
                } else {
                    (src_rect.width as f32, src_rect.height as f32)
                };
                let Some(subregion) = subregion else {
                    // An image drawn over the box without a subregion still fits it as its preserveAspectRatio says,
                    // and is cut to the box like any other primitive's result, before a later primitive moves it.
                    let box_rect = self.image_rect_without_target?;
                    let dest_rect = crate::painting::record::paint::svg::image_draw_rect(
                        box_rect,
                        natural_size,
                        values.image_preserve_aspect_ratio_align,
                        values.image_preserve_aspect_ratio_meet_or_slice,
                    );
                    return Some(FilterResult::new(
                        Filter::Crop {
                            rect: box_rect,
                            input: Some(Box::new(Filter::Image {
                                frame_id: frame.id(),
                                src_rect,
                                dest_rect,
                                scaling_mode: crate::painting::record::paint::background::to_gfx_scaling_mode(
                                    self.image_rendering,
                                    (src_rect.width, src_rect.height),
                                    (dest_rect.width.round() as i32, dest_rect.height.round() as i32),
                                ),
                            })),
                        },
                        InterpolationColorSpace::SRGB,
                    ));
                };
                let draw_rect = crate::painting::record::paint::svg::image_draw_rect(
                    subregion,
                    natural_size,
                    values.image_preserve_aspect_ratio_align,
                    values.image_preserve_aspect_ratio_meet_or_slice,
                );
                FilterResult::new(
                    Filter::Image {
                        frame_id: frame.id(),
                        src_rect,
                        dest_rect: draw_rect,
                        scaling_mode: crate::painting::record::paint::background::to_gfx_scaling_mode(
                            self.image_rendering,
                            (src_rect.width, src_rect.height),
                            (draw_rect.width.round() as i32, draw_rect.height.round() as i32),
                        ),
                    },
                    InterpolationColorSpace::SRGB,
                )
            }
            FfiSvgFilterPrimitiveKind::Merge => in_space(Filter::Merge {
                inputs: primitive
                    .merge_inputs
                    .iter()
                    .map(|name| self.resolve_input_in(name, space))
                    .collect(),
            }),
            FfiSvgFilterPrimitiveKind::Morphology => {
                let radius_x = self.horizontal_length(values.radius_x);
                let radius_y = self.vertical_length(values.radius_y);
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
                        dx: self.horizontal_length(values.dx),
                        dy: self.vertical_length(values.dy),
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
                let blurred = Filter::gaussian_blur(
                    self.horizontal_length(values.std_deviation_x),
                    self.vertical_length(values.std_deviation_y),
                    Some(alpha),
                );
                // 2. Offset the result of step 1 by dx and dy as specified on the feDropShadow element,
                //    equivalent to applying an feOffset with these parameters:
                //
                // <feOffset dx="dx-of-feDropShadow" dy="dy-of-feDropShadow" result="offsetblur"/>
                let offset_blur = Filter::Offset {
                    dx: self.horizontal_length(values.dx),
                    dy: self.vertical_length(values.dy),
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
                // https://drafts.csswg.org/filter-effects-1/#element-attrdef-feturbulence-stitchtiles
                // stitch: the user agent will automatically attempt to synthesize Perlin noise functions that are
                // continuous at the border of the filter primitive subregion.
                tile_stitch_size: match subregion {
                    Some(subregion) if values.turbulence_stitches_tiles => IntSize {
                        width: subregion.width.round() as i32,
                        height: subregion.height.round() as i32,
                    },
                    _ => IntSize {
                        width: (values.stitch_tile_width * self.scale).round() as i32,
                        height: (values.stitch_tile_height * self.scale).round() as i32,
                    },
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
                        scale: self.horizontal_length(values.scale),
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

    fn number(value: f32) -> FfiSvgNumberPercentage {
        FfiSvgNumberPercentage {
            value,
            is_percentage: false,
        }
    }

    // A 100x50 box at (10, 20), and a filter region of x=0, y=0, width=200%, height=200% around it.
    fn target(primitive_units_are_object_bounding_box: bool) -> SvgFilterTarget {
        SvgFilterTarget {
            region: FfiSvgFilterRegion {
                filter_units_are_object_bounding_box: true,
                primitive_units_are_object_bounding_box,
                x: number(0.0),
                y: number(0.0),
                width: number(2.0),
                height: number(2.0),
            },
            object_bounding_box: FloatRect::new(10.0, 20.0, 100.0, 50.0),
            user_space_origin: (10.0, 20.0),
            user_space_viewport: (100.0, 50.0),
        }
    }

    fn targeted_graph(
        target: SvgFilterTarget,
        primitives: impl IntoIterator<Item = SvgFilterPrimitive>,
    ) -> Option<Filter> {
        let mut builder = SvgFilterGraphBuilder::new(2.0);
        builder.set_target(target, 0);
        for primitive in primitives {
            builder.push(primitive);
        }
        builder.finish()
    }

    fn flood() -> SvgFilterPrimitive {
        let mut flood = primitive(FfiSvgFilterPrimitiveKind::Flood, InterpolationColorSpace::SRGB);
        flood.values.flood_opacity = 1.0;
        flood
    }

    fn with_subregion(
        mut primitive: SvgFilterPrimitive,
        x: f32,
        y: f32,
        width: f32,
        height: f32,
    ) -> SvgFilterPrimitive {
        primitive.values.subregion_x = number(x);
        primitive.values.subregion_y = number(y);
        primitive.values.subregion_width = number(width);
        primitive.values.subregion_height = number(height);
        primitive.values.subregion_specified =
            SUBREGION_SPECIFIES_X | SUBREGION_SPECIFIES_Y | SUBREGION_SPECIFIES_WIDTH | SUBREGION_SPECIFIES_HEIGHT;
        primitive
    }

    fn flood_filter() -> Filter {
        Filter::Flood {
            color: Default::default(),
            opacity: 1.0,
        }
    }

    #[test]
    fn a_primitive_without_inputs_fills_the_filter_region() {
        assert_eq!(
            targeted_graph(target(false), [flood()]),
            Some(Filter::Crop {
                rect: FloatRect::new(20.0, 40.0, 400.0, 200.0),
                input: Some(Box::new(flood_filter())),
            })
        );
    }

    #[test]
    fn a_subregion_resolves_in_primitive_units() {
        // Fractions of the bounding box under objectBoundingBox, user units from the box's origin under userSpaceOnUse.
        assert_eq!(
            targeted_graph(target(true), [with_subregion(flood(), 0.25, 0.5, 0.5, 0.25)]),
            Some(Filter::Crop {
                rect: FloatRect::new(70.0, 90.0, 100.0, 25.0),
                input: Some(Box::new(flood_filter())),
            })
        );
        assert_eq!(
            targeted_graph(target(false), [with_subregion(flood(), 5.0, 5.0, 20.0, 10.0)]),
            Some(Filter::Crop {
                rect: FloatRect::new(30.0, 50.0, 40.0, 20.0),
                input: Some(Box::new(flood_filter())),
            })
        );
    }

    #[test]
    fn an_empty_subregion_disables_the_primitive_but_not_the_graph() {
        let mut empty_flood = with_subregion(flood(), 0.0, 0.0, 0.0, 10.0);
        empty_flood.result = name("empty");
        let mut merge = primitive(FfiSvgFilterPrimitiveKind::Merge, InterpolationColorSpace::SRGB);
        merge.merge_inputs = vec![name("empty"), name("SourceGraphic")];
        let region = FloatRect::new(20.0, 40.0, 400.0, 200.0);
        assert_eq!(
            targeted_graph(target(false), [empty_flood, merge]),
            Some(Filter::Crop {
                rect: region,
                input: Some(Box::new(Filter::Merge {
                    inputs: vec![
                        Some(Filter::Crop {
                            rect: FloatRect::new(0.0, 0.0, 0.0, 0.0),
                            input: None,
                        }),
                        Some(Filter::Crop {
                            rect: region,
                            input: None,
                        }),
                    ],
                })),
            })
        );
    }

    #[test]
    fn a_default_subregion_is_the_union_of_the_input_subregions() {
        let mut half = with_subregion(flood(), 0.0, 0.0, 50.0, 50.0);
        half.result = name("half");
        let half_rect = FloatRect::new(20.0, 40.0, 100.0, 100.0);
        assert_eq!(
            targeted_graph(target(false), [half, blur("half", 1.0, "")]),
            Some(Filter::Crop {
                rect: half_rect,
                input: Some(Box::new(Filter::gaussian_blur(
                    2.0,
                    2.0,
                    Some(Filter::Crop {
                        rect: half_rect,
                        input: Some(Box::new(flood_filter())),
                    }),
                ))),
            })
        );
    }

    #[test]
    fn a_default_subregion_takes_the_input_subregion_as_defined_not_as_clipped() {
        // The flood's subregion reaches past the left edge of the filter region, which clips its output but not the
        // width the blur's subregion defaults to.
        let mut overhanging = with_subregion(flood(), -50.0, 0.0, 100.0, 100.0);
        overhanging.result = name("overhanging");
        let mut shifted = blur("overhanging", 1.0, "");
        shifted.values.subregion_x = number(0.0);
        shifted.values.subregion_specified = SUBREGION_SPECIFIES_X;
        assert_eq!(
            targeted_graph(target(false), [overhanging, shifted]),
            Some(Filter::Crop {
                rect: FloatRect::new(20.0, 40.0, 200.0, 200.0),
                input: Some(Box::new(Filter::gaussian_blur(
                    2.0,
                    2.0,
                    Some(Filter::Crop {
                        rect: FloatRect::new(20.0, 40.0, 100.0, 200.0),
                        input: Some(Box::new(flood_filter())),
                    }),
                ))),
            })
        );
    }

    #[test]
    fn bounding_box_primitive_units_scale_lengths_by_the_box() {
        let crop_of = |filter| Filter::Crop {
            rect: FloatRect::new(20.0, 40.0, 400.0, 200.0),
            input: Some(Box::new(filter)),
        };
        let source = Filter::Crop {
            rect: FloatRect::new(20.0, 40.0, 400.0, 200.0),
            input: None,
        };
        let mut blur = blur("SourceGraphic", 0.1, "");
        blur.values.std_deviation_y = 0.2;
        assert_eq!(
            targeted_graph(target(true), [blur]),
            Some(crop_of(Filter::gaussian_blur(20.0, 20.0, Some(source))))
        );
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
                input: Some(Box::new(Filter::gaussian_blur(
                    4.0,
                    4.0,
                    Some(Filter::ColorSpaceConversion {
                        source_color_space: InterpolationColorSpace::SRGB,
                        destination_color_space: InterpolationColorSpace::LinearRGB,
                        input: None,
                    }),
                ))),
            })
        );
    }

    #[test]
    fn named_results_resolve_and_unnamed_inputs_take_the_previous_result() {
        let mut merge = primitive(FfiSvgFilterPrimitiveKind::Merge, InterpolationColorSpace::SRGB);
        merge.merge_inputs = vec![name("first"), name("SourceAlpha"), name("missing")];

        let first_blur = Filter::gaussian_blur(1.0, 1.0, None);
        let second_blur = Filter::gaussian_blur(2.0, 2.0, Some(first_blur.clone()));
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
            Some(Filter::gaussian_blur(1.0, 1.0, None))
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
                input: Some(Box::new(Filter::gaussian_blur(
                    3.0,
                    3.0,
                    Some(Filter::ColorMatrix {
                        matrix: ALPHA_ONLY_MATRIX,
                        input: None,
                    }),
                ))),
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
