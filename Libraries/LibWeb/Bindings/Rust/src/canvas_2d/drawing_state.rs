/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#drawing-state

use std::rc::Rc;

use ak::Utf16String;
use libgfx_rust::{AffineTransform, CapStyle, Color, CompositingAndBlendingOperator, JoinStyle};
use libjs_rust::api::{Trace, Visitor};

use super::canvas::{ResolvedFont, StyleValue};
use crate::bindings::interface::Ref;
use crate::html::canvas_gradient::CanvasGradient;
use crate::html::canvas_pattern::CanvasPattern;
use crate::html::canvas_rendering_context2d::{
    CanvasDirection, CanvasLineCap, CanvasLineJoin, CanvasTextAlign, CanvasTextBaseline, ImageSmoothingQuality,
};
use crate::html::canvas_text_drawing_styles::{
    CanvasFontKerning, CanvasFontStretch, CanvasFontVariantCaps, CanvasTextRendering,
};

/// A CSS color that a context was given: the color in sRGB, which the attribute that was set to it serializes, and the
/// color converted to the color space of the context, which the context paints with.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct CanvasColor {
    pub(crate) srgb: Color,
    pub(crate) in_color_space: Color,
}

impl CanvasColor {
    /// A color that is the same in every color space of a context.
    pub(crate) const fn invariant(color: Color) -> Self {
        Self {
            srgb: color,
            in_color_space: color,
        }
    }

    pub(crate) fn alpha(self) -> u8 {
        self.in_color_space.alpha()
    }
}

/// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-fillstyle
#[derive(Clone, Copy)]
pub(crate) enum FillOrStrokeStyle {
    Color(CanvasColor),
    Gradient(Ref<CanvasGradient>),
    Pattern(Ref<CanvasPattern>),
}

impl FillOrStrokeStyle {
    /// Whether a draw with the style can change a pixel.
    pub(crate) fn is_visible(self) -> bool {
        match self {
            Self::Color(color) => color.alpha() > 0,
            Self::Gradient(gradient) => gradient.is_visible(),
            Self::Pattern(_) => true,
        }
    }

    /// Whether the style is a pattern that is marked as not origin-clean.
    pub(crate) fn is_not_origin_clean(self) -> bool {
        matches!(self, Self::Pattern(pattern) if pattern.is_not_origin_clean())
    }
}

// SAFETY: Visits the gradient or the pattern, the cells that a style holds.
unsafe impl Trace for FillOrStrokeStyle {
    fn trace(&self, visitor: &mut Visitor) {
        match self {
            Self::Color(_) => {}
            Self::Gradient(gradient) => gradient.trace(visitor),
            Self::Pattern(pattern) => pattern.trace(visitor),
        }
    }
}

/// The letter spacing or the word spacing of a context, a CSS <length> as the context was given it.
#[derive(Clone)]
pub(crate) struct Spacing {
    pub(crate) length: StyleValue,
    pub(crate) serialized: Utf16String,
}

/// https://html.spec.whatwg.org/multipage/canvas.html#drawing-state
#[derive(Clone)]
pub(crate) struct DrawingState {
    pub(crate) transform: AffineTransform,
    pub(crate) fill_style: FillOrStrokeStyle,
    pub(crate) stroke_style: FillOrStrokeStyle,
    pub(crate) shadow_offset_x: f32,
    pub(crate) shadow_offset_y: f32,
    pub(crate) shadow_blur: f32,
    pub(crate) shadow_color: CanvasColor,
    /// The serialized filter graph of the current filter, which the states share.
    pub(crate) filter: Option<Rc<[u8]>>,
    /// The value of the filter attribute, unless it is "none".
    pub(crate) filter_string: Option<Utf16String>,
    pub(crate) line_width: f32,
    pub(crate) line_cap: CanvasLineCap,
    pub(crate) line_join: CanvasLineJoin,
    pub(crate) miter_limit: f32,
    pub(crate) dash_list: Vec<f64>,
    pub(crate) line_dash_offset: f32,
    pub(crate) image_smoothing_enabled: bool,
    pub(crate) image_smoothing_quality: ImageSmoothingQuality,
    pub(crate) global_alpha: f32,
    pub(crate) current_compositing_and_blending_operator: CompositingAndBlendingOperator,
    /// The font, which is resolved the first time the context needs one.
    pub(crate) font: Option<ResolvedFont>,
    pub(crate) text_align: CanvasTextAlign,
    pub(crate) text_baseline: CanvasTextBaseline,
    pub(crate) direction: CanvasDirection,
    /// The language, https://html.spec.whatwg.org/multipage/canvas.html#concept-canvastextdrawingstyles-language
    pub(crate) language: Utf16String,
    /// The letter spacing, unless it is 0px.
    pub(crate) letter_spacing: Option<Spacing>,
    /// The word spacing, unless it is 0px.
    pub(crate) word_spacing: Option<Spacing>,
    pub(crate) font_kerning: CanvasFontKerning,
    pub(crate) font_stretch: CanvasFontStretch,
    pub(crate) font_variant_caps: CanvasFontVariantCaps,
    pub(crate) text_rendering: CanvasTextRendering,
}

