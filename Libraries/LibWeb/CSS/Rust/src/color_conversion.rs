/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Color-space conversions used by CSS color interpolation.

pub(crate) type Components = [f32; 4];

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

// https://drafts.csswg.org/css-color-4/#predefined-sRGB
// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn srgb_to_linear_srgb(srgb: Components) -> Components {
    let convert = |component: f32| {
        let sign = if component < 0.0 { -1.0 } else { 1.0 };
        let absolute = component.abs();
        if absolute <= 0.04045 {
            component / 12.92
        } else {
            sign * ((absolute + 0.055) / 1.055).powf(2.4)
        }
    };
    [convert(srgb[0]), convert(srgb[1]), convert(srgb[2]), srgb[3]]
}

// https://drafts.csswg.org/css-color-4/#predefined-sRGB
// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn linear_srgb_to_srgb(linear: Components) -> Components {
    let convert = |component: f32| {
        let sign = if component < 0.0 { -1.0 } else { 1.0 };
        let absolute = component.abs();
        if absolute > 0.0031308 {
            sign * (1.055 * absolute.powf(1.0 / 2.4) - 0.055)
        } else {
            12.92 * component
        }
    };
    [convert(linear[0]), convert(linear[1]), convert(linear[2]), linear[3]]
}

// https://drafts.csswg.org/css-color-4/#predefined-sRGB-linear
// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn linear_srgb_to_xyz65(color: Components) -> Components {
    let [red, green, blue, alpha] = color;
    [
        0.4123908 * red + 0.35758433 * green + 0.1804808 * blue,
        0.212639 * red + 0.71516865 * green + 0.07219232 * blue,
        0.01933082 * red + 0.11919478 * green + 0.95053214 * blue,
        alpha,
    ]
}

// https://drafts.csswg.org/css-color-4/#predefined-display-p3
// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn display_p3_to_xyz65(color: Components, is_linear: bool) -> Components {
    let convert = |component: f32| {
        if is_linear {
            return component;
        }
        let sign = if component < 0.0 { -1.0 } else { 1.0 };
        let absolute = component.abs();
        if absolute <= 0.04045 {
            component / 12.92
        } else {
            sign * ((absolute + 0.055) / 1.055).powf(2.4)
        }
    };
    let red = convert(color[0]);
    let green = convert(color[1]);
    let blue = convert(color[2]);
    [
        0.48657095 * red + 0.2656677 * green + 0.19821729 * blue,
        0.22897457 * red + 0.69173855 * green + 0.07928691 * blue,
        0.04511338 * green + 1.0439444 * blue,
        color[3],
    ]
}

// https://drafts.csswg.org/css-color-4/#predefined-a98-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn a98_rgb_to_xyz65(color: Components) -> Components {
    let convert = |component: f32| component.signum() * component.abs().powf(563.0 / 256.0);
    let red = convert(color[0]);
    let green = convert(color[1]);
    let blue = convert(color[2]);
    [
        0.57666904 * red + 0.18555824 * green + 0.18822865 * blue,
        0.29734498 * red + 0.62736356 * green + 0.07529146 * blue,
        0.02703136 * red + 0.07068885 * green + 0.99133754 * blue,
        color[3],
    ]
}

// https://drafts.csswg.org/css-color-4/#predefined-prophoto-rgb
// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn prophoto_rgb_to_xyz50(color: Components) -> Components {
    let convert = |component: f32| {
        let absolute = component.abs();
        if absolute <= 16.0 / 512.0 {
            component / 16.0
        } else {
            component.signum() * absolute.powf(1.8)
        }
    };
    let red = convert(color[0]);
    let green = convert(color[1]);
    let blue = convert(color[2]);
    [
        0.7977666 * red + 0.1351813 * green + 0.03134773 * blue,
        0.28807482 * red + 0.7118352 * green + 0.00008994 * blue,
        0.8251046 * blue,
        color[3],
    ]
}

