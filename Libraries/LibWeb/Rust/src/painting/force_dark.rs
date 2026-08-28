/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Force-dark: a paint-time color filter for pages that offer no dark theme of their own. A classifier decides
//! whether a color inverts; a filter decides how. Chrome's decision logic, with the inversion in Oklab, not CIE-Lab.

use libgfx_rust::Color;
use std::collections::{HashMap, HashSet};

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ForceDarkSettings {
    /// Foreground colors dimmer than this invert. 255 inverts every foreground, 0 inverts none.
    pub foreground_brightness_threshold: i32,
    /// Background colors brighter than this invert. 0 inverts every background, 255 inverts none.
    pub background_brightness_threshold: i32,
}

impl Default for ForceDarkSettings {
    // Chrome's shipped thresholds (dark_mode_settings_builder.cc), which leave already-dark regions of a page alone
    // instead of flipping them back to light.
    fn default() -> Self {
        Self {
            foreground_brightness_threshold: 150,
            background_brightness_threshold: 205,
        }
    }
}

/// What a color is being used for; None marks a draw force-dark must leave alone. The roles map onto the two
/// classifiers the way Chrome's DarkModeFilter::ShouldApplyToColor maps them.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum ForceDarkRole {
    #[default]
    None,
    Foreground,
    ListSymbol,
    Background,
    Svg,
    Border,
    Selection,
}

impl ForceDarkRole {
    fn classifies_as_background(self) -> bool {
        matches!(self, ForceDarkRole::Background | ForceDarkRole::Selection)
    }
}

/// Perceived brightness, ITU-R BT.601 luma over the 0..=255 channels.
pub fn brightness(color: Color) -> i32 {
    let weighted = color.red() as i32 * 299 + color.green() as i32 * 587 + color.blue() as i32 * 114;
    weighted / 1000
}

/// The two thresholds read in opposite directions, and so do their degenerate ends: a foreground threshold of 255
/// inverts everything, while a background threshold of 255 inverts nothing.
pub fn should_invert(color: Color, role: ForceDarkRole, settings: &ForceDarkSettings) -> bool {
    if role == ForceDarkRole::None {
        return false;
    }
    if role.classifies_as_background() {
        let threshold = settings.background_brightness_threshold;
        if threshold >= 255 {
            return false;
        }
        if threshold <= 0 {
            return true;
        }
        return brightness(color) > threshold;
    }

    let threshold = settings.foreground_brightness_threshold;
    if threshold >= 255 {
        return true;
    }
    if threshold <= 0 {
        return false;
    }
    brightness(color) < threshold
}

// Grays landing between these two snap down to the lower one, so the near-blacks an inverted page produces come out
// as one dark surface color instead of a spread of almost-identical values.
const GRAY_BRIGHTNESS_THRESHOLD: f32 = 32.0 / 255.0;
const GRAY_ADJUSTED_BRIGHTNESS: f32 = 18.0 / 255.0;

// Lightness inversion is L -> LIGHTNESS_LIFT - L, clamped to 1.0. The value is twice the Oklab lightness of sRGB
// mid-gray, which makes mid-gray invert to itself and lands white inside the gray band above.
const LIGHTNESS_LIFT: f32 = 1.20;

// The contrast floor selection colors are raised to, against the dark surface color; Chrome's
// color_utils::kMinimumVisibleContrastRatio.
const MINIMUM_VISIBLE_CONTRAST_RATIO: f64 = 3.0;
const DARK_SURFACE: Color = Color::from_rgb(18, 18, 18);