impl Default for DrawingState {
    fn default() -> Self {
        Self {
            transform: AffineTransform::identity(),
            fill_style: FillOrStrokeStyle::Color(CanvasColor::invariant(Color::from_rgb(0, 0, 0))),
            stroke_style: FillOrStrokeStyle::Color(CanvasColor::invariant(Color::from_rgb(0, 0, 0))),
            shadow_offset_x: 0.0,
            shadow_offset_y: 0.0,
            shadow_blur: 0.0,
            shadow_color: CanvasColor::invariant(Color::TRANSPARENT),
            filter: None,
            filter_string: None,
            line_width: 1.0,
            line_cap: CanvasLineCap::Butt,
            line_join: CanvasLineJoin::Miter,
            miter_limit: 10.0,
            dash_list: Vec::new(),
            line_dash_offset: 0.0,
            image_smoothing_enabled: true,
            image_smoothing_quality: ImageSmoothingQuality::Low,
            global_alpha: 1.0,
            current_compositing_and_blending_operator: CompositingAndBlendingOperator::SourceOver,
            font: None,
            text_align: CanvasTextAlign::Start,
            text_baseline: CanvasTextBaseline::Alphabetic,
            direction: CanvasDirection::Inherit,
            language: Utf16String::from_ascii(b"inherit"),
            letter_spacing: None,
            word_spacing: None,
            font_kerning: CanvasFontKerning::Auto,
            font_stretch: CanvasFontStretch::Normal,
            font_variant_caps: CanvasFontVariantCaps::Normal,
            text_rendering: CanvasTextRendering::Auto,
        }
    }
}

// SAFETY: Visits the fill and the stroke style, the cells that a drawing state holds.
unsafe impl Trace for DrawingState {
    fn trace(&self, visitor: &mut Visitor) {
        self.fill_style.trace(visitor);
        self.stroke_style.trace(visitor);
    }
}

impl DrawingState {
    pub(crate) fn cap_style(&self) -> CapStyle {
        match self.line_cap {
            CanvasLineCap::Butt => CapStyle::Butt,
            CanvasLineCap::Round => CapStyle::Round,
            CanvasLineCap::Square => CapStyle::Square,
        }
    }

    pub(crate) fn join_style(&self) -> JoinStyle {
        match self.line_join {
            CanvasLineJoin::Round => JoinStyle::Round,
            CanvasLineJoin::Bevel => JoinStyle::Bevel,
            CanvasLineJoin::Miter => JoinStyle::Miter,
        }
    }

    /// The dash list in floats, as a stroke records it.
    #[allow(
        clippy::cast_possible_truncation,
        reason = "a stroke records its dashes in floats, as the C++ did"
    )]
    pub(crate) fn dash_array(&self) -> Vec<f32> {
        self.dash_list.iter().map(|&dash| dash as f32).collect()
    }
}

/// The names of the compositing and blending operators that globalCompositeOperation takes, in the order that the
/// getter of the C++ looked the operator up in.
const COMPOSITE_OPERATIONS: [(&str, CompositingAndBlendingOperator); 30] = [
    ("normal", CompositingAndBlendingOperator::Normal),
    ("multiply", CompositingAndBlendingOperator::Multiply),
    ("screen", CompositingAndBlendingOperator::Screen),
    ("overlay", CompositingAndBlendingOperator::Overlay),
    ("darken", CompositingAndBlendingOperator::Darken),
    ("lighten", CompositingAndBlendingOperator::Lighten),
    ("color-dodge", CompositingAndBlendingOperator::ColorDodge),
    ("color-burn", CompositingAndBlendingOperator::ColorBurn),
    ("hard-light", CompositingAndBlendingOperator::HardLight),
    ("soft-light", CompositingAndBlendingOperator::SoftLight),
    ("difference", CompositingAndBlendingOperator::Difference),
    ("exclusion", CompositingAndBlendingOperator::Exclusion),
    ("hue", CompositingAndBlendingOperator::Hue),
    ("saturation", CompositingAndBlendingOperator::Saturation),
    ("color", CompositingAndBlendingOperator::Color),
    ("luminosity", CompositingAndBlendingOperator::Luminosity),
    ("clear", CompositingAndBlendingOperator::Clear),
    ("copy", CompositingAndBlendingOperator::Copy),
    ("source-over", CompositingAndBlendingOperator::SourceOver),
    ("destination-over", CompositingAndBlendingOperator::DestinationOver),
    ("source-in", CompositingAndBlendingOperator::SourceIn),
    ("destination-in", CompositingAndBlendingOperator::DestinationIn),
    ("source-out", CompositingAndBlendingOperator::SourceOut),
    ("destination-out", CompositingAndBlendingOperator::DestinationOut),
    ("source-atop", CompositingAndBlendingOperator::SourceATop),
    ("destination-atop", CompositingAndBlendingOperator::DestinationATop),
    ("xor", CompositingAndBlendingOperator::Xor),
    ("lighter", CompositingAndBlendingOperator::Lighter),
    ("plus-darker", CompositingAndBlendingOperator::PlusDarker),
    ("plus-lighter", CompositingAndBlendingOperator::PlusLighter),
];

