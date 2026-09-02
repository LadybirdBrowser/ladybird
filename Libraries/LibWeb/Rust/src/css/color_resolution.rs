/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS color resolution primitives: the 8-bit sRGB color type mirroring Gfx::Color, the
//! color-function descriptor table, the system-color keyword values, and the to_color()
//! resolution layer over color-bearing style values.

// The FFI consumer of to_color() lands with the FFI flip.
#![allow(dead_code)]
// Color values use the same thread-confined shared graph as the rest of the style core.
#![allow(clippy::arc_with_non_send_sync)]

use std::sync::Arc;

use crate::css::calc::{
    ANGLE_UNIT_CANONICAL_RATIOS, ResolveAs, resolve_as_from_fields, resolve_calculated_angle_with_channels,
    resolve_calculated_number_with_channels, resolve_calculated_percentage_with_channels,
};
use crate::css::color_conversion::{self, Components};
use crate::css::color_interpolation::{FfiResolvedColor, rust_interpolate_color};
use crate::css::css_enums::channel_keyword;
use crate::css::css_enums::keyword;
use crate::css::css_enums::keyword_to_channel_keyword;
use crate::css::serialize::TextSink;
use crate::css::style_compute::FfiLengthResolutionContext;
use crate::css::style_value::{
    ColorBase, RetainedStyleValueData, RetainedUtf16FlyString, StyleValueData, value_depends_on_current_color,
};

/// Mirrors Gfx::Color: 8-bit unpremultiplied sRGB with alpha 255 meaning fully opaque.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Rgba {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

/// Mirrors the quantization in Gfx::srgb_to_color() and color_from_linear_srgb():
/// lroundf (round half away from zero, which f32::round matches) then clamp to 0..=255.
fn quantize(component: f32) -> u8 {
    (component * 255.0).round().clamp(0.0, 255.0) as u8
}

/// The wide-gamut Gfx::Color::from_*() constructors funnel through color_from_linear_srgb().
/// This reuses color_conversion::convert() instead of duplicating its matrices. Routing
/// differences against the C++ code, all below u8 quantization error:
/// - C++ from_xyz50/from_lab/from_pro_photo_rgb multiply a direct XYZ-D50-to-linear-sRGB
///   matrix; convert() hops through XYZ-D65 instead.
/// - C++ from_oklab uses the bottosson.github.io OKLab-to-linear-sRGB matrix; convert()
///   composes the CSS Color 4 OKLab-to-XYZ-D65 and XYZ-D65-to-linear-sRGB matrices.
/// - C++ from_linear_srgb gamma-encodes directly; convert() round-trips through XYZ-D65.
/// - Two literals disagree with LibGfx by one f32 ulp: the display-p3 X-row green
///   coefficient (0.26566769f vs 0.2656677) and xyz50_to_xyz65's first coefficient
///   (0.9554734528f vs 0.9554735).
fn from_converted(color_type: u8, components: Components) -> Rgba {
    let srgb =
        color_conversion::convert(color_type, color_conversion::SRGB, components).expect("color type converts to sRGB");
    Rgba {
        r: quantize(srgb[0]),
        g: quantize(srgb[1]),
        b: quantize(srgb[2]),
        a: quantize(srgb[3]),
    }
}

impl Rgba {
    pub(crate) const BLACK: Self = Self {
        r: 0,
        g: 0,
        b: 0,
        a: 255,
    };
    pub(crate) const WHITE: Self = Self {
        r: 255,
        g: 255,
        b: 255,
        a: 255,
    };

    /// Port of Gfx::Color::from_hsla(): saturation and lightness clamp to 0..=1 before the
    /// conversion, and hsl_to_srgb() clamps alpha itself. The C++ code narrows to f32 first.
    pub(crate) fn from_hsla(h_degrees: f64, s: f64, l: f64, a: f64) -> Self {
        let hsl = [
            h_degrees as f32,
            (s as f32).clamp(0.0, 1.0),
            (l as f32).clamp(0.0, 1.0),
            a as f32,
        ];
        let srgb = color_conversion::legacy_color_to_srgb(color_conversion::HSL, hsl).expect("HSL converts to sRGB");
        Self {
            r: quantize(srgb[0]),
            g: quantize(srgb[1]),
            b: quantize(srgb[2]),
            a: quantize(srgb[3]),
        }
    }

    /// Port of Gfx::Color::from_hsv(). The input ranges are the ones the C++ code VERIFYs.
    /// Gfx::hsv_to_srgb() computes in f64 but stores through f32 components, and the result
    /// rounds without clamping (the input ranges keep every product inside 0..=255).
    pub(crate) fn from_hsv(hue: f64, saturation: f64, value: f64) -> Self {
        debug_assert!((0.0..360.0).contains(&hue));
        debug_assert!((0.0..=1.0).contains(&saturation));
        debug_assert!((0.0..=1.0).contains(&value));

        let hue = f64::from(hue as f32);
        let saturation = f64::from(saturation as f32);
        let value = f64::from(value as f32);

        let high = ((hue / 60.0) as i32) % 6;
        let f = hue / 60.0 - f64::from(high);
        let c1 = value * (1.0 - saturation);
        let c2 = value * (1.0 - saturation * f);
        let c3 = value * (1.0 - saturation * (1.0 - f));

        let (r, g, b) = match high {
            0 => (value, c3, c1),
            1 => (c2, value, c1),
            2 => (c1, value, c3),
            3 => (c1, c2, value),
            4 => (c3, c1, value),
            5 => (value, c1, c2),
            _ => (0.0, 0.0, 0.0),
        };

        Self {
            r: (r as f32 * 255.0).round() as u8,
            g: (g as f32 * 255.0).round() as u8,
            b: (b as f32 * 255.0).round() as u8,
            a: 255,
        }
    }

    /// Port of Gfx::srgb_to_color().
    pub(crate) fn from_srgb(r: f32, g: f32, b: f32, alpha: f32) -> Self {
        Self {
            r: quantize(r),
            g: quantize(g),
            b: quantize(b),
            a: quantize(alpha),
        }
    }

    /// Port of Gfx::Color::from_linear_srgb().
    pub(crate) fn from_linear_srgb(r: f32, g: f32, b: f32, alpha: f32) -> Self {
        from_converted(color_conversion::SRGB_LINEAR, [r, g, b, alpha])
    }

    /// Port of Gfx::Color::from_a98rgb().
    pub(crate) fn from_a98rgb(r: f32, g: f32, b: f32, alpha: f32) -> Self {
        from_converted(color_conversion::A98_RGB, [r, g, b, alpha])
    }

    /// Port of Gfx::Color::from_display_p3().
    pub(crate) fn from_display_p3(r: f32, g: f32, b: f32, alpha: f32) -> Self {
        from_converted(color_conversion::DISPLAY_P3, [r, g, b, alpha])
    }

    /// Port of Gfx::Color::from_linear_display_p3().
    pub(crate) fn from_display_p3_linear(r: f32, g: f32, b: f32, alpha: f32) -> Self {
        from_converted(color_conversion::DISPLAY_P3_LINEAR, [r, g, b, alpha])
    }

    /// Port of Gfx::Color::from_pro_photo_rgb().
    pub(crate) fn from_pro_photo(r: f32, g: f32, b: f32, alpha: f32) -> Self {
        from_converted(color_conversion::PROPHOTO_RGB, [r, g, b, alpha])
    }

    /// Port of Gfx::Color::from_rec2020().
    pub(crate) fn from_rec2020(r: f32, g: f32, b: f32, alpha: f32) -> Self {
        from_converted(color_conversion::REC2020, [r, g, b, alpha])
    }

    /// Port of Gfx::Color::from_xyz50().
    pub(crate) fn from_xyz50(x: f32, y: f32, z: f32, alpha: f32) -> Self {
        from_converted(color_conversion::XYZ_D50, [x, y, z, alpha])
    }

    /// Port of Gfx::Color::from_xyz65().
    pub(crate) fn from_xyz65(x: f32, y: f32, z: f32, alpha: f32) -> Self {
        from_converted(color_conversion::XYZ_D65, [x, y, z, alpha])
    }

    /// Port of Gfx::Color::from_lab().
    pub(crate) fn from_lab(lightness: f32, a: f32, b: f32, alpha: f32) -> Self {
        from_converted(color_conversion::LAB, [lightness, a, b, alpha])
    }

    /// Port of Gfx::Color::from_oklab().
    pub(crate) fn from_oklab(lightness: f32, a: f32, b: f32, alpha: f32) -> Self {
        from_converted(color_conversion::OKLAB, [lightness, a, b, alpha])
    }

    pub(crate) fn with_alpha(self, a: u8) -> Self {
        Self { a, ..self }
    }

    /// Port of Gfx::Color::with_opacity(): scales the alpha channel in f32 like the C++ float
    /// parameter, rounding half away from zero. The C++ code VERIFYs the range.
    pub(crate) fn with_opacity(self, opacity: f64) -> Self {
        debug_assert!((0.0..=1.0).contains(&opacity));
        let opacity = opacity as f32;
        self.with_alpha((f32::from(self.a) * opacity).round() as u8)
    }

    /// Port of Gfx::Color::darkened(): the float products truncate through C++'s
    /// float-to-u8 conversion, so no rounding here.
    pub(crate) fn darkened(self, amount: f32) -> Self {
        Self {
            r: (f32::from(self.r) * amount) as u8,
            g: (f32::from(self.g) * amount) as u8,
            b: (f32::from(self.b) * amount) as u8,
            a: self.a,
        }
    }

    /// https://w3c.github.io/wcag/guidelines/22/#dfn-relative-luminance
    pub(crate) fn relative_luminance(&self) -> f64 {
        // Gfx::Color caches these per-component linearizations in a 256-entry table built
        // with the same f64 arithmetic; computing directly yields identical values.
        fn linearize(component: u8) -> f64 {
            let srgb = f64::from(component) / 255.0;
            if srgb <= 0.04045 {
                srgb / 12.92
            } else {
                ((srgb + 0.055) / 1.055).powf(2.4)
            }
        }
        0.2126 * linearize(self.r) + 0.7152 * linearize(self.g) + 0.0722 * linearize(self.b)
    }

    /// https://w3c.github.io/wcag/guidelines/22/#dfn-contrast-ratio
    pub(crate) fn contrast_ratio(&self, other: Rgba) -> f64 {
        let l1 = self.relative_luminance();
        let l2 = other.relative_luminance();
        let darkest = l1.min(l2);
        let brightest = l1.max(l2);
        (brightest + 0.05) / (darkest + 0.05)
    }

    pub(crate) fn suggested_foreground_color(&self) -> Rgba {
        if self.contrast_ratio(Self::BLACK) >= self.contrast_ratio(Self::WHITE) {
            Self::BLACK
        } else {
            Self::WHITE
        }
    }