fn srgb_channel_to_linear(c: f32) -> f32 {
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

fn linear_channel_to_srgb(c: f32) -> f32 {
    if c <= 0.0031308 {
        c * 12.92
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    }
}

pub fn srgb_to_oklab(r: f32, g: f32, b: f32) -> (f32, f32, f32) {
    let r = srgb_channel_to_linear(r);
    let g = srgb_channel_to_linear(g);
    let b = srgb_channel_to_linear(b);

    let l = (0.412_221_46 * r + 0.536_332_55 * g + 0.051_445_995 * b).cbrt();
    let m = (0.211_903_5 * r + 0.680_699_5 * g + 0.107_396_96 * b).cbrt();
    let s = (0.088_302_46 * r + 0.281_718_85 * g + 0.629_978_7 * b).cbrt();

    (
        0.210_454_26 * l + 0.793_617_8 * m - 0.004_072_047 * s,
        1.977_998_5 * l - 2.428_592_2 * m + 0.450_593_7 * s,
        0.025_904_037 * l + 0.782_771_77 * m - 0.808_675_77 * s,
    )
}

pub fn oklab_to_srgb(l: f32, a: f32, b: f32) -> (f32, f32, f32) {
    let l_ = l + 0.396_337_78 * a + 0.215_803_76 * b;
    let m_ = l - 0.105_561_346 * a - 0.063_854_17 * b;
    let s_ = l - 0.089_484_18 * a - 1.291_485_5 * b;

    let l_cubed = l_ * l_ * l_;
    let m_cubed = m_ * m_ * m_;
    let s_cubed = s_ * s_ * s_;

    let r = 4.076_741_7 * l_cubed - 3.307_711_6 * m_cubed + 0.230_969_94 * s_cubed;
    let g = -1.268_438 * l_cubed + 2.609_757_4 * m_cubed - 0.341_319_38 * s_cubed;
    let b = -0.0041960863 * l_cubed - 0.703_418_6 * m_cubed + 1.707_614_7 * s_cubed;

    (
        linear_channel_to_srgb(r).clamp(0.0, 1.0),
        linear_channel_to_srgb(g).clamp(0.0, 1.0),
        linear_channel_to_srgb(b).clamp(0.0, 1.0),
    )
}

/// Snap near-black grays to the dark surface color. Operates in sRGB channel space, not Oklab.
pub fn adjust_gray(color: Color) -> Color {
    let r = color.red() as f32 / 255.0;
    let g = color.green() as f32 / 255.0;
    let b = color.blue() as f32 / 255.0;

    const EPSILON: f32 = 1.0 / 255.0;
    let is_gray = (r - g).abs() <= EPSILON && (r - b).abs() <= EPSILON;
    if is_gray && r < GRAY_BRIGHTNESS_THRESHOLD && r > GRAY_ADJUSTED_BRIGHTNESS {
        let level = (GRAY_ADJUSTED_BRIGHTNESS * 255.0).round() as u8;
        return Color::from_rgba(level, level, level, color.alpha());
    }
    color
}

fn with_lightness(color: Color, lightness: f32) -> Color {
    let (_, a, b) = srgb_to_oklab(
        color.red() as f32 / 255.0,
        color.green() as f32 / 255.0,
        color.blue() as f32 / 255.0,
    );
    let (r, g, bl) = oklab_to_srgb(lightness, a, b);
    Color::from_rgba(
        (r * 255.0).round() as u8,
        (g * 255.0).round() as u8,
        (bl * 255.0).round() as u8,
        color.alpha(),
    )
}

/// Invert a color's lightness in Oklab, preserving hue, chroma and alpha.
pub fn invert_lightness(color: Color) -> Color {
    let (l, _, _) = srgb_to_oklab(
        color.red() as f32 / 255.0,
        color.green() as f32 / 255.0,
        color.blue() as f32 / 255.0,
    );
    adjust_gray(with_lightness(color, (LIGHTNESS_LIFT - l).min(1.0)))
}

/// Move lightness until what the color paints — its composite over the opaque backdrop — clears the target
/// contrast, keeping the candidate nearest the original that does. Chrome only lightens, and judges the raw color;
/// a backdrop left light needs the other direction, and a translucent color only ever shows as its composite.
pub fn adjust_for_contrast(color: Color, backdrop: Color, target_ratio: f64) -> Color {
    let clears = |candidate: Color| backdrop.blend(candidate).contrast_ratio(backdrop) >= target_ratio;
    if clears(color) {
        return color;
    }

    let (start_lightness, _, _) = srgb_to_oklab(
        color.red() as f32 / 255.0,
        color.green() as f32 / 255.0,
        color.blue() as f32 / 255.0,
    );

    // Lighter when white clears, darker when only black does; when the alpha lets neither reach the target,
    // the pole that paints more contrast is the best the authored alpha allows.
    let white = with_lightness(color, 1.0);
    let black = with_lightness(color, 0.0);
    let pole = if clears(white) {
        1.0f32
    } else if clears(black) {
        0.0f32
    } else if backdrop.blend(white).contrast_ratio(backdrop) >= backdrop.blend(black).contrast_ratio(backdrop) {
        1.0f32
    } else {
        0.0f32
    };
    let mut near = start_lightness;
    let mut far = pole;
    let mut best = with_lightness(color, pole);
    for _ in 0..16 {
        let mid = (near + far) / 2.0;
        let candidate = with_lightness(color, mid);
        if clears(candidate) {
            best = candidate;
            far = mid;
        } else {
            near = mid;
        }
    }
    best
}

/// The color to actually paint. Selection colors are additionally kept visible against the dark surface color, the
/// way Chrome adjusts ElementRole::kSelection when no contrast background is known.
pub fn resolve(color: Color, role: ForceDarkRole, settings: &ForceDarkSettings) -> Color {
    if role == ForceDarkRole::None || color.alpha() == 0 {
        return color;
    }
    let resolved = if should_invert(color, role, settings) {
        invert_lightness(color)
    } else {
        color
    };
    if role == ForceDarkRole::Selection {
        return adjust_for_contrast(resolved, DARK_SURFACE, MINIMUM_VISIBLE_CONTRAST_RATIO);
    }
    resolved
}

/// Resolve every stop in a gradient. Each one goes through the filter on its own, so the gradient darkens while
/// keeping its shape rather than flattening toward a single color.
pub fn resolve_each(colors: &[Color], role: ForceDarkRole, settings: &ForceDarkSettings) -> Vec<Color> {
    colors.iter().map(|color| resolve(*color, role, settings)).collect()
}

/// A resolver that memoizes the inversions it has already worked out, the way Chrome's DarkModeInvertedColorCache
/// does: a page reuses few distinct colors, so recording stops paying the Oklab round trip per command.
pub struct ForceDarkResolver {
    settings: ForceDarkSettings,
    inverted: HashMap<u32, Color>,
}

const INVERTED_COLOR_CACHE_CAPACITY: usize = 1024;

impl ForceDarkResolver {
    pub fn new(settings: ForceDarkSettings) -> Self {
        Self {
            settings,
            inverted: HashMap::new(),
        }
    }

    pub fn settings(&self) -> ForceDarkSettings {
        self.settings
    }

    pub fn resolve(&mut self, color: Color, role: ForceDarkRole) -> Color {
        if role == ForceDarkRole::None || color.alpha() == 0 {
            return color;
        }
        let resolved = if should_invert(color, role, &self.settings) {
            self.invert_memoized(color)
        } else {
            color
        };
        if role == ForceDarkRole::Selection {
            return adjust_for_contrast(resolved, DARK_SURFACE, MINIMUM_VISIBLE_CONTRAST_RATIO);
        }
        resolved
    }

    pub fn resolve_each(&mut self, colors: &[Color], role: ForceDarkRole) -> Vec<Color> {
        colors.iter().map(|color| self.resolve(*color, role)).collect()
    }

    fn invert_memoized(&mut self, color: Color) -> Color {
        if let Some(inverted) = self.inverted.get(&color.0) {
            return *inverted;
        }
        if self.inverted.len() >= INVERTED_COLOR_CACHE_CAPACITY {
            self.inverted.clear();
        }
        let inverted = invert_lightness(color);
        self.inverted.insert(color.0, inverted);
        inverted
    }
}

// Images at most this large in CSS pixels count as icons; larger is a photo unless the source is narrow enough to
// be a separator. Chrome's values (paint_auto_dark_mode.cc).
const MAX_ICON_LENGTH_CSS: f32 = 64.0;
const MAX_SEPARATOR_SOURCE_LENGTH: i32 = 8;

/// Whether an image drawn at this size may be classified at all. Photo-sized images are left untouched without ever
/// being sampled.
pub fn image_size_allows_classification(dest_width_css: f32, dest_height_css: f32, source_size: (i32, i32)) -> bool {
    if dest_width_css <= MAX_ICON_LENGTH_CSS && dest_height_css <= MAX_ICON_LENGTH_CSS {
        return true;
    }
    source_size.0 <= MAX_SEPARATOR_SOURCE_LENGTH || source_size.1 <= MAX_SEPARATOR_SOURCE_LENGTH
}

/// The role an image draw takes: the one the call site meant, or None for a photo-sized image. The gate is sized in
/// CSS pixels so page zoom can't flip an icon into a photo.
pub fn role_for_image(
    role: ForceDarkRole,
    dest_device_width: f32,
    dest_device_height: f32,
    device_pixels_per_css_pixel: f64,
    source_size: (i32, i32),
) -> ForceDarkRole {
    let scale = device_pixels_per_css_pixel as f32;
    if scale > 0.0
        && image_size_allows_classification(dest_device_width / scale, dest_device_height / scale, source_size)
    {
        role
    } else {
        ForceDarkRole::None
    }
}

// Image classification: an icon or line art reads fine inverted; a photo inverted is grotesque. Features and
// thresholds are Chrome's (dark_mode_image_classifier.cc); the band Chrome hands to a neural network declines here.

/// A pixel reads as light at or above this BT.601 luma.
const HIGH_LIGHTNESS: i32 = 96;

/// Channel spread at or below which a pixel reads as gray rather than colored.
const GRAY_CHANNEL_SPREAD: i32 = 8;

/// Below this share of the available buckets an image has too few colors to be a photo. Indexed by is_colorful.
pub const LOW_COLOR_COUNT: [f32; 2] = [0.8125, 0.015137];

/// Above this share an image is photographic. Between the two marks is the band Chrome hands to its network.
pub const HIGH_COLOR_COUNT: [f32; 2] = [1.0, 0.025635];

const TRANSPARENCY_RATIO: f32 = 0.4;
const HIGH_LUMINANCE_RATIO: f32 = 0.5;

/// Four bits of illumination for gray, four bits per channel for color.
const MAX_BUCKETS: [f32; 2] = [16.0, 4096.0];

/// What sampling an image tells us about it. Ratios are all shares of the samples taken.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ImageFeatures {
    pub color_buckets_ratio: f32,
    pub transparency_ratio: f32,
    pub high_luminance_ratio: f32,
    pub is_colorful: bool,
}