/// The name of a compositing and blending operator, which every operator that a context can have has.
pub(crate) fn composite_operation_name(operator: CompositingAndBlendingOperator) -> &'static str {
    COMPOSITE_OPERATIONS
        .iter()
        .find(|(_, candidate)| *candidate == operator)
        .map(|(name, _)| *name)
        .expect("a context has an operator that globalCompositeOperation names")
}

/// The compositing and blending operator that a string names exactly, if it names one.
pub(crate) fn composite_operation_named(name: &Utf16String) -> Option<CompositingAndBlendingOperator> {
    let units = name.to_utf16();
    COMPOSITE_OPERATIONS
        .iter()
        .find(|(candidate, _)| candidate.encode_utf16().eq(units.iter().copied()))
        .map(|(_, operator)| *operator)
}

/// A color as the attributes of a context serialize it, with HTML-compatible serialization requested.
/// https://html.spec.whatwg.org/multipage/infrastructure.html#serialisation-of-a-color
/// https://drafts.csswg.org/css-color-4/#HTML-compatible-serialization-of-srgb
pub(crate) fn serialize_color(color: Color) -> Utf16String {
    // NB: Our colors are always sRGB with 8-bit components, and HTML-compatible serialization is always requested
    //     here, so only the alpha decides which serialization is used.
    // If the following conditions are all true:
    // 1. The color space is sRGB
    // 2. The alpha is 1
    // 3. The RGB component values are internally represented as integers between 0 and 255 inclusive (i.e. 8-bit
    //    unsigned integer)
    // 4. HTML-compatible serialization is requested
    // Then corresponding sRGB values are serialized in 6-digit hex color notation as follows:
    // A seven-character string consisting of the character "#", followed immediately by the two-digit hexadecimal
    // representations of the red component, the green component, and the blue component, in that order, using ASCII
    // lower hex digits. No spaces are permitted.
    if color.alpha() == 255 {
        let serialized = format!("#{:02x}{:02x}{:02x}", color.red(), color.green(), color.blue());
        return Utf16String::from_ascii(serialized.as_bytes());
    }
    // Otherwise, for sRGB the CSS serialization of sRGB values is used and for other color spaces, the relevant
    // serialization of the <color> value.
    let serialized = if color.alpha() == 0 {
        format!("rgba({}, {}, {}, 0)", color.red(), color.green(), color.blue())
    } else {
        format!(
            "rgba({}, {}, {}, 0.{})",
            color.red(),
            color.green(),
            color.blue(),
            alpha_digits(color.alpha())
        )
    };
    Utf16String::from_ascii(serialized.as_bytes())
}

/// The shortest fractional digits of an 8-bit alpha that read back as it, as format_to_8bit_compatible() of
/// LibGfx/Color.cpp writes them: 127 is "498", and 128 is "5".
fn alpha_digits(value: u8) -> String {
    let value = u32::from(value);
    let three_digits = (value * 1000 + 127) / 255;
    let rounded_to_two_digits = (three_digits + 5) / 10 * 10;
    if (rounded_to_two_digits * 255 / 100 + 5) / 10 != value {
        return format!("{three_digits:03}");
    }
    let rounded_to_one_digit = (three_digits + 50) / 100 * 100;
    if (rounded_to_one_digit * 255 / 100 + 5) / 10 != value {
        return format!("{:02}", rounded_to_two_digits / 10);
    }
    format!("{}", rounded_to_one_digit / 100)
}