    /// https://www.w3.org/TR/css-color-4/#serializing-sRGB-values
    pub(crate) fn serialize_a_srgb_value(&self, sink: &mut TextSink) {
        // The serialized form uses either the rgb() or rgba() form depending on whether the
        // alpha is exactly 1; an "alpha of 1" is 255 in 8-bit storage.
        if self.a == 0 {
            sink.push_ascii(&format!("rgba({}, {}, {}, 0)", self.r, self.g, self.b));
        } else if self.a == 255 {
            sink.push_ascii(&format!("rgb({}, {}, {})", self.r, self.g, self.b));
        } else {
            let (digits, length) = format_to_8bit_compatible(self.a);
            let fraction = std::str::from_utf8(&digits[..length]).expect("digits are ASCII");
            sink.push_ascii(&format!("rgba({}, {}, {}, 0.{fraction})", self.r, self.g, self.b));
        }
    }
}

pub(crate) fn resolved_srgb_style_value(color: Rgba) -> StyleValueData {
    StyleValueData::ColorFunction {
        color_base: ColorBase {
            has_color_type: true,
            color_type: color_conversion::RGB,
            color_syntax: 0,
        },
        channel_0: retained_value(StyleValueData::Number { value: color.r.into() }),
        channel_1: retained_value(StyleValueData::Number { value: color.g.into() }),
        channel_2: retained_value(StyleValueData::Number { value: color.b.into() }),
        alpha: retained_value(StyleValueData::Number {
            value: f64::from(color.a) / 255.0,
        }),
        has_name: false,
        name: RetainedUtf16FlyString::none(),
        origin_color: retained_null(),
    }
}

/// nth_digit(745, 1) is '5'; digit 3 is the hundreds place.
fn nth_digit(mut value: u32, mut digit: u8) -> u8 {
    debug_assert!(value < 1000);
    debug_assert!((1..=3).contains(&digit));
    while digit > 1 {
        value /= 10;
        digit -= 1;
    }
    b'0' + (value % 10) as u8
}

/// Port of format_to_8bit_compatible() in Gfx/Color.cpp: the shortest fractional digits that
/// round-trip at 8 bits. 127/255 = 0.498 +- 0.001 and 128/255 = 0.502 +- 0.001, but
/// round(0.5 * 255) == 128, so 127 formats as "498" while 128 formats as "5".
pub(crate) fn format_to_8bit_compatible(value: u8) -> ([u8; 3], usize) {
    let value = u32::from(value);

    let three_digits = (value * 1000 + 127) / 255;
    let rounded_to_two_digits = (three_digits + 5) / 10 * 10;

    if (rounded_to_two_digits * 255 / 100 + 5) / 10 != value {
        let digits = [
            nth_digit(three_digits, 3),
            nth_digit(three_digits, 2),
            nth_digit(three_digits, 1),
        ];
        return (digits, 3);
    }

    let rounded_to_one_digit = (three_digits + 50) / 100 * 100;
    if (rounded_to_one_digit * 255 / 100 + 5) / 10 != value {
        let digits = [
            nth_digit(rounded_to_two_digits, 3),
            nth_digit(rounded_to_two_digits, 2),
            0,
        ];
        return (digits, 2);
    }

    ([nth_digit(rounded_to_one_digit, 3), 0, 0], 1)
}

/// Mirrors Web::CSS::ChannelDescriptor; is_hue stands in for ChannelKind::Hue.
pub(crate) struct ChannelDescriptor {
    pub is_hue: bool,
    pub percent_reference: f32,
    pub serialize_clamp_min: Option<f64>,
    pub serialize_clamp_max: Option<f64>,
}

/// Mirrors Web::CSS::SerializationBehavior.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SerializationBehavior {
    /// rgb(), hsl(): commas or slash chosen via ColorSyntax; serializes via the sRGB shortcut.
    SrgbLegacy,
    /// hwb(): modern named function in the sRGB family; serializes via the sRGB shortcut.
    SrgbModern,
    /// lab(), oklab(), lch(), oklch(): modern named function, no sRGB shortcut.
    ModernNamed,
    /// color(&lt;space&gt; c1 c2 c3 / a).
    ColorFunction,
}

/// Mirrors Web::CSS::ColorFunctionDescriptor.
pub(crate) struct ColorFunctionDescriptor {
    pub name: &'static str,
    pub behavior: SerializationBehavior,
    pub channels: [ChannelDescriptor; 3],
    pub absolutizes_to_rgb: bool,
}

const fn number_channel(
    percent_reference: f32,
    serialize_clamp_min: Option<f64>,
    serialize_clamp_max: Option<f64>,
) -> ChannelDescriptor {
    ChannelDescriptor {
        is_hue: false,
        percent_reference,
        serialize_clamp_min,
        serialize_clamp_max,
    }
}

const fn hue_channel() -> ChannelDescriptor {
    ChannelDescriptor {
        is_hue: true,
        percent_reference: 0.0,
        serialize_clamp_min: None,
        serialize_clamp_max: None,
    }
}

const fn color_function(name: &'static str) -> ColorFunctionDescriptor {
    ColorFunctionDescriptor {
        name,
        behavior: SerializationBehavior::ColorFunction,
        channels: [
            number_channel(1.0, None, None),
            number_channel(1.0, None, None),
            number_channel(1.0, None, None),
        ],
        absolutizes_to_rgb: false,
    }
}

/// Transcribed from ColorFunctionDescriptor.cpp, indexed by the frozen color-type codes
/// (the color_conversion constants, asserted against C++ in ColorMixStyleValue.cpp).
static COLOR_FUNCTION_DESCRIPTORS: [ColorFunctionDescriptor; 16] = [
    // RGB
    ColorFunctionDescriptor {
        name: "rgb",
        behavior: SerializationBehavior::SrgbLegacy,
        channels: [
            number_channel(255.0, Some(0.0), Some(255.0)),
            number_channel(255.0, Some(0.0), Some(255.0)),
            number_channel(255.0, Some(0.0), Some(255.0)),
        ],
        absolutizes_to_rgb: false,
    },
    // A98RGB
    color_function("a98-rgb"),
    // DisplayP3
    color_function("display-p3"),
    // DisplayP3Linear
    color_function("display-p3-linear"),
    // HSL
    ColorFunctionDescriptor {
        name: "hsl",
        behavior: SerializationBehavior::SrgbLegacy,
        channels: [
            hue_channel(),
            number_channel(100.0, Some(0.0), None),
            number_channel(100.0, Some(0.0), None),
        ],
        absolutizes_to_rgb: true,
    },
    // HWB
    ColorFunctionDescriptor {
        name: "hwb",
        behavior: SerializationBehavior::SrgbModern,
        channels: [
            hue_channel(),
            number_channel(100.0, Some(0.0), None),
            number_channel(100.0, Some(0.0), None),
        ],
        absolutizes_to_rgb: true,
    },
    // Lab
    ColorFunctionDescriptor {
        name: "lab",
        behavior: SerializationBehavior::ModernNamed,
        channels: [
            number_channel(100.0, Some(0.0), Some(100.0)),
            number_channel(125.0, None, None),
            number_channel(125.0, None, None),
        ],
        absolutizes_to_rgb: false,
    },
    // LCH
    ColorFunctionDescriptor {
        name: "lch",
        behavior: SerializationBehavior::ModernNamed,
        channels: [
            number_channel(100.0, Some(0.0), Some(100.0)),
            number_channel(150.0, Some(0.0), None),
            hue_channel(),
        ],
        absolutizes_to_rgb: false,
    },
    // OKLab
    ColorFunctionDescriptor {
        name: "oklab",
        behavior: SerializationBehavior::ModernNamed,
        channels: [
            number_channel(1.0, Some(0.0), Some(1.0)),
            number_channel(0.4, None, None),
            number_channel(0.4, None, None),
        ],
        absolutizes_to_rgb: false,
    },
    // OKLCH
    ColorFunctionDescriptor {
        name: "oklch",
        behavior: SerializationBehavior::ModernNamed,
        channels: [
            number_channel(1.0, Some(0.0), Some(1.0)),
            number_channel(0.4, Some(0.0), None),
            hue_channel(),
        ],
        absolutizes_to_rgb: false,
    },
    // sRGB
    color_function("srgb"),
    // sRGBLinear
    color_function("srgb-linear"),
    // ProPhotoRGB
    color_function("prophoto-rgb"),
    // Rec2020
    color_function("rec2020"),
    // XYZD50
    color_function("xyz-d50"),
    // XYZD65
    color_function("xyz-d65"),
];

pub(crate) fn descriptor_for_color_type(color_type: u8) -> &'static ColorFunctionDescriptor {
    &COLOR_FUNCTION_DESCRIPTORS[usize::from(color_type)]
}

/// Relative color syntax channel values, indexed by the generated `channel_keyword` codes
/// (R = 0 through Z = 12).
pub(crate) type RelativeColorContext = [Option<f64>; 13];

const _: () = assert!(channel_keyword::Z as usize + 1 == 13);

// System colors, transcribed from SystemColor.cpp. SystemColor::inactive_highlight() and
// transform_selection_background_color() are not reachable from KeywordStyleValue::to_color()
// and are intentionally absent.

const fn rgb(r: u8, g: u8, b: u8) -> Rgba {
    Rgba { r, g, b, a: 255 }
}

pub(crate) fn accent_color(_dark: bool) -> Rgba {
    rgb(61, 174, 233)
}

fn accent_color_text(_dark: bool) -> Rgba {
    rgb(0, 0, 0)
}

fn active_text(dark: bool) -> Rgba {
    if dark { rgb(213, 97, 82) } else { rgb(255, 0, 0) }
}

fn button_border(dark: bool) -> Rgba {
    if dark { rgb(72, 72, 72) } else { rgb(128, 128, 128) }
}

fn button_face(dark: bool) -> Rgba {
    if dark { rgb(32, 29, 25) } else { rgb(212, 208, 200) }
}

fn button_text(dark: bool) -> Rgba {
    if dark { rgb(235, 235, 235) } else { rgb(0, 0, 0) }
}

fn canvas(dark: bool) -> Rgba {
    if dark { rgb(20, 20, 20) } else { rgb(255, 255, 255) }
}

fn canvas_text(dark: bool) -> Rgba {
    if dark { rgb(235, 235, 235) } else { rgb(0, 0, 0) }
}

fn field(dark: bool) -> Rgba {
    if dark { rgb(32, 29, 25) } else { rgb(255, 255, 255) }
}

fn field_text(dark: bool) -> Rgba {
    if dark { rgb(235, 235, 235) } else { rgb(0, 0, 0) }
}

fn gray_text(_dark: bool) -> Rgba {
    rgb(128, 128, 128)
}

fn highlight(_dark: bool) -> Rgba {
    rgb(61, 174, 233).with_alpha(128)
}

fn highlight_text(dark: bool) -> Rgba {
    if dark { rgb(20, 20, 20) } else { rgb(255, 255, 255) }
}

fn link_text(dark: bool) -> Rgba {
    if dark { rgb(100, 149, 237) } else { rgb(0, 0, 238) }
}

fn mark(_dark: bool) -> Rgba {
    rgb(255, 255, 0)
}

fn mark_text(_dark: bool) -> Rgba {
    rgb(0, 0, 0)
}

fn selected_item(_dark: bool) -> Rgba {
    rgb(61, 174, 233)
}