fn is_gray(color: Color) -> bool {
    let (r, g, b) = (color.red() as i32, color.green() as i32, color.blue() as i32);
    (r - g).abs() + (g - b).abs() <= GRAY_CHANNEL_SPREAD
}

/// How much of the available bucket space the samples occupy. A photo spreads across many buckets; a logo sits in a
/// handful.
fn color_buckets_ratio(samples: &[Color], is_colorful: bool) -> f32 {
    let mut seen = HashSet::new();
    for sample in samples {
        let bucket = if is_colorful {
            ((sample.red() as u16 >> 4) << 8) | ((sample.green() as u16 >> 4) << 4) | (sample.blue() as u16 >> 4)
        } else {
            let illumination = (sample.red() as u16 * 5 + sample.green() as u16 * 3 + sample.blue() as u16 * 2) / 10;
            illumination / 16
        };
        seen.insert(bucket);
    }
    seen.len() as f32 / MAX_BUCKETS[is_colorful as usize]
}

pub fn features_from_samples(samples: &[Color], transparency_ratio: f32) -> ImageFeatures {
    let count = samples.len();
    if count == 0 {
        // Nothing sampled is nothing to judge on; a ratio at the photographic end declines below.
        return ImageFeatures {
            color_buckets_ratio: 1.0,
            transparency_ratio,
            high_luminance_ratio: 0.0,
            is_colorful: false,
        };
    }

    let mut color_pixels = 0usize;
    let mut light_pixels = 0usize;
    for sample in samples {
        if !is_gray(*sample) {
            color_pixels += 1;
        }
        if brightness(*sample) >= HIGH_LIGHTNESS {
            light_pixels += 1;
        }
    }

    // A hundredth of the samples carrying color is enough to treat the whole image as colored.
    let is_colorful = color_pixels > count / 100;
    ImageFeatures {
        color_buckets_ratio: color_buckets_ratio(samples, is_colorful),
        transparency_ratio,
        high_luminance_ratio: light_pixels as f32 / count as f32,
        is_colorful,
    }
}

