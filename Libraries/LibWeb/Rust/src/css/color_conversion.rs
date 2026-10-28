/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Color-space conversions used by CSS color interpolation.

pub(crate) type Components = [f32; 4];

// Colors are converted in double precision with the full-precision matrices of CSS Color 4's sample code, since the hue
// of a color with little chroma is very sensitive to rounding.
type Color = [f64; 4];
type Matrix = [[f64; 3]; 3];

pub(crate) const RGB: u8 = 0;
pub(crate) const A98_RGB: u8 = 1;
pub(crate) const DISPLAY_P3: u8 = 2;
pub(crate) const DISPLAY_P3_LINEAR: u8 = 3;
pub(crate) const HSL: u8 = 4;
pub(crate) const HWB: u8 = 5;
pub(crate) const LAB: u8 = 6;
pub(crate) const LCH: u8 = 7;
pub(crate) const OKLAB: u8 = 8;
pub(crate) const OKLCH: u8 = 9;
pub(crate) const SRGB: u8 = 10;
pub(crate) const SRGB_LINEAR: u8 = 11;
pub(crate) const PROPHOTO_RGB: u8 = 12;
pub(crate) const REC2020: u8 = 13;
pub(crate) const XYZ_D50: u8 = 14;
pub(crate) const XYZ_D65: u8 = 15;
// NB: alpha() has no color space of its own; its result is in the color space of its origin color.
pub(crate) const ALPHA: u8 = 16;