fn selected_item_text(dark: bool) -> Rgba {
    if dark { rgb(20, 20, 20) } else { rgb(255, 255, 255) }
}

fn visited_text(dark: bool) -> Rgba {
    if dark { rgb(156, 113, 212) } else { rgb(85, 26, 139) }
}

/// The system-color half of KeywordStyleValue::to_color(): the keyword fan-out is transcribed
/// from KeywordStyleValue.cpp, including the two ad-hoc LibWeb derivations. Non-system-color
/// keywords (currentcolor included) return None.
// https://www.w3.org/TR/css-color-4/#css-system-colors
// https://www.w3.org/TR/css-color-4/#deprecated-system-colors
pub(crate) fn system_color_for_keyword(keyword_code: u16, dark: bool) -> Option<Rgba> {
    match keyword_code {
        // AD-HOC: The spec says that 'accentcolor' and 'accentcolortext' should be resolved
        //         based on the value of the 'accent-color' property, but this isn't implemented
        //         by any other browsers and isn't fully specified
        //         (https://github.com/w3c/csswg-drafts/issues/10971).
        keyword::ACCENTCOLOR => Some(accent_color(dark)),
        keyword::ACCENTCOLORTEXT => Some(accent_color_text(dark)),
        keyword::ACTIVETEXT => Some(active_text(dark)),
        keyword::BUTTONBORDER
        | keyword::ACTIVEBORDER
        | keyword::INACTIVEBORDER
        | keyword::THREEDDARKSHADOW
        | keyword::THREEDHIGHLIGHT
        | keyword::THREEDLIGHTSHADOW
        | keyword::THREEDSHADOW
        | keyword::WINDOWFRAME => Some(button_border(dark)),
        keyword::BUTTONFACE | keyword::BUTTONHIGHLIGHT | keyword::BUTTONSHADOW | keyword::THREEDFACE => {
            Some(button_face(dark))
        }
        keyword::BUTTONTEXT => Some(button_text(dark)),
        keyword::CANVAS
        | keyword::APPWORKSPACE
        | keyword::BACKGROUND
        | keyword::INACTIVECAPTION
        | keyword::INFOBACKGROUND
        | keyword::MENU
        | keyword::SCROLLBAR
        | keyword::WINDOW => Some(canvas(dark)),
        keyword::CANVASTEXT
        | keyword::ACTIVECAPTION
        | keyword::CAPTIONTEXT
        | keyword::INFOTEXT
        | keyword::MENUTEXT
        | keyword::WINDOWTEXT => Some(canvas_text(dark)),
        keyword::FIELD => Some(field(dark)),
        keyword::FIELDTEXT => Some(field_text(dark)),
        keyword::GRAYTEXT | keyword::INACTIVECAPTIONTEXT => Some(gray_text(dark)),
        keyword::HIGHLIGHT => Some(highlight(dark)),
        keyword::HIGHLIGHTTEXT => Some(highlight_text(dark)),
        keyword::MARK => Some(mark(dark)),
        keyword::MARKTEXT => Some(mark_text(dark)),
        keyword::SELECTEDITEM => Some(selected_item(dark)),
        keyword::SELECTEDITEMTEXT => Some(selected_item_text(dark)),
        keyword::_LIBWEB_BUTTONFACEDISABLED => Some(button_face(dark).with_alpha(128)),
        keyword::_LIBWEB_BUTTONFACEHOVER => Some(button_face(dark).darkened(0.8)),
        keyword::LINKTEXT => Some(link_text(dark)),
        keyword::VISITEDTEXT => Some(visited_text(dark)),
        _ => None,
    }
}

// PreferredColorScheme.h: Auto = 0, Dark = 1, Light = 2.
pub(crate) const PREFERRED_COLOR_SCHEME_DARK: u8 = 1;

// Gfx::RectangularColorSpace::Oklab, asserted against C++ in ColorMixStyleValue.cpp.
pub(crate) const RECTANGULAR_COLOR_SPACE_OKLAB: u8 = 8;

/// Mirror of Web::CSS::ColorResolutionContext: the preferred color scheme, the used value of
/// currentcolor, the computed currentcolor style value, and the calculation-resolution inputs
/// (a length context plus relative-color channel values).
#[derive(Clone, Copy)]
pub(crate) struct ColorResolutionInput<'a> {
    /// The PreferredColorScheme code; Dark is 1.
    pub scheme: Option<u8>,
    pub current_color: Option<Rgba>,
    pub current_color_value: Option<&'a StyleValueData>,
    pub length: Option<&'a FfiLengthResolutionContext>,
    pub channels: Option<&'a RelativeColorContext>,
}

impl ColorResolutionInput<'_> {
    fn dark(&self) -> bool {
        self.scheme == Some(PREFERRED_COLOR_SCHEME_DARK)
    }
}

/// The equivalent of a default-constructed C++ CalculationResolutionContext /
/// ColorResolutionContext: no scheme, no currentcolor, no length context, no channels.
pub(crate) const EMPTY_INPUT: ColorResolutionInput<'static> = ColorResolutionInput {
    scheme: None,
    current_color: None,
    current_color_value: None,
    length: None,
    channels: None,
};

/// Angle's base type index in the numeric type order, as fixed by
/// resolve_calculated_angle_with_channels()'s matches_dimension(1, ..) check.
const BASE_TYPE_ANGLE: usize = 1;

/// The stored resolve-as target of a Calculated variant, or None for any other variant.
fn calculated_resolve_as(value: &StyleValueData) -> Option<ResolveAs> {
    let StyleValueData::Calculated {
        has_percentages_resolve_as,
        resolve_as_is_number,
        resolve_as_base,
        ..
    } = value
    else {
        return None;
    };
    resolve_as_from_fields(*has_percentages_resolve_as, *resolve_as_is_number, *resolve_as_base)
}

/// Port of JS::modulo() over doubles: fmod with a negative result shifted into the divisor's sign.
fn js_modulo(x: f64, y: f64) -> f64 {
    let result = x % y;
    if result < 0.0 { result + y } else { result }
}

fn is_none_keyword(value: &StyleValueData) -> bool {
    matches!(value, StyleValueData::Keyword { keyword: code } if *code == keyword::NONE)
}

/// Port of ColorStyleValue::resolve_hue().
pub(crate) fn resolve_hue(style_value: &StyleValueData, input: &ColorResolutionInput) -> Option<f64> {
    // <number> | <angle> | none
    let normalized = |mut number: f64| {
        // +inf should be clamped to 360
        if !number.is_finite() && number > 0.0 {
            number = 360.0;
        }

        // -inf and NaN should be clamped to 0
        if !number.is_finite() || number.is_nan() {
            number = 0.0;
        }

        js_modulo(number, 360.0)
    };

    match style_value {
        StyleValueData::Number { value } => Some(normalized(*value)),
        // Angle::to_degrees(): the canonical angle unit is degrees.
        StyleValueData::Angle { value, unit } => {
            Some(normalized(value * ANGLE_UNIT_CANONICAL_RATIOS[usize::from(*unit)]))
        }
        StyleValueData::Keyword { keyword: code } => {
            if let Some(channel) = keyword_to_channel_keyword(*code) {
                let channels = input.channels?;
                let resolved = channels[usize::from(channel)]?;
                return Some(normalized(resolved));
            }
            Some(0.0)
        }
        StyleValueData::Calculated { resolved_type, .. } => {
            let resolve_as = calculated_resolve_as(style_value);
            let numeric_type = resolved_type.to_calc();
            if numeric_type.matches_number(resolve_as) {
                return resolve_calculated_number_with_channels(style_value, input.channels, input.length)
                    .map(normalized);
            }
            if numeric_type.matches_dimension(BASE_TYPE_ANGLE, resolve_as) {
                return resolve_calculated_angle_with_channels(style_value, input.channels, input.length)
                    .map(normalized);
            }
            Some(0.0)
        }
        _ => Some(0.0),
    }
}

/// Port of ColorStyleValue::resolve_with_reference_value().
pub(crate) fn resolve_with_reference_value(
    style_value: &StyleValueData,
    one_hundred_percent_value: f64,
    input: &ColorResolutionInput,
) -> Option<f64> {
    // <percentage> | <number> | none
    let normalize_percentage = |percentage: f64| percentage / 100.0 * one_hundred_percent_value;

    match style_value {
        StyleValueData::Percentage { value } => Some(normalize_percentage(*value)),
        StyleValueData::Number { value } => Some(*value),
        StyleValueData::Keyword { keyword: code } => {
            if let Some(channel) = keyword_to_channel_keyword(*code) {
                let channels = input.channels?;
                return channels[usize::from(channel)];
            }
            Some(0.0)
        }
        StyleValueData::Calculated { resolved_type, .. } => {
            let resolve_as = calculated_resolve_as(style_value);
            let numeric_type = resolved_type.to_calc();
            if numeric_type.matches_number(resolve_as) {
                return resolve_calculated_number_with_channels(style_value, input.channels, input.length);
            }
            if numeric_type.matches_percentage() {
                return resolve_calculated_percentage_with_channels(style_value, input.channels, input.length)
                    .map(normalize_percentage);
            }
            Some(0.0)
        }
        _ => Some(0.0),
    }
}

/// Port of ColorStyleValue::resolve_alpha().
pub(crate) fn resolve_alpha(style_value: &StyleValueData, input: &ColorResolutionInput) -> Option<f64> {
    // <number> | <percentage> | none
    let normalized = |number: f64| {
        let number = if number.is_nan() { 0.0 } else { number };
        number.clamp(0.0, 1.0)
    };

    match style_value {
        StyleValueData::Number { value } => Some(normalized(*value)),
        StyleValueData::Percentage { value } => Some(normalized(*value / 100.0)),
        StyleValueData::Keyword { keyword: code } => {
            if let Some(channel) = keyword_to_channel_keyword(*code) {
                let channels = input.channels?;
                let resolved = channels[usize::from(channel)]?;
                return Some(normalized(resolved));
            }
            if *code == keyword::NONE {
                return Some(0.0);
            }
            Some(1.0)
        }
        StyleValueData::Calculated { resolved_type, .. } => {
            let resolve_as = calculated_resolve_as(style_value);
            let numeric_type = resolved_type.to_calc();
            if numeric_type.matches_number(resolve_as) {
                return resolve_calculated_number_with_channels(style_value, input.channels, input.length)
                    .map(normalized);
            }
            if numeric_type.matches_percentage() {
                return resolve_calculated_percentage_with_channels(style_value, input.channels, input.length)
                    .map(|percentage| normalized(percentage / 100.0));
            }
            Some(1.0)
        }
        _ => Some(1.0),
    }
}

// Shared-graph retained-value constructors, mirroring make_result() in color_interpolation.rs.

fn retained_value(value: StyleValueData) -> RetainedStyleValueData {
    let pointer = Arc::into_raw(Arc::new(value));
    // SAFETY: Arc::into_raw transfers exactly one strong reference.
    unsafe { RetainedStyleValueData::from_retained_pointer(pointer) }
}

fn retained_null() -> RetainedStyleValueData {
    // SAFETY: null encodes an absent optional value.
    unsafe { RetainedStyleValueData::from_retained_optional_pointer(std::ptr::null()) }
}