pub fn should_filter(features: &ImageFeatures) -> bool {
    // Light pixels floating on transparency would darken into whatever dark backdrop sits behind them, so the image
    // would sink out of view rather than adapt.
    if features.transparency_ratio > TRANSPARENCY_RATIO && features.high_luminance_ratio > HIGH_LUMINANCE_RATIO {
        return false;
    }

    // Too few colors to be a photo: an icon, a logo, line art, text rendered to an image.
    if features.color_buckets_ratio < LOW_COLOR_COUNT[features.is_colorful as usize] {
        return true;
    }

    false
}

#[cfg(test)]
mod tests {
    use super::*;

    const WHITE: Color = Color::from_rgb(255, 255, 255);
    const BLACK: Color = Color::from_rgb(0, 0, 0);

    fn thresholds(foreground: i32, background: i32) -> ForceDarkSettings {
        ForceDarkSettings {
            foreground_brightness_threshold: foreground,
            background_brightness_threshold: background,
        }
    }

    #[test]
    fn brightness_uses_bt601_weights() {
        assert_eq!(brightness(WHITE), 255);
        assert_eq!(brightness(BLACK), 0);
        assert!(brightness(Color::from_rgb(0, 255, 0)) > brightness(Color::from_rgb(255, 0, 0)));
        assert!(brightness(Color::from_rgb(255, 0, 0)) > brightness(Color::from_rgb(0, 0, 255)));
    }