// https://drafts.csswg.org/css-color-4/#color-conversion-code
const LINEAR_SRGB_TO_XYZ65: Matrix = [
    [506752.0 / 1228815.0, 87881.0 / 245763.0, 12673.0 / 70218.0],
    [87098.0 / 409605.0, 175762.0 / 245763.0, 12673.0 / 175545.0],
    [7918.0 / 409605.0, 87881.0 / 737289.0, 1001167.0 / 1053270.0],
];
const XYZ65_TO_LINEAR_SRGB: Matrix = [
    [12831.0 / 3959.0, -329.0 / 214.0, -1974.0 / 3959.0],
    [-851781.0 / 878810.0, 1648619.0 / 878810.0, 36519.0 / 878810.0],
    [705.0 / 12673.0, -2585.0 / 12673.0, 705.0 / 667.0],
];
const LINEAR_DISPLAY_P3_TO_XYZ65: Matrix = [
    [608311.0 / 1250200.0, 189793.0 / 714400.0, 198249.0 / 1000160.0],
    [35783.0 / 156275.0, 247089.0 / 357200.0, 198249.0 / 2500400.0],
    [0.0, 32229.0 / 714400.0, 5220557.0 / 5000800.0],
];
const XYZ65_TO_LINEAR_DISPLAY_P3: Matrix = [
    [446124.0 / 178915.0, -333277.0 / 357830.0, -72051.0 / 178915.0],
    [-14852.0 / 17905.0, 63121.0 / 35810.0, 423.0 / 17905.0],
    [11844.0 / 330415.0, -50337.0 / 660830.0, 316169.0 / 330415.0],
];
const LINEAR_PROPHOTO_RGB_TO_XYZ50: Matrix = [
    [0.7977666449006423, 0.13518129740053308, 0.0313477341283922],
    [0.2880748288194013, 0.711835234241873, 0.00008993693872564],
    [0.0, 0.0, 0.8251046025104602],
];
const XYZ50_TO_LINEAR_PROPHOTO_RGB: Matrix = [
    [1.3457868816471583, -0.25557208737979464, -0.05110186497554526],
    [-0.5446307051249019, 1.5082477428451468, 0.02052744743642139],
    [0.0, 0.0, 1.2119675456389452],
];
const LINEAR_A98_RGB_TO_XYZ65: Matrix = [
    [573536.0 / 994567.0, 263643.0 / 1420810.0, 187206.0 / 994567.0],
    [591459.0 / 1989134.0, 6239551.0 / 9945670.0, 374412.0 / 4972835.0],
    [53769.0 / 1989134.0, 351524.0 / 4972835.0, 4929758.0 / 4972835.0],
];
const XYZ65_TO_LINEAR_A98_RGB: Matrix = [
    [1829569.0 / 896150.0, -506331.0 / 896150.0, -308931.0 / 896150.0],
    [-851781.0 / 878810.0, 1648619.0 / 878810.0, 36519.0 / 878810.0],
    [16779.0 / 1248040.0, -147721.0 / 1248040.0, 1266979.0 / 1248040.0],
];
const LINEAR_REC2020_TO_XYZ65: Matrix = [
    [
        63426534.0 / 99577255.0,
        20160776.0 / 139408157.0,
        47086771.0 / 278816314.0,
    ],
    [
        26158966.0 / 99577255.0,
        472592308.0 / 697040785.0,
        8267143.0 / 139408157.0,
    ],
    [0.0, 19567812.0 / 697040785.0, 295819943.0 / 278816314.0],
];
const XYZ65_TO_LINEAR_REC2020: Matrix = [
    [
        30757411.0 / 17917100.0,
        -6372589.0 / 17917100.0,
        -4539589.0 / 17917100.0,
    ],
    [-19765991.0 / 29648200.0, 47925759.0 / 29648200.0, 467509.0 / 29648200.0],
    [792561.0 / 44930125.0, -1921689.0 / 44930125.0, 42328811.0 / 44930125.0],
];
const XYZ65_TO_XYZ50: Matrix = [
    [1.0479297925449969, 0.022946870601609652, -0.05019226628920524],
    [0.02962780877005599, 0.9904344267538799, -0.017073799063418826],
    [-0.009243040646204504, 0.015055191490298152, 0.7518742814281371],
];
const XYZ50_TO_XYZ65: Matrix = [
    [0.955473421488075, -0.02309845494876471, 0.06325924320057072],
    [-0.0283697093338637, 1.0099953980813041, 0.021041441191917323],
    [0.012314014864481998, -0.020507649298898964, 1.330365926242124],
];
const XYZ65_TO_LMS: Matrix = [
    [0.819022437996703, 0.3619062600528904, -0.1288737815209879],
    [0.0329836539323885, 0.9292868615863434, 0.0361446663506424],
    [0.0481771893596242, 0.2642395317527308, 0.6335478284694309],
];
const LMS_TO_OKLAB: Matrix = [
    [0.210454268309314, 0.7936177747023054, -0.0040720430116193],
    [1.9779985324311684, -2.42859224204858, 0.450593709617411],
    [0.0259040424655478, 0.7827717124575296, -0.8086757549230774],
];
const OKLAB_TO_LMS: Matrix = [
    [1.0, 0.3963377773761749, 0.2158037573099136],
    [1.0, -0.1055613458156586, -0.0638541728258133],
    [1.0, -0.0894841775298119, -1.2914855480194092],
];
const LMS_TO_XYZ65: Matrix = [
    [1.2268798758459243, -0.5578149944602171, 0.2813910456659647],
    [-0.0405757452148008, 1.112286803280317, -0.0717110580655164],
    [-0.0763729366746601, -0.4214933324022432, 1.5869240198367816],
];

// https://drafts.csswg.org/css-color-4/#color-conversion-code
const D50_WHITE_X: f64 = 0.3457 / 0.3585;
const D50_WHITE_Z: f64 = (1.0 - 0.3457 - 0.3585) / 0.3585;
const LAB_KAPPA: f64 = 24389.0 / 27.0;
const LAB_EPSILON: f64 = 216.0 / 24389.0;

// AD-HOC: CSS Color 4 specifies a pure 2.4 gamma transfer function for rec2020, but all major engines and the WPT
//         rec2020 reftests use the piecewise OETF from ITU-R BT.2020-2, so we do the same.
const REC2020_ALPHA: f64 = 1.09929682680944;
const REC2020_BETA: f64 = 0.018053968510807;

fn widen(color: Components) -> Color {
    color.map(f64::from)
}

fn narrow(color: Color) -> Components {
    color.map(|component| component as f32)
}