/// Port of clamp_to_byte() in ColorFunctionStyleValue.cpp: NaN becomes 0, then llround
/// (round half away from zero, which f64::round matches) after clamping.
fn clamp_to_byte(value: f64) -> u8 {
    let value = if value.is_nan() { 0.0 } else { value };
    value.clamp(0.0, 255.0).round() as u8
}

/// Port of fraction_to_byte() in ColorFunctionStyleValue.cpp.
fn fraction_to_byte(fraction_0_1: f64) -> u8 {
    // Match CSS Color 4 "resolve to sRGB" rounding: round half away from zero,
    // not the default round-half-to-even that cvtsd2si uses.
    (fraction_0_1 * 255.0).clamp(0.0, 255.0).round() as u8
}

/// Port of ResolvedChannels in ColorFunctionStyleValue.cpp.
struct ResolvedChannels {
    c1: f64,
    c2: f64,
    c3: f64,
    alpha: f64,
}

/// Port of resolve_channels_for() in ColorFunctionStyleValue.cpp.
fn resolve_channels_for(
    descriptor: &ColorFunctionDescriptor,
    channels: [&StyleValueData; 3],
    alpha_style_value: Option<&StyleValueData>,
    input: &ColorResolutionInput,
) -> Option<ResolvedChannels> {
    let resolve_channel = |index: usize| {
        let channel_descriptor = &descriptor.channels[index];
        if channel_descriptor.is_hue {
            resolve_hue(channels[index], input)
        } else {
            resolve_with_reference_value(channels[index], f64::from(channel_descriptor.percent_reference), input)
        }
    };
    let c1 = resolve_channel(0);
    let c2 = resolve_channel(1);
    let c3 = resolve_channel(2);
    let alpha = match alpha_style_value {
        Some(alpha) => resolve_alpha(alpha, input),
        None => Some(1.0),
    };

    Some(ResolvedChannels {
        c1: c1?,
        c2: c2?,
        c3: c3?,
        alpha: alpha?,
    })
}

/// Port of the per-color-type construction switch in ColorFunctionStyleValue::to_color().
fn construct_color(color_type: u8, c1: f64, c2: f64, c3: f64, alpha: f64) -> Option<Rgba> {
    use color_conversion as ct;
    Some(match color_type {
        ct::RGB => Rgba {
            r: clamp_to_byte(c1),
            g: clamp_to_byte(c2),
            b: clamp_to_byte(c3),
            a: fraction_to_byte(alpha),
        },
        ct::HSL => Rgba::from_hsla(c1, c2 / 100.0, c3 / 100.0, alpha),
        ct::HWB => {
            let whiteness = (c2.clamp(0.0, 100.0) / 100.0) as f32;
            let blackness = (c3.clamp(0.0, 100.0) / 100.0) as f32;
            if whiteness + blackness >= 1.0 {
                let gray = fraction_to_byte(f64::from(whiteness / (whiteness + blackness)));
                return Some(Rgba {
                    r: gray,
                    g: gray,
                    b: gray,
                    a: fraction_to_byte(alpha),
                });
            }
            let value = 1.0 - blackness;
            let saturation = 1.0 - (whiteness / value);
            Rgba::from_hsv(c1, f64::from(saturation), f64::from(value)).with_opacity(alpha)
        }
        ct::LAB => Rgba::from_lab(c1.clamp(0.0, 100.0) as f32, c2 as f32, c3 as f32, alpha as f32),
        ct::OKLAB => Rgba::from_oklab(c1.clamp(0.0, 1.0) as f32, c2 as f32, c3 as f32, alpha as f32),
        ct::LCH => {
            let l = c1.clamp(0.0, 100.0);
            let hue_radians = c3.to_radians();
            Rgba::from_lab(
                l as f32,
                (c2 * hue_radians.cos()) as f32,
                (c2 * hue_radians.sin()) as f32,
                alpha as f32,
            )
        }
        ct::OKLCH => {
            let l = c1.clamp(0.0, 1.0);
            // AK::max() keeps NaN chroma as NaN, unlike f64::max.
            let chroma = if c2 < 0.0 { 0.0 } else { c2 };
            let hue_radians = c3.to_radians();
            Rgba::from_oklab(
                l as f32,
                (chroma * hue_radians.cos()) as f32,
                (chroma * hue_radians.sin()) as f32,
                alpha as f32,
            )
        }
        ct::A98_RGB => Rgba::from_a98rgb(c1 as f32, c2 as f32, c3 as f32, alpha as f32),
        ct::DISPLAY_P3 => Rgba::from_display_p3(c1 as f32, c2 as f32, c3 as f32, alpha as f32),
        ct::DISPLAY_P3_LINEAR => Rgba::from_display_p3_linear(c1 as f32, c2 as f32, c3 as f32, alpha as f32),
        ct::SRGB => {
            // AK::round_to<u8> lowers to cvtsd2si: round half to even, unlike the two byte
            // helpers above.
            let to_u8 = |value: f64| (255.0 * value).clamp(0.0, 255.0).round_ties_even() as u8;
            Rgba {
                r: to_u8(c1),
                g: to_u8(c2),
                b: to_u8(c3),
                a: to_u8(alpha),
            }
        }
        ct::SRGB_LINEAR => Rgba::from_linear_srgb(c1 as f32, c2 as f32, c3 as f32, alpha as f32),
        ct::PROPHOTO_RGB => Rgba::from_pro_photo(c1 as f32, c2 as f32, c3 as f32, alpha as f32),
        ct::REC2020 => Rgba::from_rec2020(c1 as f32, c2 as f32, c3 as f32, alpha as f32),
        ct::XYZ_D50 => Rgba::from_xyz50(c1 as f32, c2 as f32, c3 as f32, alpha as f32),
        ct::XYZ_D65 => Rgba::from_xyz65(c1 as f32, c2 as f32, c3 as f32, alpha as f32),
        // The C++ switch is exhaustive over ColorType and VERIFYs past its end.
        _ => return None,
    })
}

/// Port of ColorFunctionStyleValue::to_color().
fn color_function_to_color(value: &StyleValueData, input: &ColorResolutionInput) -> Option<Rgba> {
    let StyleValueData::ColorFunction {
        color_base,
        channel_0,
        channel_1,
        channel_2,
        alpha,
        origin_color,
        ..
    } = value
    else {
        return None;
    };
    // The C++ code dereferences color_type() unconditionally.
    if !color_base.has_color_type {
        return None;
    }
    let color_type = color_base.color_type;

    let relative_channels;
    let synthesized_alpha;
    let mut effective_input = *input;
    let mut alpha_value = alpha.optional_data();
    if let Some(origin) = origin_color.optional_data() {
        relative_channels = extract_channels_in_color_space(origin, color_type, input)?;
        effective_input.channels = Some(&relative_channels);
        // https://drafts.csswg.org/css-color-5/#rcs-intro
        // If the alpha value of the relative color is omitted, it defaults to that of the origin
        // color (rather than defaulting to 100%, as it does in the absolute syntax).
        if alpha_value.is_none() {
            synthesized_alpha = StyleValueData::Keyword {
                keyword: keyword::ALPHA,
            };
            alpha_value = Some(&synthesized_alpha);
        }
    }

    let resolved = resolve_channels_for(
        descriptor_for_color_type(color_type),
        [channel_0.data(), channel_1.data(), channel_2.data()],
        alpha_value,
        &effective_input,
    )?;
    construct_color(color_type, resolved.c1, resolved.c2, resolved.c3, resolved.alpha)
}

/// Port of native_channels_to_xyz65() in ColorStyleValue.cpp. The Gfx conversion chains are
/// reached through color_conversion::convert(), whose to-XYZ-D65 compositions match them
/// step for step; RGB, HSL and HWB first rescale into the fraction conventions convert()
/// expects, with RGB routed through the sRGB code because convert()'s RGB arm additionally
/// clamps to the 0..=1 gamut, which the C++ code does not.
fn native_channels_to_xyz65(native: Components, native_type: u8) -> Components {
    use color_conversion as ct;
    let converted = match native_type {
        ct::RGB => color_conversion::convert(
            ct::SRGB,
            ct::XYZ_D65,
            [native[0] / 255.0, native[1] / 255.0, native[2] / 255.0, native[3]],
        ),
        ct::HSL | ct::HWB => color_conversion::convert(
            native_type,
            ct::XYZ_D65,
            [native[0], native[1] / 100.0, native[2] / 100.0, native[3]],
        ),
        _ => color_conversion::convert(native_type, ct::XYZ_D65, native),
    };
    converted.expect("every color type converts to XYZ-D65")
}

/// Port of set_channels_for_target() in ColorStyleValue.cpp. Conversions out of XYZ-D65 and
/// the sRGB fraction reuse color_conversion::convert(), whose from-XYZ-D65 chains match the
/// Gfx compositions the C++ code spells out.
fn set_channels_for_target(
    context: &mut RelativeColorContext,
    target_color_type: u8,
    xyz65: Components,
    srgb_fraction: Components,
    linear_srgb: Components,
) {
    use channel_keyword as ck;
    use color_conversion as ct;
    let converted_from_xyz65 =
        |target: u8| color_conversion::convert(ct::XYZ_D65, target, xyz65).expect("XYZ-D65 converts to the target");
    let mut set = |slot: u8, value: f64| context[usize::from(slot)] = Some(value);
    match target_color_type {
        ct::RGB => {
            set(ck::R, f64::from(srgb_fraction[0]) * 255.0);
            set(ck::G, f64::from(srgb_fraction[1]) * 255.0);
            set(ck::B, f64::from(srgb_fraction[2]) * 255.0);
        }
        ct::HSL => {
            let hsl = color_conversion::convert(ct::SRGB, ct::HSL, srgb_fraction).expect("sRGB converts to HSL");
            set(ck::H, f64::from(hsl[0]));
            set(ck::S, f64::from(hsl[1]) * 100.0);
            set(ck::L, f64::from(hsl[2]) * 100.0);
        }
        ct::HWB => {
            let hwb = color_conversion::convert(ct::SRGB, ct::HWB, srgb_fraction).expect("sRGB converts to HWB");
            set(ck::H, f64::from(hwb[0]));
            set(ck::W, f64::from(hwb[1]) * 100.0);
            set(ck::B, f64::from(hwb[2]) * 100.0);
        }
        ct::LAB => {
            let lab = converted_from_xyz65(ct::LAB);
            set(ck::L, f64::from(lab[0]));
            set(ck::A, f64::from(lab[1]));
            set(ck::B, f64::from(lab[2]));
        }
        ct::LCH => {
            let lch = converted_from_xyz65(ct::LCH);
            set(ck::L, f64::from(lch[0]));
            set(ck::C, f64::from(lch[1]));
            set(ck::H, f64::from(lch[2]));
        }
        ct::OKLAB => {
            let oklab = converted_from_xyz65(ct::OKLAB);
            set(ck::L, f64::from(oklab[0]));
            set(ck::A, f64::from(oklab[1]));
            set(ck::B, f64::from(oklab[2]));
        }
        ct::OKLCH => {
            let oklch = converted_from_xyz65(ct::OKLCH);
            set(ck::L, f64::from(oklch[0]));
            set(ck::C, f64::from(oklch[1]));
            set(ck::H, f64::from(oklch[2]));
        }
        ct::SRGB => {
            set(ck::R, f64::from(srgb_fraction[0]));
            set(ck::G, f64::from(srgb_fraction[1]));
            set(ck::B, f64::from(srgb_fraction[2]));
        }
        ct::SRGB_LINEAR => {
            set(ck::R, f64::from(linear_srgb[0]));
            set(ck::G, f64::from(linear_srgb[1]));
            set(ck::B, f64::from(linear_srgb[2]));
        }
        ct::A98_RGB | ct::DISPLAY_P3 | ct::DISPLAY_P3_LINEAR | ct::PROPHOTO_RGB | ct::REC2020 => {
            let rgb = converted_from_xyz65(target_color_type);
            set(ck::R, f64::from(rgb[0]));
            set(ck::G, f64::from(rgb[1]));
            set(ck::B, f64::from(rgb[2]));
        }
        ct::XYZ_D50 => {
            let xyz50 = converted_from_xyz65(ct::XYZ_D50);
            set(ck::X, f64::from(xyz50[0]));
            set(ck::Y, f64::from(xyz50[1]));
            set(ck::Z, f64::from(xyz50[2]));
        }
        ct::XYZ_D65 => {
            set(ck::X, f64::from(xyz65[0]));
            set(ck::Y, f64::from(xyz65[1]));
            set(ck::Z, f64::from(xyz65[2]));
        }
        // The C++ switch is exhaustive over ColorType.
        _ => {}
    }
}