    #[test]
    fn foreground_inverts_below_its_threshold() {
        let settings = thresholds(128, 205);
        assert!(should_invert(BLACK, ForceDarkRole::Foreground, &settings));
        assert!(!should_invert(WHITE, ForceDarkRole::Foreground, &settings));
        assert!(!should_invert(
            Color::from_rgb(128, 128, 128),
            ForceDarkRole::Foreground,
            &settings
        ));
    }

    #[test]
    fn background_inverts_above_its_threshold() {
        let settings = thresholds(150, 128);
        assert!(should_invert(WHITE, ForceDarkRole::Background, &settings));
        assert!(!should_invert(BLACK, ForceDarkRole::Background, &settings));
        assert!(!should_invert(
            Color::from_rgb(128, 128, 128),
            ForceDarkRole::Background,
            &settings
        ));
    }

    #[test]
    fn degenerate_foreground_thresholds_short_circuit() {
        assert!(should_invert(WHITE, ForceDarkRole::Foreground, &thresholds(255, 205)));
        assert!(!should_invert(BLACK, ForceDarkRole::Foreground, &thresholds(0, 205)));
    }

    #[test]
    fn degenerate_background_thresholds_are_reversed() {
        assert!(!should_invert(WHITE, ForceDarkRole::Background, &thresholds(150, 255)));
        assert!(should_invert(BLACK, ForceDarkRole::Background, &thresholds(150, 0)));
    }

    #[test]
    fn selection_classifies_with_the_background_rule() {
        // Chrome sends kSelection through the background classifier (DarkModeFilter::ShouldApplyToColor); a light
        // selection color inverts, a dark one is left alone.
        let settings = ForceDarkSettings::default();
        assert!(should_invert(
            Color::from_rgb(224, 224, 224),
            ForceDarkRole::Selection,
            &settings
        ));
        assert!(!should_invert(
            Color::from_rgb(40, 40, 60),
            ForceDarkRole::Selection,
            &settings
        ));
    }

    #[test]
    fn borders_svg_and_list_symbols_classify_with_the_foreground_rule() {
        let settings = thresholds(128, 205);
        for role in [ForceDarkRole::Border, ForceDarkRole::Svg, ForceDarkRole::ListSymbol] {
            assert!(should_invert(BLACK, role, &settings));
            assert!(!should_invert(WHITE, role, &settings));
        }
    }

    #[test]
    fn defaults_preserve_already_dark_pages() {
        // Chrome's shipped thresholds leave dark backgrounds and light text as they are, so a page's black navbar
        // stays black instead of flipping to white.
        let settings = ForceDarkSettings::default();
        let dark_background = Color::from_rgb(0x20, 0x20, 0x20);
        let light_text = Color::from_rgb(0xee, 0xee, 0xee);
        assert_eq!(
            resolve(dark_background, ForceDarkRole::Background, &settings),
            dark_background
        );
        assert_eq!(resolve(light_text, ForceDarkRole::Foreground, &settings), light_text);
        // While the light background and dark text of a light page invert.
        assert_ne!(resolve(WHITE, ForceDarkRole::Background, &settings), WHITE);
        assert_ne!(resolve(BLACK, ForceDarkRole::Foreground, &settings), BLACK);
    }