fn multiply(matrix: &Matrix, color: Color) -> Color {
    let [x, y, z, alpha] = color;
    let row = |row: &[f64; 3]| row[0] * x + row[1] * y + row[2] * z;
    [row(&matrix[0]), row(&matrix[1]), row(&matrix[2]), alpha]
}

fn map_rgb(color: Color, convert: impl Fn(f64) -> f64) -> Color {
    [convert(color[0]), convert(color[1]), convert(color[2]), color[3]]
}

// https://drafts.csswg.org/css-color-4/#predefined-sRGB
// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn srgb_to_linear_srgb(srgb: Color) -> Color {
    map_rgb(srgb, |component| {
        let absolute = component.abs();
        if absolute <= 0.04045 {
            component / 12.92
        } else {
            component.signum() * ((absolute + 0.055) / 1.055).powf(2.4)
        }
    })
}

// https://drafts.csswg.org/css-color-4/#predefined-sRGB
// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn linear_srgb_to_srgb(linear: Color) -> Color {
    map_rgb(linear, |component| {
        let absolute = component.abs();
        if absolute > 0.0031308 {
            component.signum() * (1.055 * absolute.powf(1.0 / 2.4) - 0.055)
        } else {
            12.92 * component
        }
    })
}

// https://drafts.csswg.org/css-color-4/#predefined-a98-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn a98_rgb_to_linear_a98_rgb(color: Color) -> Color {
    map_rgb(color, |component| {
        component.signum() * component.abs().powf(563.0 / 256.0)
    })
}

fn linear_a98_rgb_to_a98_rgb(color: Color) -> Color {
    map_rgb(color, |component| {
        component.signum() * component.abs().powf(256.0 / 563.0)
    })
}

// https://drafts.csswg.org/css-color-4/#predefined-prophoto-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn prophoto_rgb_to_linear_prophoto_rgb(color: Color) -> Color {
    map_rgb(color, |component| {
        let absolute = component.abs();
        if absolute <= 16.0 / 512.0 {
            component / 16.0
        } else {
            component.signum() * absolute.powf(1.8)
        }
    })
}

fn linear_prophoto_rgb_to_prophoto_rgb(color: Color) -> Color {
    map_rgb(color, |component| {
        let absolute = component.abs();
        if absolute <= 1.0 / 512.0 {
            component * 16.0
        } else {
            component.signum() * absolute.powf(1.0 / 1.8)
        }
    })
}

// https://drafts.csswg.org/css-color-4/#predefined-rec2020
fn rec2020_to_linear_rec2020(color: Color) -> Color {
    map_rgb(color, |component| {
        let absolute = component.abs();
        if absolute < REC2020_BETA * 4.5 {
            component / 4.5
        } else {
            component.signum() * ((absolute + REC2020_ALPHA - 1.0) / REC2020_ALPHA).powf(1.0 / 0.45)
        }
    })
}