/// Port of resolve_origin_native_channels() in ColorStyleValue.cpp: resolves an absolute
/// color-function origin's channels without going through the u8 sRGB representation. The
/// C++ empty CalculationResolutionContext maps to no channels and no length context.
fn resolve_origin_native_channels(origin: &StyleValueData) -> Option<(u8, Components)> {
    let StyleValueData::ColorFunction {
        color_base,
        channel_0,
        channel_1,
        channel_2,
        alpha,
        origin_color,
        ..
    } = origin
    else {
        return None;
    };
    if origin_color.optional_data().is_some() {
        return None;
    }
    if !color_base.has_color_type {
        return None;
    }

    let descriptor = descriptor_for_color_type(color_base.color_type);
    let channel_value = |channel: &RetainedStyleValueData, index: usize| -> Option<f64> {
        let value = channel.data();
        if is_none_keyword(value) {
            return Some(0.0);
        }
        let channel_descriptor = &descriptor.channels[index];
        if channel_descriptor.is_hue {
            resolve_hue(value, &EMPTY_INPUT)
        } else {
            resolve_with_reference_value(value, f64::from(channel_descriptor.percent_reference), &EMPTY_INPUT)
        }
    };
    let alpha_value = || -> Option<f64> {
        match alpha.optional_data() {
            // An omitted alpha on a non-relative color function defaults to 1.
            None => Some(1.0),
            Some(alpha) if is_none_keyword(alpha) => Some(0.0),
            Some(alpha) => resolve_alpha(alpha, &EMPTY_INPUT),
        }
    };

    let c1 = channel_value(channel_0, 0)?;
    let c2 = channel_value(channel_1, 1)?;
    let c3 = channel_value(channel_2, 2)?;
    let alpha = alpha_value()?;

    Some((color_base.color_type, [c1 as f32, c2 as f32, c3 as f32, alpha as f32]))
}

/// Port of ColorFunctionStyleValue::resolve_relative_form(): flattens a relative color
/// function into an absolute one whose channels are plain numbers (or `none` keywords).
// https://drafts.csswg.org/css-color-5/#resolving-rcs
pub(crate) fn resolve_relative_form(value: &StyleValueData, input: &ColorResolutionInput) -> Option<StyleValueData> {
    let StyleValueData::ColorFunction {
        color_base,
        channel_0,
        channel_1,
        channel_2,
        alpha,
        origin_color,
        ..
    } = value
    else {
        return None;
    };
    // The C++ code VERIFYs both of these.
    let origin = origin_color.optional_data()?;
    if !color_base.has_color_type {
        return None;
    }
    let target_color_type = color_base.color_type;

    let relative_channels = extract_channels_in_color_space(origin, target_color_type, input)?;
    let mut channel_input = *input;
    channel_input.channels = Some(&relative_channels);

    let descriptor = descriptor_for_color_type(target_color_type);
    let resolve_channel = |channel: &RetainedStyleValueData, index: usize| -> StyleValueData {
        let value = channel.data();
        if is_none_keyword(value) {
            return StyleValueData::Keyword { keyword: keyword::NONE };
        }
        let channel_descriptor = &descriptor.channels[index];
        let resolved = if channel_descriptor.is_hue {
            resolve_hue(value, &channel_input)
        } else {
            resolve_with_reference_value(value, f64::from(channel_descriptor.percent_reference), &channel_input)
        };
        StyleValueData::Number {
            value: resolved.unwrap_or(0.0),
        }
    };

    let resolved_alpha = {
        // https://drafts.csswg.org/css-color-5/#rcs-intro
        // If the alpha value of the relative color is omitted, it defaults to that of the origin
        // color (rather than defaulting to 100%, as it does in the absolute syntax).
        let synthesized_alpha;
        let effective_alpha = match alpha.optional_data() {
            Some(alpha) => alpha,
            None => {
                synthesized_alpha = StyleValueData::Keyword {
                    keyword: keyword::ALPHA,
                };
                &synthesized_alpha
            }
        };
        if is_none_keyword(effective_alpha) {
            StyleValueData::Keyword { keyword: keyword::NONE }
        } else {
            StyleValueData::Number {
                value: resolve_alpha(effective_alpha, &channel_input).unwrap_or(1.0),
            }
        }
    };

    Some(StyleValueData::ColorFunction {
        color_base: *color_base,
        channel_0: retained_value(resolve_channel(channel_0, 0)),
        channel_1: retained_value(resolve_channel(channel_1, 1)),
        channel_2: retained_value(resolve_channel(channel_2, 2)),
        alpha: retained_value(resolved_alpha),
        has_name: false,
        name: RetainedUtf16FlyString::none(),
        origin_color: retained_null(),
    })
}

/// Port of ColorStyleValue::extract_channels_in_color_space().
///
/// NB: The C++ code computes linear sRGB with the direct transfer function; that function is
/// private to color_conversion, so linear sRGB (and the gamma encode of an sRGB-linear origin)
/// hop through XYZ-D65 via convert() here. The difference is one f32 matrix round-trip, as with
/// from_converted() above.
// https://drafts.csswg.org/css-color-5/#resolving-rcs
fn extract_channels_in_color_space(
    origin_color: &StyleValueData,
    target_color_type: u8,
    input: &ColorResolutionInput,
) -> Option<RelativeColorContext> {
    use channel_keyword as ck;
    use color_conversion as ct;

    let mut effective_origin = origin_color;
    if let StyleValueData::Keyword { keyword: code } = effective_origin
        && *code == keyword::CURRENTCOLOR
        && let Some(current_color_value) = input.current_color_value
        && !value_depends_on_current_color(current_color_value)
    {
        effective_origin = current_color_value;
    }

    let resolved_origin_storage;
    if let StyleValueData::ColorFunction {
        origin_color: nested_origin,
        ..
    } = effective_origin
        && nested_origin.optional_data().is_some()
    {
        resolved_origin_storage = resolve_relative_form(effective_origin, input)?;
        effective_origin = &resolved_origin_storage;
    }

    // High-precision path: for color-function origins, resolve channels directly without going
    // through the u8 sRGB representation, which clips wide-gamut colors and loses precision.
    if let Some((origin_native_type, native_channels)) = resolve_origin_native_channels(effective_origin) {
        let mut context: RelativeColorContext = [None; 13];
        context[usize::from(ck::ALPHA)] = Some(f64::from(native_channels[3]));

        // If the target matches the origin's native type, use the origin's channel values
        // directly to avoid round-trip losses.
        if origin_native_type == target_color_type {
            let slots = match target_color_type {
                ct::RGB => (ck::R, ck::G, ck::B),
                ct::HSL => (ck::H, ck::S, ck::L),
                ct::HWB => (ck::H, ck::W, ck::B),
                ct::LAB | ct::OKLAB => (ck::L, ck::A, ck::B),
                ct::LCH | ct::OKLCH => (ck::L, ck::C, ck::H),
                ct::SRGB
                | ct::SRGB_LINEAR
                | ct::A98_RGB
                | ct::DISPLAY_P3
                | ct::DISPLAY_P3_LINEAR
                | ct::PROPHOTO_RGB
                | ct::REC2020 => (ck::R, ck::G, ck::B),
                ct::XYZ_D50 | ct::XYZ_D65 => (ck::X, ck::Y, ck::Z),
                // The C++ switch is exhaustive over ColorType.
                _ => return None,
            };
            context[usize::from(slots.0)] = Some(f64::from(native_channels[0]));
            context[usize::from(slots.1)] = Some(f64::from(native_channels[1]));
            context[usize::from(slots.2)] = Some(f64::from(native_channels[2]));
            return Some(context);
        }

        // When both origin and target are in the sRGB gamut family, convert directly to avoid
        // matrix round-trip precision loss.
        let is_srgb_family =
            |color_type: u8| matches!(color_type, ct::RGB | ct::HSL | ct::HWB | ct::SRGB | ct::SRGB_LINEAR);

        if is_srgb_family(origin_native_type) && is_srgb_family(target_color_type) {
            let mut srgb_fraction = match origin_native_type {
                ct::RGB => [
                    native_channels[0] / 255.0,
                    native_channels[1] / 255.0,
                    native_channels[2] / 255.0,
                    native_channels[3],
                ],
                ct::HSL | ct::HWB => color_conversion::legacy_color_to_srgb(
                    origin_native_type,
                    [
                        native_channels[0],
                        native_channels[1] / 100.0,
                        native_channels[2] / 100.0,
                        native_channels[3],
                    ],
                )
                .expect("HSL and HWB convert to sRGB"),
                ct::SRGB => native_channels,
                ct::SRGB_LINEAR => color_conversion::convert(ct::SRGB_LINEAR, ct::SRGB, native_channels)
                    .expect("linear sRGB converts to sRGB"),
                // The C++ switch VERIFYs past the family members.
                _ => return None,
            };
            srgb_fraction[3] = native_channels[3];
            let linear_srgb = color_conversion::convert(ct::SRGB, ct::SRGB_LINEAR, srgb_fraction)
                .expect("sRGB converts to linear sRGB");
            let xyz65 =
                color_conversion::convert(ct::SRGB, ct::XYZ_D65, srgb_fraction).expect("sRGB converts to XYZ-D65");
            set_channels_for_target(&mut context, target_color_type, xyz65, srgb_fraction, linear_srgb);
            return Some(context);
        }

        let xyz65 = native_channels_to_xyz65(native_channels, origin_native_type);
        let linear_srgb =
            color_conversion::convert(ct::XYZ_D65, ct::SRGB_LINEAR, xyz65).expect("XYZ-D65 converts to linear sRGB");
        let srgb_fraction = color_conversion::convert(ct::XYZ_D65, ct::SRGB, xyz65).expect("XYZ-D65 converts to sRGB");
        set_channels_for_target(&mut context, target_color_type, xyz65, srgb_fraction, linear_srgb);

        return Some(context);
    }

    // Low-precision fallback through the resolved 8-bit color.
    let color = to_color(effective_origin, input)?;

    let mut context: RelativeColorContext = [None; 13];
    let srgb_fraction: Components = [
        f32::from(color.r) / 255.0,
        f32::from(color.g) / 255.0,
        f32::from(color.b) / 255.0,
        f32::from(color.a) / 255.0,
    ];

    context[usize::from(ck::ALPHA)] = Some(f64::from(srgb_fraction[3]));

    let linear_srgb =
        color_conversion::convert(ct::SRGB, ct::SRGB_LINEAR, srgb_fraction).expect("sRGB converts to linear sRGB");
    let xyz65 = color_conversion::convert(ct::SRGB, ct::XYZ_D65, srgb_fraction).expect("sRGB converts to XYZ-D65");
    set_channels_for_target(&mut context, target_color_type, xyz65, srgb_fraction, linear_srgb);

    Some(context)
}