    #[test]
    fn oklab_round_trips() {
        for color in [WHITE, BLACK, Color::from_rgb(200, 30, 90), Color::from_rgb(18, 52, 86)] {
            let (r, g, b) = (
                color.red() as f32 / 255.0,
                color.green() as f32 / 255.0,
                color.blue() as f32 / 255.0,
            );
            let (l, a, bb) = srgb_to_oklab(r, g, b);
            let (r2, g2, b2) = oklab_to_srgb(l, a, bb);
            assert!((r - r2).abs() < 0.01, "r {r} -> {r2}");
            assert!((g - g2).abs() < 0.01, "g {g} -> {g2}");
            assert!((b - b2).abs() < 0.01, "b {b} -> {b2}");
        }
    }

    #[test]
    fn white_lands_on_the_dark_surface_value() {
        // White inverts to just inside adjust_gray's band and snaps to the surface value. Pinning the exact result
        // matters: a lift landing white below the band crushes it toward black, and brightness > 0 wouldn't notice.
        assert_eq!(invert_lightness(WHITE), Color::from_rgb(18, 18, 18));
    }

    #[test]
    fn mid_gray_is_a_fixed_point() {
        // The lift is chosen so that sRGB mid-gray inverts to itself, making the inversion a reflection about the
        // midpoint rather than a slide toward one end.
        let inverted = invert_lightness(Color::from_rgb(128, 128, 128));
        assert!(
            inverted.red().abs_diff(128) <= 1,
            "mid-gray should invert to itself, got {}",
            inverted.red()
        );
    }

    #[test]
    fn grays_stay_distinguishable_after_inversion() {
        // A lift that's too small crushes the light end: white and #eeeeee both collapse to near-black, and the page
        // loses the banding it used for structure.
        let white = brightness(invert_lightness(WHITE));
        let near_white = brightness(invert_lightness(Color::from_rgb(0xee, 0xee, 0xee)));
        assert!(
            near_white - white >= 12,
            "white {white} and #eeeeee {near_white} should stay apart"
        );
    }

    #[test]
    fn black_lightens() {
        assert!(brightness(invert_lightness(BLACK)) > 200);
    }

    #[test]
    fn inversion_preserves_alpha() {
        let translucent = Color::from_rgba(200, 30, 90, 128);
        assert_eq!(invert_lightness(translucent).alpha(), 128);
    }

    #[test]
    fn inversion_roughly_preserves_hue() {
        // A red stays recognizably red: inverting lightness must not rotate hue into another family.
        let inverted = invert_lightness(Color::from_rgb(200, 30, 30));
        assert!(inverted.red() > inverted.green());
        assert!(inverted.red() > inverted.blue());
    }

    #[test]
    fn adjust_gray_snaps_near_black_grays_to_the_dark_surface_value() {
        assert_eq!(adjust_gray(Color::from_rgb(20, 20, 20)), Color::from_rgb(18, 18, 18));
        // Not a gray: left alone even though it is dark.
        let dark_blue = Color::from_rgb(0, 0, 40);
        assert_eq!(adjust_gray(dark_blue), dark_blue);
        // Gray, but above the threshold: left alone.
        let mid_gray = Color::from_rgb(128, 128, 128);
        assert_eq!(adjust_gray(mid_gray), mid_gray);
    }

    #[test]
    fn contrast_adjustment_reaches_the_target_when_possible() {
        let backdrop = Color::from_rgb(20, 20, 20);
        let text = Color::from_rgb(40, 40, 40);
        assert!(text.contrast_ratio(backdrop) < 4.5);
        let adjusted = adjust_for_contrast(text, backdrop, 4.5);
        assert!(adjusted.contrast_ratio(backdrop) >= 4.5);
    }

    #[test]
    fn contrast_adjustment_leaves_adequate_colors_alone() {
        let backdrop = Color::from_rgb(20, 20, 20);
        assert_eq!(adjust_for_contrast(WHITE, backdrop, 4.5), WHITE);
    }