fn linear_rec2020_to_rec2020(color: Color) -> Color {
    map_rgb(color, |component| {
        let absolute = component.abs();
        if absolute < REC2020_BETA {
            4.5 * component
        } else {
            component.signum() * (REC2020_ALPHA * absolute.powf(0.45) - (REC2020_ALPHA - 1.0))
        }
    })
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn lab_to_xyz50(color: Color) -> Color {
    let [lightness, a, b, alpha] = color;
    let f1 = (lightness + 16.0) / 116.0;
    let f0 = a / 500.0 + f1;
    let f2 = f1 - b / 200.0;
    let compute = |f: f64| {
        let cubed = f * f * f;
        if cubed > LAB_EPSILON {
            cubed
        } else {
            (116.0 * f - 16.0) / LAB_KAPPA
        }
    };
    let y = if lightness > LAB_KAPPA * LAB_EPSILON {
        f1.powi(3)
    } else {
        lightness / LAB_KAPPA
    };
    [D50_WHITE_X * compute(f0), y, D50_WHITE_Z * compute(f2), alpha]
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn xyz50_to_lab(color: Color) -> Color {
    let [x, y, z, alpha] = color;
    let convert = |value: f64| {
        if value > LAB_EPSILON {
            value.cbrt()
        } else {
            (LAB_KAPPA * value + 16.0) / 116.0
        }
    };
    let fx = convert(x / D50_WHITE_X);
    let fy = convert(y);
    let fz = convert(z / D50_WHITE_Z);
    [116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz), alpha]
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn oklab_to_xyz65(color: Color) -> Color {
    let [long, medium, short, alpha] = multiply(&OKLAB_TO_LMS, color);
    multiply(&LMS_TO_XYZ65, [long.powi(3), medium.powi(3), short.powi(3), alpha])
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn xyz65_to_oklab(color: Color) -> Color {
    let [long, medium, short, alpha] = multiply(&XYZ65_TO_LMS, color);
    multiply(&LMS_TO_OKLAB, [long.cbrt(), medium.cbrt(), short.cbrt(), alpha])
}

fn polar_to_rectangular(color: Color) -> Color {
    let radians = color[2].to_radians();
    [color[0], color[1] * radians.cos(), color[1] * radians.sin(), color[3]]
}

fn rectangular_to_polar(color: Color) -> Color {
    let chroma = color[1].hypot(color[2]);
    let mut hue = color[2].atan2(color[1]).to_degrees();
    if hue < 0.0 {
        hue += 360.0;
    }
    [color[0], chroma, hue, color[3]]
}

// Algorithm from https://drafts.csswg.org/css-color-3/#hsl-color
fn hsl_color_to_srgb(color: Color) -> Color {
    let mut hue = color[0] % 360.0;
    if hue < 0.0 {
        hue += 360.0;
    }
    let convert = |offset: f64| {
        let k = (offset + hue / 30.0) % 12.0;
        let a = color[1] * color[2].min(1.0 - color[2]);
        color[2] - a * (-1.0_f64).max((k - 3.0).min(9.0 - k).min(1.0))
    };
    [convert(0.0), convert(8.0), convert(4.0), color[3].clamp(0.0, 1.0)]
}

// https://drafts.csswg.org/css-color-4/#hwb-to-rgb
fn hwb_color_to_srgb(color: Color) -> Color {
    if color[1] + color[2] >= 1.0 {
        let gray = color[1] / (color[1] + color[2]);
        return [gray, gray, gray, color[3]];
    }
    let rgb = hsl_color_to_srgb([color[0], 1.0, 0.5, color[3]]);
    let scale = 1.0 - color[1] - color[2];
    [
        rgb[0] * scale + color[1],
        rgb[1] * scale + color[1],
        rgb[2] * scale + color[1],
        color[3],
    ]
}

pub(crate) fn hsl_to_srgb(color: Components) -> Components {
    narrow(hsl_color_to_srgb(widen(color)))
}

pub(crate) fn hwb_to_srgb(color: Components) -> Components {
    narrow(hwb_color_to_srgb(widen(color)))
}

// https://drafts.csswg.org/css-color-4/#rgb-to-hsl
fn srgb_to_hsl(color: Color) -> Color {
    let [red, green, blue, alpha] = color;
    let maximum = red.max(green).max(blue);
    let minimum = red.min(green).min(blue);
    let chroma = maximum - minimum;
    let lightness = (minimum + maximum) / 2.0;
    let mut hue = 0.0;
    let mut saturation = 0.0;
    let epsilon = 1.0 / 100000.0;

    if chroma != 0.0 {
        if lightness.abs() > epsilon && (1.0 - lightness).abs() >= epsilon {
            saturation = (maximum - lightness) / lightness.min(1.0 - lightness);
        }
        if maximum == red {
            hue = (green - blue) / chroma + if green < blue { 6.0 } else { 0.0 };
        } else if maximum == green {
            hue = (blue - red) / chroma + 2.0;
        } else {
            hue = (red - green) / chroma + 4.0;
        }
        hue *= 60.0;
        if saturation < 0.0 {
            hue += 180.0;
            saturation = saturation.abs();
        }
        if hue >= 360.0 {
            hue -= 360.0;
        }
        if saturation <= epsilon {
            hue = f64::NAN;
            saturation = 0.0;
        }
    }
    [hue, saturation, lightness, alpha]
}

// https://drafts.csswg.org/css-color-4/#rgb-to-hwb
fn srgb_to_hwb(color: Color) -> Color {
    let [red, green, blue, alpha] = color;
    let maximum = red.max(green).max(blue);
    let minimum = red.min(green).min(blue);
    let chroma = maximum - minimum;
    let mut hue = 0.0;
    if chroma != 0.0 {
        if maximum == red {
            hue = (green - blue) / chroma + if green < blue { 6.0 } else { 0.0 };
        } else if maximum == green {
            hue = (blue - red) / chroma + 2.0;
        } else {
            hue = (red - green) / chroma + 4.0;
        }
        hue *= 60.0;
        if hue >= 360.0 {
            hue -= 360.0;
        }
    }
    [hue, minimum, 1.0 - maximum, alpha]
}

fn legacy_color_to_srgb_color(color_type: u8, color: Color) -> Option<Color> {
    match color_type {
        RGB => Some(color),
        HSL => Some(hsl_color_to_srgb(color)),
        HWB => Some(hwb_color_to_srgb(color)),
        _ => None,
    }
}

pub(crate) fn legacy_color_to_srgb(color_type: u8, color: Components) -> Option<Components> {
    legacy_color_to_srgb_color(color_type, widen(color)).map(narrow)
}

fn to_xyz65(color_type: u8, mut color: Color) -> Option<Color> {
    match color_type {
        RGB => {
            color[0] = color[0].clamp(0.0, 1.0);
            color[1] = color[1].clamp(0.0, 1.0);
            color[2] = color[2].clamp(0.0, 1.0);
            Some(multiply(&LINEAR_SRGB_TO_XYZ65, srgb_to_linear_srgb(color)))
        }
        SRGB => Some(multiply(&LINEAR_SRGB_TO_XYZ65, srgb_to_linear_srgb(color))),
        SRGB_LINEAR => Some(multiply(&LINEAR_SRGB_TO_XYZ65, color)),
        DISPLAY_P3 => Some(multiply(&LINEAR_DISPLAY_P3_TO_XYZ65, srgb_to_linear_srgb(color))),
        DISPLAY_P3_LINEAR => Some(multiply(&LINEAR_DISPLAY_P3_TO_XYZ65, color)),
        A98_RGB => Some(multiply(&LINEAR_A98_RGB_TO_XYZ65, a98_rgb_to_linear_a98_rgb(color))),
        PROPHOTO_RGB => Some(multiply(
            &XYZ50_TO_XYZ65,
            multiply(
                &LINEAR_PROPHOTO_RGB_TO_XYZ50,
                prophoto_rgb_to_linear_prophoto_rgb(color),
            ),
        )),
        REC2020 => Some(multiply(&LINEAR_REC2020_TO_XYZ65, rec2020_to_linear_rec2020(color))),
        XYZ_D50 => Some(multiply(&XYZ50_TO_XYZ65, color)),
        XYZ_D65 => Some(color),
        LAB => Some(multiply(&XYZ50_TO_XYZ65, lab_to_xyz50(color))),
        LCH => Some(multiply(&XYZ50_TO_XYZ65, lab_to_xyz50(polar_to_rectangular(color)))),
        OKLAB => Some(oklab_to_xyz65(color)),
        OKLCH => Some(oklab_to_xyz65(polar_to_rectangular(color))),
        HSL => Some(multiply(
            &LINEAR_SRGB_TO_XYZ65,
            srgb_to_linear_srgb(hsl_color_to_srgb(color)),
        )),
        HWB => Some(multiply(
            &LINEAR_SRGB_TO_XYZ65,
            srgb_to_linear_srgb(hwb_color_to_srgb(color)),
        )),
        _ => None,
    }
}

fn from_xyz65(color_type: u8, color: Color) -> Option<Color> {
    let srgb = || linear_srgb_to_srgb(multiply(&XYZ65_TO_LINEAR_SRGB, color));
    match color_type {
        RGB | SRGB => Some(srgb()),
        SRGB_LINEAR => Some(multiply(&XYZ65_TO_LINEAR_SRGB, color)),
        DISPLAY_P3 => Some(linear_srgb_to_srgb(multiply(&XYZ65_TO_LINEAR_DISPLAY_P3, color))),
        DISPLAY_P3_LINEAR => Some(multiply(&XYZ65_TO_LINEAR_DISPLAY_P3, color)),
        A98_RGB => Some(linear_a98_rgb_to_a98_rgb(multiply(&XYZ65_TO_LINEAR_A98_RGB, color))),
        PROPHOTO_RGB => Some(linear_prophoto_rgb_to_prophoto_rgb(multiply(
            &XYZ50_TO_LINEAR_PROPHOTO_RGB,
            multiply(&XYZ65_TO_XYZ50, color),
        ))),
        REC2020 => Some(linear_rec2020_to_rec2020(multiply(&XYZ65_TO_LINEAR_REC2020, color))),
        XYZ_D50 => Some(multiply(&XYZ65_TO_XYZ50, color)),
        XYZ_D65 => Some(color),
        LAB => Some(xyz50_to_lab(multiply(&XYZ65_TO_XYZ50, color))),
        LCH => Some(rectangular_to_polar(xyz50_to_lab(multiply(&XYZ65_TO_XYZ50, color)))),
        OKLAB => Some(xyz65_to_oklab(color)),
        OKLCH => Some(rectangular_to_polar(xyz65_to_oklab(color))),
        HSL => Some(srgb_to_hsl(srgb())),
        HWB => Some(srgb_to_hwb(srgb())),
        _ => None,
    }
}

fn convert_color(color_type: u8, target_type: u8, color: Color) -> Option<Color> {
    if color_type == target_type || color_type == RGB && target_type == SRGB {
        return Some(color);
    }
    if target_type == HSL || target_type == HWB {
        let srgb = if color_type == RGB || color_type == SRGB {
            color
        } else {
            from_xyz65(SRGB, to_xyz65(color_type, color)?)?
        };
        return Some(if target_type == HSL {
            srgb_to_hsl(srgb)
        } else {
            srgb_to_hwb(srgb)
        });
    }
    if (color_type == HSL || color_type == HWB) && target_type == SRGB {
        return legacy_color_to_srgb_color(color_type, color);
    }
    from_xyz65(target_type, to_xyz65(color_type, color)?)
}

pub(crate) fn convert(color_type: u8, target_type: u8, color: Components) -> Option<Components> {
    convert_color(color_type, target_type, widen(color)).map(narrow)
}

pub(crate) fn to_oklab(color_type: u8, color: Components) -> Option<Components> {
    convert(color_type, OKLAB, color)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn converts_srgb_endpoints_to_oklab() {
        assert_eq!(to_oklab(SRGB, [0.0, 0.0, 0.0, 1.0]), Some([0.0, 0.0, 0.0, 1.0]));
        let white = to_oklab(SRGB, [1.0, 1.0, 1.0, 1.0]).unwrap();
        assert!((white[0] - 1.0).abs() < 0.00001);
        assert!(white[1].abs() < 0.00001);
        assert!(white[2].abs() < 0.00001);
    }

    #[test]
    fn round_trips_supported_color_spaces() {
        let source = [0.25, 0.5, 0.75, 0.8];
        for color_type in [
            SRGB,
            SRGB_LINEAR,
            DISPLAY_P3,
            DISPLAY_P3_LINEAR,
            A98_RGB,
            PROPHOTO_RGB,
            REC2020,
            XYZ_D50,
            XYZ_D65,
            LAB,
            LCH,
            OKLAB,
            OKLCH,
            HSL,
            HWB,
        ] {
            let converted = convert(SRGB, color_type, source).unwrap();
            let result = convert(color_type, SRGB, converted).unwrap();
            for index in 0..4 {
                assert!(
                    (result[index] - source[index]).abs() < 0.0001,
                    "color type {color_type}, component {index}"
                );
            }
        }
    }
}