/// Port of Percentage::from_style_value(). The C++ version VERIFYs on the remaining cases;
/// here they surface as None and invalidate the surrounding color.
pub(crate) fn percentage_from_style_value(value: &StyleValueData) -> Option<f64> {
    match value {
        StyleValueData::Percentage { value } => Some(*value),
        StyleValueData::Calculated { .. } => resolve_calculated_percentage_with_channels(value, None, None),
        _ => None,
    }
}

/// Port of ColorMixStyleValue::NormalizedPercentages; percentages are 0-100 values.
pub(crate) struct NormalizedPercentages {
    pub(crate) first_percentage: f64,
    pub(crate) second_percentage: f64,
    pub(crate) alpha_multiplier: f64,
}

/// Port of ColorMixStyleValue::normalize_percentage_pair().
// https://drafts.csswg.org/css-color-5/#color-mix-percent-norm
pub(crate) fn normalize_percentage_pair(p1: Option<f64>, p2: Option<f64>) -> NormalizedPercentages {
    let mut alpha_multiplier = 1.0;

    // 1. Let p1 be the first percentage and p2 the second one.
    // NB: Provided by the caller.
    let (p1, p2) = match (p1, p2) {
        // 2. If both percentages are omitted, they each default to 50% (an equal mix of the two
        //    colors).
        (None, None) => (50.0, 50.0),
        // 3. Otherwise, if p2 is omitted, it becomes 100% - p1
        (Some(p1), None) => (p1, 100.0 - p1),
        // 4. Otherwise, if p1 is omitted, it becomes 100% - p2
        (None, Some(p2)) => (100.0 - p2, p2),
        (Some(mut p1), Some(mut p2)) => {
            // 5. Otherwise, if both are provided and add up to greater than 100%, they are scaled
            //    accordingly so that they add up to 100%.
            if p1 + p2 > 100.0 {
                let sum = p1 + p2;
                p1 = (p1 / sum) * 100.0;
                p2 = (p2 / sum) * 100.0;
            }
            // 6. Otherwise, if both are provided and add up to less than 100%, the sum is saved as
            //    an alpha multiplier. If the sum is greater than zero, they are then scaled
            //    accordingly so that they add up to 100%.
            else if p1 + p2 < 100.0 {
                let sum = p1 + p2;
                alpha_multiplier = sum / 100.0;
                if sum > 0.0 {
                    p1 = (p1 / sum) * 100.0;
                    p2 = (p2 / sum) * 100.0;
                }
            }
            (p1, p2)
        }
    };

    NormalizedPercentages {
        first_percentage: p1,
        second_percentage: p2,
        alpha_multiplier,
    }
}

/// Port of resolve_native_color_components() in ColorMixStyleValue.cpp.
fn resolve_native_color_components(
    style_value: &StyleValueData,
    input: &ColorResolutionInput,
) -> Option<(u8, Components)> {
    use color_conversion as ct;
    let StyleValueData::ColorFunction {
        color_base,
        channel_0,
        channel_1,
        channel_2,
        alpha,
        origin_color,
        ..
    } = style_value
    else {
        return None;
    };
    if !color_base.has_color_type || origin_color.optional_data().is_some() {
        return None;
    }

    let resolve_alpha_value = || -> Option<f64> {
        match alpha.optional_data() {
            // An omitted alpha on a color function is treated as 1 for interpolation.
            None => Some(1.0),
            Some(alpha) => resolve_alpha(alpha, input),
        }
    };
    let (c0, c1, c2) = (channel_0.data(), channel_1.data(), channel_2.data());

    let components = match color_base.color_type {
        ct::HSL => {
            let h = resolve_hue(c0, input)?;
            let s = resolve_with_reference_value(c1, 100.0, input)?;
            let l = resolve_with_reference_value(c2, 100.0, input)?;
            let a = resolve_alpha_value()?;
            [h as f32, (s / 100.0) as f32, (l / 100.0) as f32, a as f32]
        }
        ct::HWB => {
            let h = resolve_hue(c0, input)?;
            let w = resolve_with_reference_value(c1, 100.0, input)?;
            let b = resolve_with_reference_value(c2, 100.0, input)?;
            let a = resolve_alpha_value()?;
            [h as f32, (w / 100.0) as f32, (b / 100.0) as f32, a as f32]
        }
        ct::LAB => {
            let l = resolve_with_reference_value(c0, 100.0, input)?;
            let a_component = resolve_with_reference_value(c1, 125.0, input)?;
            let b_component = resolve_with_reference_value(c2, 125.0, input)?;
            let alpha = resolve_alpha_value()?;
            [l as f32, a_component as f32, b_component as f32, alpha as f32]
        }
        ct::OKLAB => {
            let l = resolve_with_reference_value(c0, 1.0, input)?;
            let a_component = resolve_with_reference_value(c1, 0.4, input)?;
            let b_component = resolve_with_reference_value(c2, 0.4, input)?;
            let alpha = resolve_alpha_value()?;
            [l as f32, a_component as f32, b_component as f32, alpha as f32]
        }
        ct::LCH => {
            let l = resolve_with_reference_value(c0, 100.0, input)?;
            let c = resolve_with_reference_value(c1, 150.0, input)?;
            let h = resolve_hue(c2, input)?;
            let a = resolve_alpha_value()?;
            [l as f32, c as f32, h as f32, a as f32]
        }
        ct::OKLCH => {
            let l = resolve_with_reference_value(c0, 1.0, input)?;
            let c = resolve_with_reference_value(c1, 0.4, input)?;
            let h = resolve_hue(c2, input)?;
            let a = resolve_alpha_value()?;
            [l as f32, c as f32, h as f32, a as f32]
        }
        ct::RGB => {
            let r = resolve_with_reference_value(c0, 255.0, input)?;
            let g = resolve_with_reference_value(c1, 255.0, input)?;
            let b = resolve_with_reference_value(c2, 255.0, input)?;
            let a = resolve_alpha_value()?;
            [
                (r.clamp(0.0, 255.0) / 255.0) as f32,
                (g.clamp(0.0, 255.0) / 255.0) as f32,
                (b.clamp(0.0, 255.0) / 255.0) as f32,
                a as f32,
            ]
        }
        _ => {
            let first = resolve_with_reference_value(c0, 1.0, input)?;
            let second = resolve_with_reference_value(c1, 1.0, input)?;
            let third = resolve_with_reference_value(c2, 1.0, input)?;
            let alpha = resolve_alpha_value()?;
            [first as f32, second as f32, third as f32, alpha as f32]
        }
    };
    Some((color_base.color_type, components))
}

/// Port of resolve_color_for_rust_interpolation() in ColorMixStyleValue.cpp.
pub(crate) fn resolve_color_for_interpolation(
    input_value: &StyleValueData,
    input: &ColorResolutionInput,
) -> Option<FfiResolvedColor> {
    let resolved_relative_storage;
    let mut style_value = input_value;
    if let StyleValueData::ColorFunction { origin_color, .. } = input_value
        && origin_color.optional_data().is_some()
    {
        resolved_relative_storage = resolve_relative_form(input_value, input)?;
        style_value = &resolved_relative_storage;
    }

    if let Some((color_type, components)) = resolve_native_color_components(style_value, input) {
        let StyleValueData::ColorFunction {
            channel_0,
            channel_1,
            channel_2,
            alpha,
            ..
        } = style_value
        else {
            // resolve_native_color_components() only accepts color functions.
            return None;
        };
        return Some(FfiResolvedColor {
            color_type,
            components,
            missing: [
                is_none_keyword(channel_0.data()),
                is_none_keyword(channel_1.data()),
                is_none_keyword(channel_2.data()),
                alpha.optional_data().is_some_and(is_none_keyword),
            ],
            has_native_components: true,
        });
    }

    let color = to_color(style_value, input)?;
    // Gfx::color_to_srgb(): the u8 channels back to fractions.
    Some(FfiResolvedColor {
        color_type: color_conversion::SRGB,
        components: [
            f32::from(color.r) / 255.0,
            f32::from(color.g) / 255.0,
            f32::from(color.b) / 255.0,
            f32::from(color.a) / 255.0,
        ],
        missing: [false; 4],
        has_native_components: false,
    })
}

/// Port of ColorMixStyleValue::to_color() and interpolate_color_in_rust(), reaching the
/// interpolation entry point in-crate instead of through the C++ round trip.
// https://drafts.csswg.org/css-color-5/#color-mix-result
fn color_mix_to_color(value: &StyleValueData, input: &ColorResolutionInput) -> Option<Rgba> {
    let StyleValueData::ColorMix {
        color_interpolation_method,
        first_color,
        first_percentage,
        second_color,
        second_percentage,
        ..
    } = value
    else {
        return None;
    };

    let percentage = |value: &RetainedStyleValueData| -> Option<Option<f64>> {
        match value.optional_data() {
            None => Some(None),
            Some(value) => Some(Some(percentage_from_style_value(value)?)),
        }
    };
    let p1 = percentage(first_percentage)?;
    let p2 = percentage(second_percentage)?;
    let normalized = normalize_percentage_pair(p1, p2);

    // An absent method interpolates in the rectangular Oklab space.
    let default_method = StyleValueData::ColorInterpolationMethod {
        is_polar: false,
        color_space: RECTANGULAR_COLOR_SPACE_OKLAB,
        hue_interpolation_method: 0,
    };
    let method = color_interpolation_method.optional_data().unwrap_or(&default_method);

    let resolved_from = resolve_color_for_interpolation(first_color.data(), input)?;
    let resolved_to = resolve_color_for_interpolation(second_color.data(), input)?;
    let delta = (normalized.second_percentage / 100.0) as f32;
    // SAFETY: All pointers stay live for the duration of the call.
    let result = unsafe {
        rust_interpolate_color(
            &raw const resolved_from,
            &raw const resolved_to,
            std::ptr::from_ref(method),
            delta,
            normalized.alpha_multiplier as f32,
        )
    };
    if result.is_null() {
        return None;
    }
    // SAFETY: The returned pointer owns exactly one strong reference.
    let result = unsafe { Arc::from_raw(result) };
    to_color(&result, input)
}