    #[test]
    fn none_leaves_the_color_alone() {
        assert_eq!(
            resolve(WHITE, ForceDarkRole::None, &ForceDarkSettings::default()),
            WHITE
        );
    }

    #[test]
    fn a_white_background_goes_dark() {
        let resolved = resolve(WHITE, ForceDarkRole::Background, &ForceDarkSettings::default());
        assert!(brightness(resolved) < brightness(WHITE));
    }

    #[test]
    fn black_text_goes_light() {
        let resolved = resolve(BLACK, ForceDarkRole::Foreground, &ForceDarkSettings::default());
        assert!(brightness(resolved) > brightness(BLACK));
    }

    #[test]
    fn a_background_the_threshold_excludes_is_left_alone() {
        let dark = Color::from_rgb(0x20, 0x20, 0x20);
        assert_eq!(resolve(dark, ForceDarkRole::Background, &thresholds(255, 128)), dark);
    }

    #[test]
    fn a_foreground_the_threshold_excludes_is_left_alone() {
        let light = Color::from_rgb(0xe0, 0xe0, 0xe0);
        assert_eq!(resolve(light, ForceDarkRole::Foreground, &thresholds(128, 0)), light);
    }

    #[test]
    fn alpha_survives_the_filter() {
        let translucent = Color::from_rgba(0xff, 0xff, 0xff, 0x80);
        let resolved = resolve(translucent, ForceDarkRole::Background, &ForceDarkSettings::default());
        assert_eq!(resolved.alpha(), 0x80);
    }

    #[test]
    fn a_fully_transparent_color_is_left_alone() {
        // Inverting something invisible only costs time, and it can turn a no-op fill into a visible one.
        let invisible = Color::from_rgba(0xff, 0xff, 0xff, 0);
        assert_eq!(
            resolve(invisible, ForceDarkRole::Background, &ForceDarkSettings::default()),
            invisible
        );
    }

    #[test]
    fn a_translucent_selection_is_judged_as_painted() {
        // Alpha-blind contrast flatters a translucent selection: its raw RGB clears the floor while its composite
        // over the dark surface doesn't. The judgment has to see what paints.
        let settings = ForceDarkSettings::default();
        let resolved = resolve(
            Color::from_rgba(140, 140, 140, 140),
            ForceDarkRole::Selection,
            &settings,
        );
        assert_eq!(resolved.alpha(), 140);
        assert!(
            DARK_SURFACE.blend(resolved).contrast_ratio(DARK_SURFACE) >= MINIMUM_VISIBLE_CONTRAST_RATIO,
            "painted selection landed at {:?}, contrast {}",
            DARK_SURFACE.blend(resolved),
            DARK_SURFACE.blend(resolved).contrast_ratio(DARK_SURFACE)
        );
    }

    #[test]
    fn selection_stays_visible_against_the_dark_surface() {
        // A white selection background inverts onto the dark surface color itself, where it would disappear; the
        // contrast floor lifts it back to visibility.
        let settings = ForceDarkSettings::default();
        assert_eq!(resolve(WHITE, ForceDarkRole::Background, &settings), DARK_SURFACE);
        let resolved = resolve(WHITE, ForceDarkRole::Selection, &settings);
        assert!(
            resolved.contrast_ratio(DARK_SURFACE) >= MINIMUM_VISIBLE_CONTRAST_RATIO,
            "selection landed at {resolved:?}, contrast {}",
            resolved.contrast_ratio(DARK_SURFACE)
        );
    }

    #[test]
    fn every_gradient_stop_is_resolved_on_its_own() {
        let stops = [WHITE, Color::from_rgb(0x80, 0x80, 0x80), BLACK];
        let settings = thresholds(255, 0);
        let resolved = resolve_each(&stops, ForceDarkRole::Background, &settings);
        assert_eq!(resolved.len(), stops.len());
        for (before, after) in stops.iter().zip(&resolved) {
            assert_eq!(*after, resolve(*before, ForceDarkRole::Background, &settings));
        }
        // The ends swap places, so the gradient keeps its shape instead of flattening.
        assert!(brightness(resolved[0]) < brightness(resolved[2]));
    }

    #[test]
    fn gradient_stops_are_left_alone_for_role_none() {
        let stops = [WHITE, BLACK];
        assert_eq!(
            resolve_each(&stops, ForceDarkRole::None, &ForceDarkSettings::default()),
            stops.to_vec()
        );
    }