// https://drafts.csswg.org/css-color-4/#predefined-rec2020
fn rec2020_to_xyz65(color: Components) -> Components {
    let convert = |component: f32| {
        // AD-HOC: CSS Color 4 specifies a pure 2.4 gamma transfer function for rec2020, but all major engines
        //         and the WPT rec2020 reftests use the piecewise OETF from ITU-R BT.2020-2, so we do the same.
        const ALPHA: f32 = 1.0992968;
        const BETA: f32 = 0.01805397;
        let absolute = component.abs();
        if absolute < BETA * 4.5 {
            component / 4.5
        } else {
            component.signum() * ((absolute + ALPHA - 1.0) / ALPHA).powf(1.0 / 0.45)
        }
    };
    let red = convert(color[0]);
    let green = convert(color[1]);
    let blue = convert(color[2]);
    [
        0.63695806 * red + 0.1446169 * green + 0.16888098 * blue,
        0.2627002 * red + 0.67799807 * green + 0.05930172 * blue,
        0.02807269 * green + 1.0609851 * blue,
        color[3],
    ]
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn xyz50_to_xyz65(color: Components) -> Components {
    let [x, y, z, alpha] = color;
    [
        0.9554735 * x - 0.02309854 * y + 0.06325931 * z,
        -0.02836971 * x + 1.0099955 * y + 0.02104154 * z,
        0.012314 * x - 0.0205077 * y + 1.3303659 * z,
        alpha,
    ]
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn xyz65_to_xyz50(color: Components) -> Components {
    let [x, y, z, alpha] = color;
    [
        1.0479298 * x + 0.022946874 * y - 0.05019223 * z,
        0.029627815 * x + 0.99043447 * y - 0.017073825 * z,
        -0.009243058 * x + 0.015055145 * y + 0.75187427 * z,
        alpha,
    ]
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn lab_to_xyz50(color: Components) -> Components {
    const KAPPA: f32 = 24389.0 / 27.0;
    const EPSILON: f32 = 216.0 / 24389.0;
    let [lightness, a, b, alpha] = color;
    let f1 = (lightness + 16.0) / 116.0;
    let f0 = a / 500.0 + f1;
    let f2 = f1 - b / 200.0;
    let compute = |f: f32| {
        let cubed = f * f * f;
        if cubed > EPSILON {
            cubed
        } else {
            (116.0 * f - 16.0) / KAPPA
        }
    };
    let y = if lightness > KAPPA * EPSILON {
        ((lightness + 16.0) / 116.0).powi(3)
    } else {
        lightness / KAPPA
    };
    let x_n = 0.3457 / 0.3585;
    let z_n = (1.0 - 0.3457 - 0.3585) / 0.3585;
    [x_n * compute(f0), y, z_n * compute(f2), alpha]
}

fn polar_to_rectangular(color: Components) -> Components {
    let radians = color[2].to_radians();
    [color[0], color[1] * radians.cos(), color[1] * radians.sin(), color[3]]
}

fn rectangular_to_polar(color: Components) -> Components {
    let chroma = color[1].hypot(color[2]);
    let mut hue = color[2].atan2(color[1]).to_degrees();
    if hue < 0.0 {
        hue += 360.0;
    }
    [color[0], chroma, hue, color[3]]
}

// Algorithm from https://drafts.csswg.org/css-color-3/#hsl-color
fn hsl_to_srgb(color: Components) -> Components {
    let mut hue = color[0] % 360.0;
    if hue < 0.0 {
        hue += 360.0;
    }
    let convert = |offset: f32| {
        let k = (offset + hue / 30.0) % 12.0;
        let a = color[1] * color[2].min(1.0 - color[2]);
        color[2] - a * (-1.0_f32).max((k - 3.0).min(9.0 - k).min(1.0))
    };
    [convert(0.0), convert(8.0), convert(4.0), color[3].clamp(0.0, 1.0)]
}

// https://drafts.csswg.org/css-color-4/#hwb-to-rgb
fn hwb_to_srgb(color: Components) -> Components {
    if color[1] + color[2] >= 1.0 {
        let gray = color[1] / (color[1] + color[2]);
        return [gray, gray, gray, color[3]];
    }
    let rgb = hsl_to_srgb([color[0], 1.0, 0.5, color[3]]);
    let scale = 1.0 - color[1] - color[2];
    [
        rgb[0] * scale + color[1],
        rgb[1] * scale + color[1],
        rgb[2] * scale + color[1],
        color[3],
    ]
}

// https://drafts.csswg.org/css-color-4/#rgb-to-hsl
fn srgb_to_hsl(color: Components) -> Components {
    let [red, green, blue, alpha] = color;
    let maximum = red.max(green).max(blue);
    let minimum = red.min(green).min(blue);
    let chroma = maximum - minimum;
    let lightness = (minimum + maximum) / 2.0;
    let mut hue = 0.0;
    let mut saturation = 0.0;

    if chroma != 0.0 {
        if lightness != 0.0 && lightness != 1.0 {
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
    }
    [hue, saturation, lightness, alpha]
}

// https://drafts.csswg.org/css-color-4/#rgb-to-hwb
fn srgb_to_hwb(color: Components) -> Components {
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

fn xyz65_to_linear_srgb(color: Components) -> Components {
    let [x, y, z, alpha] = color;
    [
        3.240_97 * x - 1.537383 * y - 0.498611 * z,
        -0.969244 * x + 1.875968 * y + 0.041555 * z,
        0.055630 * x - 0.203977 * y + 1.056972 * z,
        alpha,
    ]
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn oklab_to_xyz65(color: Components) -> Components {
    let [lightness, a, b, alpha] = color;
    let long = (lightness + 0.39633778 * a + 0.21580376 * b).powi(3);
    let medium = (lightness - 0.105561346 * a - 0.06385417 * b).powi(3);
    let short = (lightness - 0.08948418 * a - 1.2914855 * b).powi(3);
    [
        1.2268798 * long - 0.557815 * medium + 0.28139105 * short,
        -0.040575746 * long + 1.1122868 * medium - 0.071711056 * short,
        -0.07637294 * long - 0.42149332 * medium + 1.586924 * short,
        alpha,
    ]
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
fn xyz50_to_lab(color: Components) -> Components {
    const KAPPA: f32 = 24389.0 / 27.0;
    const EPSILON: f32 = 216.0 / 24389.0;
    let [x, y, z, alpha] = color;
    let x_n = 0.3457 / 0.3585;
    let z_n = (1.0 - 0.3457 - 0.3585) / 0.3585;
    let convert = |value: f32| {
        if value > EPSILON {
            value.cbrt()
        } else {
            (KAPPA * value + 16.0) / 116.0
        }
    };
    let fx = convert(x / x_n);
    let fy = convert(y);
    let fz = convert(z / z_n);
    [116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz), alpha]
}

fn xyz65_to_linear_display_p3(color: Components) -> Components {
    let [x, y, z, alpha] = color;
    [
        2.493497 * x - 0.9313836 * y - 0.4027108 * z,
        -0.829489 * x + 1.7626641 * y + 0.023624686 * z,
        0.03584583 * x - 0.07617239 * y + 0.9568845 * z,
        alpha,
    ]
}

fn xyz65_to_linear_a98_rgb(color: Components) -> Components {
    let [x, y, z, alpha] = color;
    [
        (1829569.0 / 896150.0) * x - (506331.0 / 896150.0) * y - (308931.0 / 896150.0) * z,
        -(851781.0 / 878810.0) * x + (1648619.0 / 878810.0) * y + (36519.0 / 878810.0) * z,
        (16779.0 / 1248040.0) * x - (147721.0 / 1248040.0) * y + (1266979.0 / 1248040.0) * z,
        alpha,
    ]
}

fn xyz50_to_linear_prophoto_rgb(color: Components) -> Components {
    let [x, y, z, alpha] = color;
    [
        1.345799 * x - 0.25558022 * y - 0.051118854 * z,
        -0.5446225 * x + 1.5082327 * y + 0.020527447 * z,
        1.2119676 * z,
        alpha,
    ]
}

fn xyz65_to_linear_rec2020(color: Components) -> Components {
    let [x, y, z, alpha] = color;
    [
        1.7166512 * x - 0.35567078 * y - 0.2533663 * z,
        -0.6666843 * x + 1.6164812 * y + 0.015768545 * z,
        0.017639857 * x - 0.042770613 * y + 0.94210315 * z,
        alpha,
    ]
}

fn linear_a98_rgb_to_a98_rgb(color: Components) -> Components {
    let convert = |component: f32| component.signum() * component.abs().powf(256.0 / 563.0);
    [convert(color[0]), convert(color[1]), convert(color[2]), color[3]]
}

fn linear_prophoto_rgb_to_prophoto_rgb(color: Components) -> Components {
    let convert = |component: f32| {
        if component.abs() <= 1.0 / 512.0 {
            component * 16.0
        } else {
            component.signum() * component.abs().powf(1.0 / 1.8)
        }
    };
    [convert(color[0]), convert(color[1]), convert(color[2]), color[3]]
}

fn linear_rec2020_to_rec2020(color: Components) -> Components {
    let convert = |component: f32| {
        const ALPHA: f32 = 1.0992968;
        const BETA: f32 = 0.01805397;
        if component.abs() < BETA {
            4.5 * component
        } else {
            component.signum() * (ALPHA * component.abs().powf(0.45) - (ALPHA - 1.0))
        }
    };
    [convert(color[0]), convert(color[1]), convert(color[2]), color[3]]
}

pub(crate) fn legacy_color_to_srgb(color_type: u8, color: Components) -> Option<Components> {
    match color_type {
        RGB => Some(color),
        HSL => Some(hsl_to_srgb(color)),
        HWB => Some(hwb_to_srgb(color)),
        _ => None,
    }
}

// https://drafts.csswg.org/css-color-4/#color-conversion-code
pub(crate) fn xyz65_to_oklab(color: Components) -> Components {
    let [x, y, z, alpha] = color;
    let long = (0.8190224 * x + 0.36190626 * y - 0.12887378 * z).cbrt();
    let medium = (0.03298365 * x + 0.92928684 * y + 0.03614467 * z).cbrt();
    let short = (0.04817719 * x + 0.26423952 * y + 0.63354784 * z).cbrt();
    [
        0.21045427 * long + 0.7936178 * medium - 0.00407204 * short,
        1.9779985 * long - 2.4285922 * medium + 0.4505937 * short,
        0.02590404 * long + 0.7827717 * medium - 0.80867577 * short,
        alpha,
    ]
}

fn to_xyz65(color_type: u8, mut color: Components) -> Option<Components> {
    match color_type {
        RGB => {
            color[0] = color[0].clamp(0.0, 1.0);
            color[1] = color[1].clamp(0.0, 1.0);
            color[2] = color[2].clamp(0.0, 1.0);
            Some(linear_srgb_to_xyz65(srgb_to_linear_srgb(color)))
        }
        SRGB => Some(linear_srgb_to_xyz65(srgb_to_linear_srgb(color))),
        SRGB_LINEAR => Some(linear_srgb_to_xyz65(color)),
        DISPLAY_P3 => Some(display_p3_to_xyz65(color, false)),
        DISPLAY_P3_LINEAR => Some(display_p3_to_xyz65(color, true)),
        A98_RGB => Some(a98_rgb_to_xyz65(color)),
        PROPHOTO_RGB => Some(xyz50_to_xyz65(prophoto_rgb_to_xyz50(color))),
        REC2020 => Some(rec2020_to_xyz65(color)),
        XYZ_D50 => Some(xyz50_to_xyz65(color)),
        XYZ_D65 => Some(color),
        LAB => Some(xyz50_to_xyz65(lab_to_xyz50(color))),
        LCH => Some(xyz50_to_xyz65(lab_to_xyz50(polar_to_rectangular(color)))),
        OKLAB => Some(oklab_to_xyz65(color)),
        OKLCH => Some(oklab_to_xyz65(polar_to_rectangular(color))),
        HSL => Some(linear_srgb_to_xyz65(srgb_to_linear_srgb(hsl_to_srgb(color)))),
        HWB => Some(linear_srgb_to_xyz65(srgb_to_linear_srgb(hwb_to_srgb(color)))),
        _ => None,
    }
}

fn from_xyz65(color_type: u8, color: Components) -> Option<Components> {
    let srgb = || linear_srgb_to_srgb(xyz65_to_linear_srgb(color));
    match color_type {
        RGB | SRGB => Some(srgb()),
        SRGB_LINEAR => Some(xyz65_to_linear_srgb(color)),
        DISPLAY_P3 => Some(linear_srgb_to_srgb(xyz65_to_linear_display_p3(color))),
        DISPLAY_P3_LINEAR => Some(xyz65_to_linear_display_p3(color)),
        A98_RGB => Some(linear_a98_rgb_to_a98_rgb(xyz65_to_linear_a98_rgb(color))),
        PROPHOTO_RGB => Some(linear_prophoto_rgb_to_prophoto_rgb(xyz50_to_linear_prophoto_rgb(
            xyz65_to_xyz50(color),
        ))),
        REC2020 => Some(linear_rec2020_to_rec2020(xyz65_to_linear_rec2020(color))),
        XYZ_D50 => Some(xyz65_to_xyz50(color)),
        XYZ_D65 => Some(color),
        LAB => Some(xyz50_to_lab(xyz65_to_xyz50(color))),
        LCH => Some(rectangular_to_polar(xyz50_to_lab(xyz65_to_xyz50(color)))),
        OKLAB => Some(xyz65_to_oklab(color)),
        OKLCH => Some(rectangular_to_polar(xyz65_to_oklab(color))),
        HSL => Some(srgb_to_hsl(srgb())),
        HWB => Some(srgb_to_hwb(srgb())),
        _ => None,
    }
}

pub(crate) fn convert(color_type: u8, target_type: u8, color: Components) -> Option<Components> {
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
        return legacy_color_to_srgb(color_type, color);
    }
    from_xyz65(target_type, to_xyz65(color_type, color)?)
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