/// The color-resolution fan-out over the color-bearing style value variants: the union of
/// ColorStyleValue::to_color() and KeywordStyleValue::to_color(). Non-color variants resolve
/// to None.
pub(crate) fn to_color(value: &StyleValueData, input: &ColorResolutionInput) -> Option<Rgba> {
    match value {
        // Port of KeywordStyleValue::to_color(); an unset scheme falls back to Light, matching
        // the C++ value_or(PreferredColorScheme::Light).
        StyleValueData::Keyword { keyword: code } => {
            if *code == keyword::CURRENTCOLOR {
                return Some(input.current_color.unwrap_or(Rgba::BLACK));
            }
            system_color_for_keyword(*code, input.dark())
        }
        StyleValueData::ColorFunction { .. } => color_function_to_color(value, input),
        StyleValueData::ColorMix { .. } => color_mix_to_color(value, input),
        // Port of ContrastColorStyleValue::to_color().
        StyleValueData::ContrastColor { color, .. } => {
            Some(to_color(color.data(), input)?.suggested_foreground_color())
        }
        // Port of LightDarkStyleValue::to_color(): a missing scheme takes the light branch.
        StyleValueData::LightDark { light, dark, .. } => {
            if input.scheme == Some(PREFERRED_COLOR_SCHEME_DARK) {
                to_color(dark.data(), input)
            } else {
                to_color(light.data(), input)
            }
        }
        _ => None,
    }
}

#[cfg(test)]
#[allow(clippy::items_after_test_module)]
mod tests {
    use super::*;
    use crate::css::style_value::ColorBase;

    fn fraction_for(alpha: u8) -> String {
        let (digits, length) = format_to_8bit_compatible(alpha);
        String::from_utf8(digits[..length].to_vec()).unwrap()
    }

    #[test]
    fn shortest_roundtripping_alpha_fraction() {
        assert_eq!(fraction_for(127), "498");
        assert_eq!(fraction_for(128), "5");
        assert_eq!(fraction_for(64), "25");
    }

    #[test]
    fn serializes_srgb_values() {
        let serialize = |color: Rgba| {
            let mut sink = TextSink::new();
            color.serialize_a_srgb_value(&mut sink);
            sink
        };
        let expect = |text: &str| {
            let mut sink = TextSink::new();
            sink.push_ascii(text);
            sink
        };
        assert!(
            serialize(Rgba {
                r: 1,
                g: 2,
                b: 3,
                a: 255
            })
            .content_equals(&expect("rgb(1, 2, 3)"))
        );
        assert!(serialize(Rgba { r: 1, g: 2, b: 3, a: 0 }).content_equals(&expect("rgba(1, 2, 3, 0)")));
        assert!(
            serialize(Rgba {
                r: 1,
                g: 2,
                b: 3,
                a: 128
            })
            .content_equals(&expect("rgba(1, 2, 3, 0.5)"))
        );
        assert!(
            serialize(Rgba {
                r: 1,
                g: 2,
                b: 3,
                a: 64
            })
            .content_equals(&expect("rgba(1, 2, 3, 0.25)"))
        );
    }

    #[test]
    fn hsla_matches_gfx_color() {
        assert_eq!(Rgba::from_hsla(0.0, 1.0, 0.5, 1.0), rgb(255, 0, 0));
        assert_eq!(Rgba::from_hsla(120.0, 1.0, 0.5, 1.0), rgb(0, 255, 0));
        // 0.5 * 255 = 127.5 rounds half away from zero to 128, matching lroundf.
        assert_eq!(
            Rgba::from_hsla(240.0, 1.0, 0.25, 0.5),
            Rgba {
                r: 0,
                g: 0,
                b: 128,
                a: 128
            }
        );
        // Saturation and lightness clamp before conversion.
        assert_eq!(Rgba::from_hsla(0.0, 2.0, 1.5, 1.0), Rgba::WHITE);
    }

    #[test]
    fn hsv_matches_gfx_color() {
        assert_eq!(Rgba::from_hsv(0.0, 0.0, 0.0), Rgba::BLACK);
        assert_eq!(Rgba::from_hsv(0.0, 0.0, 1.0), Rgba::WHITE);
        assert_eq!(Rgba::from_hsv(120.0, 1.0, 1.0), rgb(0, 255, 0));
    }

    #[test]
    fn quantizes_wide_gamut_endpoints() {
        assert_eq!(Rgba::from_srgb(0.5, 0.5, 0.5, 1.0), rgb(128, 128, 128));
        assert_eq!(Rgba::from_linear_srgb(0.0, 0.0, 0.0, 1.0), Rgba::BLACK);
        assert_eq!(Rgba::from_linear_srgb(1.0, 1.0, 1.0, 1.0), Rgba::WHITE);
        assert_eq!(Rgba::from_display_p3(1.0, 1.0, 1.0, 1.0), Rgba::WHITE);
        assert_eq!(Rgba::from_rec2020(0.0, 0.0, 0.0, 1.0), Rgba::BLACK);
        assert_eq!(Rgba::from_lab(100.0, 0.0, 0.0, 1.0), Rgba::WHITE);
        assert_eq!(Rgba::from_oklab(1.0, 0.0, 0.0, 1.0), Rgba::WHITE);
    }

    #[test]
    fn darkened_truncates_like_the_cpp_float_conversion() {
        assert_eq!(
            Rgba {
                r: 100,
                g: 200,
                b: 50,
                a: 255
            }
            .darkened(0.5),
            Rgba {
                r: 50,
                g: 100,
                b: 25,
                a: 255
            }
        );
        // 212 * 0.8 = 169.6 truncates to 169.
        assert_eq!(rgb(212, 208, 200).darkened(0.8), rgb(169, 166, 160));
    }

    #[test]
    fn luminance_and_contrast() {
        assert_eq!(Rgba::BLACK.relative_luminance(), 0.0);
        assert!((Rgba::WHITE.relative_luminance() - 1.0).abs() < 0.00001);
        assert!((Rgba::BLACK.contrast_ratio(Rgba::WHITE) - 21.0).abs() < 0.001);
        assert_eq!(Rgba::WHITE.suggested_foreground_color(), Rgba::BLACK);
        assert_eq!(Rgba::BLACK.suggested_foreground_color(), Rgba::WHITE);
    }

    #[test]
    fn descriptor_table_matches_frozen_codes() {
        assert_eq!(descriptor_for_color_type(color_conversion::RGB).name, "rgb");
        assert_eq!(descriptor_for_color_type(color_conversion::HSL).name, "hsl");
        assert_eq!(descriptor_for_color_type(color_conversion::OKLCH).name, "oklch");
        assert_eq!(descriptor_for_color_type(color_conversion::XYZ_D65).name, "xyz-d65");
        let hsl = descriptor_for_color_type(color_conversion::HSL);
        assert!(hsl.channels[0].is_hue);
        assert!(hsl.absolutizes_to_rgb);
        let lch = descriptor_for_color_type(color_conversion::LCH);
        assert_eq!(lch.channels[1].percent_reference, 150.0);
        assert_eq!(lch.channels[0].serialize_clamp_max, Some(100.0));
        assert!(lch.channels[2].is_hue);
        assert_eq!(
            descriptor_for_color_type(color_conversion::SRGB_LINEAR).behavior,
            SerializationBehavior::ColorFunction
        );
    }

    fn number(value: f64) -> StyleValueData {
        StyleValueData::Number { value }
    }

    fn percentage(value: f64) -> StyleValueData {
        StyleValueData::Percentage { value }
    }

    fn keyword_value(code: u16) -> StyleValueData {
        StyleValueData::Keyword { keyword: code }
    }

    fn color_function(color_type: u8, channels: [StyleValueData; 3], alpha: Option<StyleValueData>) -> StyleValueData {
        let [c0, c1, c2] = channels;
        StyleValueData::ColorFunction {
            color_base: ColorBase {
                has_color_type: true,
                color_type,
                color_syntax: 0,
            },
            channel_0: retained_value(c0),
            channel_1: retained_value(c1),
            channel_2: retained_value(c2),
            alpha: alpha.map_or_else(retained_null, retained_value),
            has_name: false,
            name: RetainedUtf16FlyString::none(),
            origin_color: retained_null(),
        }
    }

    #[test]
    fn hue_resolution_normalizes_like_the_cpp_resolver() {
        let hue = |value: &StyleValueData| resolve_hue(value, &EMPTY_INPUT);
        assert_eq!(hue(&number(0.0)), Some(0.0));
        assert_eq!(hue(&number(370.0)), Some(10.0));
        assert_eq!(hue(&number(-30.0)), Some(330.0));
        // +inf clamps to 360, which then wraps to 0; -inf and NaN clamp to 0.
        assert_eq!(hue(&number(f64::INFINITY)), Some(0.0));
        assert_eq!(hue(&number(f64::NEG_INFINITY)), Some(0.0));
        assert_eq!(hue(&number(f64::NAN)), Some(0.0));
        // Angles resolve through their canonical-degree ratios: deg, then turn.
        assert_eq!(hue(&StyleValueData::Angle { value: 450.0, unit: 0 }), Some(90.0));
        assert_eq!(hue(&StyleValueData::Angle { value: 0.5, unit: 3 }), Some(180.0));
        // The `none` keyword (and any non-channel keyword) falls through to 0.
        assert_eq!(hue(&keyword_value(keyword::NONE)), Some(0.0));

        // Channel keywords need a relative-color context and normalize their value.
        let mut channels: RelativeColorContext = [None; 13];
        channels[usize::from(channel_keyword::H)] = Some(400.0);
        let input = ColorResolutionInput {
            channels: Some(&channels),
            ..EMPTY_INPUT
        };
        assert_eq!(resolve_hue(&keyword_value(keyword::H), &input), Some(40.0));
        assert_eq!(resolve_hue(&keyword_value(keyword::H), &EMPTY_INPUT), None);
        // A channel without a resolved value is still unresolvable.
        assert_eq!(resolve_hue(&keyword_value(keyword::S), &input), None);
    }

    #[test]
    fn alpha_resolution_clamps_like_the_cpp_resolver() {
        let alpha = |value: &StyleValueData| resolve_alpha(value, &EMPTY_INPUT);
        assert_eq!(alpha(&number(0.5)), Some(0.5));
        assert_eq!(alpha(&number(2.0)), Some(1.0));
        assert_eq!(alpha(&number(-1.0)), Some(0.0));
        assert_eq!(alpha(&number(f64::NAN)), Some(0.0));
        assert_eq!(alpha(&percentage(50.0)), Some(0.5));
        assert_eq!(alpha(&percentage(250.0)), Some(1.0));
        assert_eq!(alpha(&percentage(-10.0)), Some(0.0));
        // `none` resolves to 0; any other non-channel keyword takes the fallback 1.
        assert_eq!(alpha(&keyword_value(keyword::NONE)), Some(0.0));
        assert_eq!(alpha(&keyword_value(keyword::AUTO)), Some(1.0));

        let mut channels: RelativeColorContext = [None; 13];
        channels[usize::from(channel_keyword::ALPHA)] = Some(2.5);
        let input = ColorResolutionInput {
            channels: Some(&channels),
            ..EMPTY_INPUT
        };
        assert_eq!(resolve_alpha(&keyword_value(keyword::ALPHA), &input), Some(1.0));
        assert_eq!(resolve_alpha(&keyword_value(keyword::ALPHA), &EMPTY_INPUT), None);
    }