    #[test]
    fn icon_sized_and_separator_sized_images_may_classify() {
        assert!(image_size_allows_classification(64.0, 64.0, (64, 64)));
        assert!(image_size_allows_classification(48.0, 20.0, (256, 256)));
        // A separator is narrow at the source, whatever it stretches to.
        assert!(image_size_allows_classification(300.0, 300.0, (8, 200)));
        assert!(image_size_allows_classification(1000.0, 4.0, (2000, 3)));
    }

    #[test]
    fn photo_sized_images_never_classify() {
        assert!(!image_size_allows_classification(65.0, 64.0, (256, 256)));
        assert!(!image_size_allows_classification(800.0, 600.0, (1024, 768)));
    }

    fn solid(n: usize, color: Color) -> Vec<Color> {
        vec![color; n]
    }

    #[test]
    fn a_flat_logo_is_inverted() {
        // One color fills every bucket sample, which is nothing like a photo.
        let samples = solid(200, Color::from_rgb(0xd0, 0x10, 0x10));
        assert!(should_filter(&features_from_samples(&samples, 0.0)));
    }

    #[test]
    fn a_photo_is_left_alone() {
        // Spread the samples over enough distinct buckets to read as photographic.
        let samples: Vec<Color> = (0..240)
            .map(|i: u32| Color::from_rgb((i * 7 % 256) as u8, (i * 13 % 256) as u8, (i * 29 % 256) as u8))
            .collect();
        let features = features_from_samples(&samples, 0.0);
        assert!(features.is_colorful);
        assert!(!should_filter(&features));
    }

    #[test]
    fn a_grayscale_icon_is_inverted() {
        let samples = solid(200, Color::from_rgb(0x64, 0x64, 0x64));
        let features = features_from_samples(&samples, 0.0);
        assert!(!features.is_colorful);
        assert!(should_filter(&features));
    }

    #[test]
    fn a_light_image_on_transparency_is_left_alone() {
        // Inverting this would darken the pixels that show, sinking them into the dark backdrop behind.
        let samples = solid(200, Color::from_rgb(0xf0, 0xf0, 0xf0));
        let features = features_from_samples(&samples, 0.5);
        assert!(features.high_luminance_ratio > 0.5);
        assert!(!should_filter(&features));
    }

    #[test]
    fn the_undecided_middle_is_left_alone() {
        // A photo that inverts is far worse than an icon left bright, so the ambiguous band declines.
        let features = ImageFeatures {
            color_buckets_ratio: 0.02,
            transparency_ratio: 0.0,
            high_luminance_ratio: 0.0,
            is_colorful: true,
        };
        assert!(features.color_buckets_ratio > LOW_COLOR_COUNT[1]);
        assert!(features.color_buckets_ratio < HIGH_COLOR_COUNT[1]);
        assert!(!should_filter(&features));
    }

    #[test]
    fn transparency_alone_does_not_spare_a_dark_image() {
        // The guard needs both halves: transparent AND mostly light.
        let samples = solid(200, Color::from_rgb(0x10, 0x10, 0x10));
        let features = features_from_samples(&samples, 0.9);
        assert!(features.high_luminance_ratio < 0.5);
        assert!(should_filter(&features));
    }

    #[test]
    fn an_empty_sample_set_is_left_alone() {
        assert!(!should_filter(&features_from_samples(&[], 0.0)));
    }

    #[test]
    fn the_resolver_matches_the_pure_functions() {
        let settings = ForceDarkSettings::default();
        let mut resolver = ForceDarkResolver::new(settings);
        let colors = [
            WHITE,
            BLACK,
            Color::from_rgb(200, 30, 90),
            Color::from_rgb(0xef, 0xef, 0xef),
        ];
        for role in [
            ForceDarkRole::None,
            ForceDarkRole::Foreground,
            ForceDarkRole::Background,
            ForceDarkRole::Border,
            ForceDarkRole::Selection,
        ] {
            for color in colors {
                // Twice, so the second answer comes from the memo.
                assert_eq!(resolver.resolve(color, role), resolve(color, role, &settings));
                assert_eq!(resolver.resolve(color, role), resolve(color, role, &settings));
            }
        }
    }
}