    #[test]
    fn reference_value_resolution_scales_percentages() {
        let resolve =
            |value: &StyleValueData, reference: f64| resolve_with_reference_value(value, reference, &EMPTY_INPUT);
        assert_eq!(resolve(&number(12.5), 255.0), Some(12.5));
        assert_eq!(resolve(&percentage(50.0), 255.0), Some(127.5));
        assert_eq!(resolve(&keyword_value(keyword::NONE), 255.0), Some(0.0));
        assert_eq!(resolve(&keyword_value(keyword::R), 255.0), None);
    }

    #[test]
    fn to_color_resolves_rgb_and_hsl_functions() {
        let rgb_value = color_function(color_conversion::RGB, [number(255.0), number(127.5), number(0.0)], None);
        // clamp_to_byte rounds half away from zero: 127.5 becomes 128.
        assert_eq!(
            to_color(&rgb_value, &EMPTY_INPUT),
            Some(Rgba {
                r: 255,
                g: 128,
                b: 0,
                a: 255
            })
        );

        let translucent = color_function(
            color_conversion::RGB,
            [number(1.0), number(2.0), number(3.0)],
            Some(number(0.5)),
        );
        assert_eq!(
            to_color(&translucent, &EMPTY_INPUT),
            Some(Rgba {
                r: 1,
                g: 2,
                b: 3,
                a: 128
            })
        );

        let hsl_value = color_function(
            color_conversion::HSL,
            [number(120.0), percentage(100.0), percentage(50.0)],
            None,
        );
        assert_eq!(to_color(&hsl_value, &EMPTY_INPUT), Some(rgb(0, 255, 0)));
    }

    #[test]
    fn to_color_resolves_keywords_and_light_dark() {
        // currentcolor takes the used value, falling back to black.
        assert_eq!(
            to_color(&keyword_value(keyword::CURRENTCOLOR), &EMPTY_INPUT),
            Some(Rgba::BLACK)
        );
        let red = rgb(255, 0, 0);
        let input = ColorResolutionInput {
            current_color: Some(red),
            ..EMPTY_INPUT
        };
        assert_eq!(to_color(&keyword_value(keyword::CURRENTCOLOR), &input), Some(red));

        // System colors follow the scheme; a missing scheme means light.
        assert_eq!(
            to_color(&keyword_value(keyword::CANVAS), &EMPTY_INPUT),
            Some(rgb(255, 255, 255))
        );
        let dark_input = ColorResolutionInput {
            scheme: Some(PREFERRED_COLOR_SCHEME_DARK),
            ..EMPTY_INPUT
        };
        assert_eq!(
            to_color(&keyword_value(keyword::CANVAS), &dark_input),
            Some(rgb(20, 20, 20))
        );
        assert_eq!(to_color(&keyword_value(keyword::AUTO), &EMPTY_INPUT), None);

        let light_dark = StyleValueData::LightDark {
            color_base: ColorBase {
                has_color_type: false,
                color_type: 0,
                color_syntax: 0,
            },
            light: retained_value(color_function(
                color_conversion::RGB,
                [number(255.0), number(255.0), number(255.0)],
                None,
            )),
            dark: retained_value(color_function(
                color_conversion::RGB,
                [number(0.0), number(0.0), number(0.0)],
                None,
            )),
        };
        assert_eq!(to_color(&light_dark, &EMPTY_INPUT), Some(Rgba::WHITE));
        assert_eq!(to_color(&light_dark, &dark_input), Some(Rgba::BLACK));
    }

    #[test]
    fn to_color_mixes_srgb_endpoints() {
        let mix = StyleValueData::ColorMix {
            color_base: ColorBase {
                has_color_type: false,
                color_type: 0,
                color_syntax: 0,
            },
            // Gfx::RectangularColorSpace::Srgb is 0.
            color_interpolation_method: retained_value(StyleValueData::ColorInterpolationMethod {
                is_polar: false,
                color_space: 0,
                hue_interpolation_method: 0,
            }),
            first_color: retained_value(color_function(
                color_conversion::RGB,
                [number(0.0), number(0.0), number(0.0)],
                None,
            )),
            first_percentage: retained_null(),
            second_color: retained_value(color_function(
                color_conversion::RGB,
                [number(255.0), number(255.0), number(255.0)],
                None,
            )),
            second_percentage: retained_null(),
        };
        // A 50/50 sRGB mix of black and white; 127.5 rounds half-to-even to 128 through the
        // sRGB construction arm.
        assert_eq!(to_color(&mix, &EMPTY_INPUT), Some(rgb(128, 128, 128)));
    }

    #[test]
    fn percentage_pair_normalization_matches_the_spec_steps() {
        let normalized = normalize_percentage_pair(None, None);
        assert_eq!(normalized.first_percentage, 50.0);
        assert_eq!(normalized.second_percentage, 50.0);
        assert_eq!(normalized.alpha_multiplier, 1.0);

        let normalized = normalize_percentage_pair(Some(30.0), None);
        assert_eq!(normalized.second_percentage, 70.0);

        let normalized = normalize_percentage_pair(None, Some(30.0));
        assert_eq!(normalized.first_percentage, 70.0);

        let normalized = normalize_percentage_pair(Some(100.0), Some(100.0));
        assert_eq!(normalized.first_percentage, 50.0);
        assert_eq!(normalized.second_percentage, 50.0);
        assert_eq!(normalized.alpha_multiplier, 1.0);

        let normalized = normalize_percentage_pair(Some(20.0), Some(30.0));
        assert_eq!(normalized.first_percentage, 40.0);
        assert_eq!(normalized.second_percentage, 60.0);
        assert_eq!(normalized.alpha_multiplier, 0.5);

        let normalized = normalize_percentage_pair(Some(0.0), Some(0.0));
        assert_eq!(normalized.alpha_multiplier, 0.0);
    }

    #[test]
    fn system_colors_cover_both_schemes() {
        assert_eq!(
            system_color_for_keyword(keyword::CANVAS, false),
            Some(rgb(255, 255, 255))
        );
        assert_eq!(system_color_for_keyword(keyword::CANVAS, true), Some(rgb(20, 20, 20)));
        assert_eq!(
            system_color_for_keyword(keyword::SCROLLBAR, false),
            Some(rgb(255, 255, 255))
        );
        assert_eq!(
            system_color_for_keyword(keyword::HIGHLIGHT, false),
            Some(rgb(61, 174, 233).with_alpha(128))
        );
        assert_eq!(
            system_color_for_keyword(keyword::_LIBWEB_BUTTONFACEDISABLED, false),
            Some(rgb(212, 208, 200).with_alpha(128))
        );
        assert_eq!(
            system_color_for_keyword(keyword::_LIBWEB_BUTTONFACEHOVER, false),
            Some(rgb(169, 166, 160))
        );
        assert_eq!(
            system_color_for_keyword(keyword::VISITEDTEXT, true),
            Some(rgb(156, 113, 212))
        );
        assert_eq!(system_color_for_keyword(keyword::CURRENTCOLOR, false), None);
        assert_eq!(system_color_for_keyword(keyword::AUTO, false), None);
    }
}

/// The plain-data resolution context C++ marshals for the FFI entry.
#[repr(C)]
pub struct FfiColorResolutionInput {
    pub has_scheme: bool,
    pub scheme: u8,
    pub has_current_color: bool,
    pub current_color_rgba: [u8; 4],
    /// Full-precision currentcolor style value data, or null.
    pub current_color_value: *const core::ffi::c_void,
    /// Length resolution context, or null.
    pub length: *const core::ffi::c_void,
    pub channels_present: [bool; 13],
    pub channels: [f64; 13],
    pub has_channels: bool,
}

/// A resolved color handed back to C++; `resolved` is false when the Rust resolution
/// declined and the C++ path must run.
#[repr(C)]
pub struct FfiResolvedColorValue {
    pub resolved: bool,
    pub rgba: [u8; 4],
}

/// Decodes an FFI input's relative-color channel values into the borrowable
/// storage `resolution_input_from_ffi` takes.
pub(crate) fn relative_color_context_from_ffi(input: &FfiColorResolutionInput) -> RelativeColorContext {
    let mut channels: RelativeColorContext = [None; 13];
    if input.has_channels {
        for (slot, (&present, &channel)) in channels
            .iter_mut()
            .zip(input.channels_present.iter().zip(input.channels.iter()))
        {
            if present {
                *slot = Some(channel);
            }
        }
    }
    channels
}

/// Borrows a marshalled FFI resolution input as the resolver's input type;
/// `channels` is the decoded storage from `relative_color_context_from_ffi`.
///
/// # Safety
/// The input's current-color value and length-context pointers must stay live
/// for the returned borrow's lifetime.
pub(crate) unsafe fn resolution_input_from_ffi<'a>(
    input: &'a FfiColorResolutionInput,
    channels: &'a RelativeColorContext,
) -> ColorResolutionInput<'a> {
    let current_color_value = unsafe {
        input
            .current_color_value
            .cast::<crate::css::style_value::StyleValueData>()
            .as_ref()
    };
    let length = unsafe {
        input
            .length
            .cast::<crate::css::style_compute::FfiLengthResolutionContext>()
            .as_ref()
    };
    ColorResolutionInput {
        scheme: input.has_scheme.then_some(input.scheme),
        current_color: input.has_current_color.then_some(Rgba {
            r: input.current_color_rgba[0],
            g: input.current_color_rgba[1],
            b: input.current_color_rgba[2],
            a: input.current_color_rgba[3],
        }),
        current_color_value,
        length,
        channels: input.has_channels.then_some(channels),
    }
}

/// Resolves a style value to an sRGB color, or declines.
///
/// # Safety
/// `value` must point at live style value data and `input` at a valid resolution input whose
/// pointers outlive the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_to_color(
    value: *const core::ffi::c_void,
    input: *const FfiColorResolutionInput,
) -> FfiResolvedColorValue {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StyleValueQueryEntry);
    let value = unsafe { &*value.cast::<crate::css::style_value::StyleValueData>() };
    let input = unsafe { &*input };
    let channels = relative_color_context_from_ffi(input);
    // SAFETY: The caller warrants the input's pointers outlive the call.
    let resolution_input = unsafe { resolution_input_from_ffi(input, &channels) };
    match to_color(value, &resolution_input) {
        Some(color) => FfiResolvedColorValue {
            resolved: true,
            rgba: [color.r, color.g, color.b, color.a],
        },
        None => FfiResolvedColorValue {
            resolved: false,
            rgba: [0; 4],
        },
    }
}
