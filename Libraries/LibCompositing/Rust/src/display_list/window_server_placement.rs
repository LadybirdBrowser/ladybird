/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where the window server can show by itself a canvas or a video that is a layer of its own. The window server places
//! an image with a 2D affine transform and clips it to rectangles, one of which can have rounded corners of a single
//! radius, and to one path. A media layer drawn under any other clip has to be replayed instead.
//!
//! Also how the window server composites by itself an effect around the layers of what the effect holds: within
//! rectangles, rounded or not, at an opacity, or over what it shows below the effect's box run through a blur and
//! color matrices, as a backdrop filter of the root group reads it. An effect that does anything else to its content,
//! or that is clipped otherwise, has to be composited by the compositor instead.

use crate::display_list::builder::{inline_clips_of, inline_transform_of, read_command};
use crate::display_list::commands::{
    ClipMode, ClipNodeIndex, DisplayListCommandType, DrawCanvas, DrawVideoFrame, EffectNodeIndex, SpatialNodeIndex,
};
use crate::display_list::replay::backdrop_is_drawn_by_masks;
use crate::display_list::tape::{DisplayListCommandHeader, DisplayListCommandRun, InlineClipKind};
use crate::visual_context::{ClipNodeData, ClipPathData, EffectNodeData, VisualContextTree, WorldTransforms};
use libgfx_rust::filter::Filter;
use libgfx_rust::path::Path;
use libgfx_rust::{
    AffineTransform, ColorFilterType, CompositingAndBlendingOperator, CornerRadii, CornerRadius, FloatMatrix4x4,
    FloatRect, IntRect, ScalingMode, WindingRule, multiply_affine,
};
use std::sync::Arc;

const UNIFORM_SCALE_TOLERANCE: f32 = 0.0001;

const RADIUS_TOLERANCE_IN_DEVICE_PIXELS: f32 = 0.01;

#[derive(Clone, Debug, PartialEq)]
pub struct WindowServerPlacement {
    /// Where the record draws its content, in the record's own space.
    pub content_rect: IntRect,
    pub content_to_viewport: AffineTransform,
    /// The intersection of every clip the record is drawn under, rounded or not, and of the bounds of the path it is
    /// clipped by, in viewport pixels.
    pub clip_rect: FloatRect,
    pub rounded_clip: Option<RoundedClip>,
    pub path_clip: Option<PathClip>,
    pub scaling_mode: ScalingMode,
}

/// A path the record is clipped by, which stays in the space of its clip node.
#[derive(Clone, Debug)]
pub struct PathClip {
    pub path: Arc<Path>,
    /// The bounds of the path, in its own space.
    pub bounding_rect: IntRect,
    pub path_to_viewport: AffineTransform,
    pub fill_rule: WindingRule,
}

impl PartialEq for PathClip {
    fn eq(&self, other: &Self) -> bool {
        self.path.identity() == other.path.identity()
            && self.bounding_rect == other.bounding_rect
            && self.path_to_viewport == other.path_to_viewport
            && self.fill_rule == other.fill_rule
    }
}

impl PathClip {
    pub fn bounds_in_viewport(&self) -> FloatRect {
        self.path_to_viewport.map_rect(self.bounding_rect.to_float())
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RoundedClip {
    pub rect: FloatRect,
    pub radius: f32,
    pub rounded_corners: RoundedCorners,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RoundedCorners {
    pub top_left: bool,
    pub top_right: bool,
    pub bottom_right: bool,
    pub bottom_left: bool,
}

impl RoundedCorners {
    pub fn are_all_rounded(self) -> bool {
        self.top_left && self.top_right && self.bottom_right && self.bottom_left
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WhyWindowServerCannotShowMedia {
    ClippedByPath,
    ClippedByMoreThanOnePath,
    ClippedByExcludingClip,
    ClippedInRotatedOrSkewedSpace,
    ClippedInSpaceTreeDoesNotHave,
    RoundedCornersOfDifferingRadii,
    MoreThanOneRoundedClip,
}

impl std::fmt::Display for WhyWindowServerCannotShowMedia {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            WhyWindowServerCannotShowMedia::ClippedByPath => "clipped by a path",
            WhyWindowServerCannotShowMedia::ClippedByMoreThanOnePath => "clipped by more than one path",
            WhyWindowServerCannotShowMedia::ClippedByExcludingClip => "clipped by an excluding clip",
            WhyWindowServerCannotShowMedia::ClippedInRotatedOrSkewedSpace => "clipped in a rotated or skewed space",
            WhyWindowServerCannotShowMedia::ClippedInSpaceTreeDoesNotHave => {
                "clipped in a space the tree does not have"
            }
            WhyWindowServerCannotShowMedia::RoundedCornersOfDifferingRadii => "rounded corners of differing radii",
            WhyWindowServerCannotShowMedia::MoreThanOneRoundedClip => "more than one rounded clip",
        })
    }
}

impl std::fmt::Display for WindowServerPlacement {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if !self.content_to_viewport.is_identity() {
            let [a, b, c, d, e, f] = self.content_to_viewport.values;
            write!(formatter, "transform=[{a},{b},{c},{d},{e},{f}] ")?;
        }
        write_float_rect(formatter, "clip", self.clip_rect)?;
        if let Some(rounded_clip) = self.rounded_clip {
            formatter.write_str(" ")?;
            write_float_rect(formatter, "rounded", rounded_clip.rect)?;
            write!(formatter, " radius={}", rounded_clip.radius)?;
            let corners = rounded_clip.rounded_corners;
            if !corners.are_all_rounded() {
                let names = [
                    (corners.top_left, "top-left"),
                    (corners.top_right, "top-right"),
                    (corners.bottom_right, "bottom-right"),
                    (corners.bottom_left, "bottom-left"),
                ];
                let rounded: Vec<&str> = names
                    .iter()
                    .filter(|(rounded, _)| *rounded)
                    .map(|(_, name)| *name)
                    .collect();
                write!(formatter, " corners={}", rounded.join(","))?;
            }
        }
        if let Some(path_clip) = &self.path_clip {
            formatter.write_str(" ")?;
            write_float_rect(formatter, "path", path_clip.bounds_in_viewport())?;
            if path_clip.fill_rule == WindingRule::EvenOdd {
                formatter.write_str(" evenodd")?;
            }
        }
        Ok(())
    }
}

fn write_float_rect(formatter: &mut std::fmt::Formatter<'_>, name: &str, rect: FloatRect) -> std::fmt::Result {
    write!(
        formatter,
        "{name}=[{},{} {}x{}]",
        rect.x, rect.y, rect.width, rect.height
    )
}

/// A rect whose corners can be rounded, with the horizontal and the vertical radius of each corner.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct RoundedRect {
    pub(crate) rect: FloatRect,
    pub(crate) horizontal_and_vertical_radii_clockwise_from_top_left: [(f32, f32); 4],
}

impl RoundedRect {
    /// The boxes that the rounded corners curve within, empty for a corner that is not rounded.
    fn corner_boxes(&self) -> [FloatRect; 4] {
        let rect = self.rect;
        let [top_left, top_right, bottom_right, bottom_left] =
            self.horizontal_and_vertical_radii_clockwise_from_top_left;
        [
            FloatRect::new(rect.x, rect.y, top_left.0, top_left.1),
            FloatRect::new(rect.right() - top_right.0, rect.y, top_right.0, top_right.1),
            FloatRect::new(
                rect.right() - bottom_right.0,
                rect.bottom() - bottom_right.1,
                bottom_right.0,
                bottom_right.1,
            ),
            FloatRect::new(rect.x, rect.bottom() - bottom_left.1, bottom_left.0, bottom_left.1),
        ]
    }

    pub(crate) fn rounded_corner_reaches_into(&self, area: FloatRect) -> bool {
        self.corner_boxes()
            .iter()
            .any(|corner_box| !corner_box.is_empty() && !corner_box.intersected(area).is_empty())
    }

    fn with_single_radius(&self) -> Result<RoundedClip, WhyWindowServerCannotShowMedia> {
        let mut single_radius = None;
        let mut corner_is_rounded = [false; 4];
        for (corner_index, &(horizontal, vertical)) in self
            .horizontal_and_vertical_radii_clockwise_from_top_left
            .iter()
            .enumerate()
        {
            if horizontal <= 0.0 || vertical <= 0.0 {
                continue;
            }
            let radius_of_first_rounded_corner = *single_radius.get_or_insert(horizontal);
            if (horizontal - radius_of_first_rounded_corner).abs() > RADIUS_TOLERANCE_IN_DEVICE_PIXELS
                || (vertical - radius_of_first_rounded_corner).abs() > RADIUS_TOLERANCE_IN_DEVICE_PIXELS
            {
                return Err(WhyWindowServerCannotShowMedia::RoundedCornersOfDifferingRadii);
            }
            corner_is_rounded[corner_index] = true;
        }
        Ok(RoundedClip {
            rect: self.rect,
            radius: single_radius.unwrap_or(0.0),
            rounded_corners: RoundedCorners {
                top_left: corner_is_rounded[0],
                top_right: corner_is_rounded[1],
                bottom_right: corner_is_rounded[2],
                bottom_left: corner_is_rounded[3],
            },
        })
    }
}

fn clip_leaves_content_whole(
    rect: FloatRect,
    corner_radii: CornerRadii,
    mode: ClipMode,
    content_rect: FloatRect,
) -> bool {
    if mode != ClipMode::Intersect || !rect.contains_rect(content_rect) {
        return false;
    }
    let radii = |corner: CornerRadius| {
        if !corner.is_present() {
            return (0.0, 0.0);
        }
        (corner.horizontal_radius as f32, corner.vertical_radius as f32)
    };
    let rounded_rect = RoundedRect {
        rect,
        horizontal_and_vertical_radii_clockwise_from_top_left: [
            radii(corner_radii.top_left),
            radii(corner_radii.top_right),
            radii(corner_radii.bottom_right),
            radii(corner_radii.bottom_left),
        ],
    };
    !rounded_rect.rounded_corner_reaches_into(content_rect)
}

/// The matrix from a space that a clip is in to the viewport, where the window server can clip in that space: one
/// that is 2D affine, without rotation or skew.
fn clip_space_to_viewport(
    world: &WorldTransforms,
    space: SpatialNodeIndex,
) -> Result<FloatMatrix4x4, WhyWindowServerCannotShowMedia> {
    let matrix = world
        .draw_matrix_if_known(space)
        .ok_or(WhyWindowServerCannotShowMedia::ClippedInSpaceTreeDoesNotHave)?;
    let elements = &matrix.elements;
    if !matrix.is_2d_affine() || elements[0][1] != 0.0 || elements[1][0] != 0.0 {
        return Err(WhyWindowServerCannotShowMedia::ClippedInRotatedOrSkewedSpace);
    }
    Ok(matrix)
}

#[derive(Default)]
struct ClipsInViewport {
    rect_clips: Vec<FloatRect>,
    rounded_clips: Vec<RoundedRect>,
    path_clip: Option<PathClip>,
}

impl ClipsInViewport {
    fn add(
        &mut self,
        world: &WorldTransforms,
        space: SpatialNodeIndex,
        rect: FloatRect,
        corner_radii: CornerRadii,
        mode: ClipMode,
    ) -> Result<(), WhyWindowServerCannotShowMedia> {
        if mode == ClipMode::Difference {
            return Err(WhyWindowServerCannotShowMedia::ClippedByExcludingClip);
        }
        let matrix = clip_space_to_viewport(world, space)?;
        let elements = &matrix.elements;
        let rect_in_viewport = matrix.extract_2d_affine().map_rect(rect);
        if !corner_radii.has_any_radius() {
            self.rect_clips.push(rect_in_viewport);
            return Ok(());
        }
        let (scale_x, scale_y) = (elements[0][0], elements[1][1]);
        let radii_scaled_into_viewport = |corner: CornerRadius| {
            if !corner.is_present() {
                return (0.0, 0.0);
            }
            (
                corner.horizontal_radius as f32 * scale_x.abs(),
                corner.vertical_radius as f32 * scale_y.abs(),
            )
        };
        let mut radii = [
            radii_scaled_into_viewport(corner_radii.top_left),
            radii_scaled_into_viewport(corner_radii.top_right),
            radii_scaled_into_viewport(corner_radii.bottom_right),
            radii_scaled_into_viewport(corner_radii.bottom_left),
        ];
        let mirrors_left_and_right = scale_x < 0.0;
        if mirrors_left_and_right {
            radii = [radii[1], radii[0], radii[3], radii[2]];
        }
        let mirrors_top_and_bottom = scale_y < 0.0;
        if mirrors_top_and_bottom {
            radii = [radii[3], radii[2], radii[1], radii[0]];
        }
        self.rounded_clips.push(RoundedRect {
            rect: rect_in_viewport,
            horizontal_and_vertical_radii_clockwise_from_top_left: radii,
        });
        Ok(())
    }

    /// Adds a clip by a path in `space`, whose bounds join the rect clips.
    fn add_path(
        &mut self,
        world: &WorldTransforms,
        space: SpatialNodeIndex,
        clip_path: &ClipPathData,
    ) -> Result<(), WhyWindowServerCannotShowMedia> {
        if self.path_clip.is_some() {
            return Err(WhyWindowServerCannotShowMedia::ClippedByMoreThanOnePath);
        }
        let path_clip = PathClip {
            path: clip_path.path.clone(),
            bounding_rect: clip_path.bounding_rect,
            path_to_viewport: clip_space_to_viewport(world, space)?.extract_2d_affine(),
            fill_rule: clip_path.fill_rule,
        };
        self.rect_clips.push(path_clip.bounds_in_viewport());
        self.path_clip = Some(path_clip);
        Ok(())
    }
}

/// How the window server composites an effect by itself around the layers of what the effect holds.
#[derive(Clone, Debug, PartialEq)]
pub struct WindowServerPlacementOfEffect {
    /// The clips the effect is composited within, in viewport pixels, with the radii of the corners each rounds.
    pub clips: Vec<(FloatRect, CornerRadii)>,
    pub opacity: f32,
    /// What the effect's backdrop filter does to what the window server shows below the effect's box, if it has one.
    pub backdrop: Option<WindowServerBackdrop>,
}

/// What the window server does to what it shows below a box with a backdrop filter, within the box's border box.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WindowServerBackdrop {
    /// The border box, which the filtered backdrop fills, in viewport pixels, and the radii of its rounded corners.
    pub region: FloatRect,
    pub corner_radii: CornerRadii,
    pub filters: WindowServerBackdropFilters,
}

/// The functions of a backdrop filter as the window server applies them: a color matrix, a blur and another color
/// matrix, in that order, each of which a filter can do without. A matrix is row-major with the offsets in the fifth
/// column, for unpremultiplied colors from 0 to 1.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WindowServerBackdropFilters {
    pub color_matrix_before_blur: Option<[f32; 20]>,
    /// The standard deviation of the blur, in viewport pixels.
    pub blur_standard_deviation: Option<f32>,
    pub color_matrix_after_blur: Option<[f32; 20]>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WhyWindowServerCannotCompositeEffect {
    Masks,
    BlendsWithWhatIsBelowIt,
    Filters,
    WindowServerFiltersNothingBelowLayers,
    BackdropFilterFunctionWithoutWindowServerEquivalent,
    BackdropDrawnByMasks,
    BackdropReadInsideAnotherEffect,
    BoxWithBackdropFilterHasOpacity,
    BoxWithBackdropFilterIsNotUniformlyScaled,
    Clipped(WhyWindowServerCannotShowMedia),
}

impl std::fmt::Display for WhyWindowServerCannotCompositeEffect {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            WhyWindowServerCannotCompositeEffect::Masks => "it masks",
            WhyWindowServerCannotCompositeEffect::BlendsWithWhatIsBelowIt => "it blends with what is below it",
            WhyWindowServerCannotCompositeEffect::Filters => "it filters",
            WhyWindowServerCannotCompositeEffect::WindowServerFiltersNothingBelowLayers => {
                "the window server filters nothing below layers"
            }
            WhyWindowServerCannotCompositeEffect::BackdropFilterFunctionWithoutWindowServerEquivalent => {
                "its backdrop filter has a function the window server has no equivalent of"
            }
            WhyWindowServerCannotCompositeEffect::BackdropDrawnByMasks => "its backdrop is drawn by masks",
            WhyWindowServerCannotCompositeEffect::BackdropReadInsideAnotherEffect => {
                "its backdrop is read inside another effect"
            }
            WhyWindowServerCannotCompositeEffect::BoxWithBackdropFilterHasOpacity => {
                "its box has a backdrop filter and an opacity"
            }
            WhyWindowServerCannotCompositeEffect::BoxWithBackdropFilterIsNotUniformlyScaled => {
                "its box has a backdrop filter under a transform that is not a uniform scale"
            }
            WhyWindowServerCannotCompositeEffect::Clipped(why) => return write!(formatter, "it is {why}"),
        })
    }
}

// The matrices are those of https://drafts.fxtf.org/filter-effects-1/#FilterPrimitiveRepresentation.
#[rustfmt::skip]
fn color_filter_matrix(kind: ColorFilterType, amount: f32) -> [f32; 20] {
    let rest = 1.0 - amount;
    match kind {
        ColorFilterType::Grayscale => [
            0.2126 + 0.7874 * rest, 0.7152 - 0.7152 * rest, 0.0722 - 0.0722 * rest, 0.0, 0.0,
            0.2126 - 0.2126 * rest, 0.7152 + 0.2848 * rest, 0.0722 - 0.0722 * rest, 0.0, 0.0,
            0.2126 - 0.2126 * rest, 0.7152 - 0.7152 * rest, 0.0722 + 0.9278 * rest, 0.0, 0.0,
            0.0, 0.0, 0.0, 1.0, 0.0,
        ],
        ColorFilterType::Brightness => [
            amount, 0.0, 0.0, 0.0, 0.0,
            0.0, amount, 0.0, 0.0, 0.0,
            0.0, 0.0, amount, 0.0, 0.0,
            0.0, 0.0, 0.0, 1.0, 0.0,
        ],
        ColorFilterType::Contrast => {
            let intercept = 0.5 - 0.5 * amount;
            [
                amount, 0.0, 0.0, 0.0, intercept,
                0.0, amount, 0.0, 0.0, intercept,
                0.0, 0.0, amount, 0.0, intercept,
                0.0, 0.0, 0.0, 1.0, 0.0,
            ]
        }
        ColorFilterType::Invert => {
            let scale = 1.0 - 2.0 * amount;
            [
                scale, 0.0, 0.0, 0.0, amount,
                0.0, scale, 0.0, 0.0, amount,
                0.0, 0.0, scale, 0.0, amount,
                0.0, 0.0, 0.0, 1.0, 0.0,
            ]
        }
        ColorFilterType::Opacity => [
            1.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 0.0, amount, 0.0,
        ],
        ColorFilterType::Sepia => [
            0.393 + 0.607 * rest, 0.769 - 0.769 * rest, 0.189 - 0.189 * rest, 0.0, 0.0,
            0.349 - 0.349 * rest, 0.686 + 0.314 * rest, 0.168 - 0.168 * rest, 0.0, 0.0,
            0.272 - 0.272 * rest, 0.534 - 0.534 * rest, 0.131 + 0.869 * rest, 0.0, 0.0,
            0.0, 0.0, 0.0, 1.0, 0.0,
        ],
        ColorFilterType::Saturate => [
            0.213 + 0.787 * amount, 0.715 - 0.715 * amount, 0.072 - 0.072 * amount, 0.0, 0.0,
            0.213 - 0.213 * amount, 0.715 + 0.285 * amount, 0.072 - 0.072 * amount, 0.0, 0.0,
            0.213 - 0.213 * amount, 0.715 - 0.715 * amount, 0.072 + 0.928 * amount, 0.0, 0.0,
            0.0, 0.0, 0.0, 1.0, 0.0,
        ],
    }
}

#[rustfmt::skip]
fn hue_rotate_matrix(angle_degrees: f32) -> [f32; 20] {
    let (sin, cos) = ak::math::sincos(angle_degrees.to_radians());
    [
        0.213 + cos * 0.787 - sin * 0.213, 0.715 - cos * 0.715 - sin * 0.715, 0.072 - cos * 0.072 + sin * 0.928, 0.0, 0.0,
        0.213 - cos * 0.213 + sin * 0.143, 0.715 + cos * 0.285 + sin * 0.140, 0.072 - cos * 0.072 - sin * 0.283, 0.0, 0.0,
        0.213 - cos * 0.213 - sin * 0.787, 0.715 - cos * 0.715 + sin * 0.715, 0.072 + cos * 0.928 + sin * 0.072, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0, 0.0,
    ]
}

/// The one color matrix that does what `first` and then `second` do.
fn color_matrix_applying_first_then_second(first: &[f32; 20], second: &[f32; 20]) -> [f32; 20] {
    let mut combined = [0.0; 20];
    for row in 0..4 {
        for column in 0..5 {
            let offset_of_second = if column == 4 { second[row * 5 + 4] } else { 0.0 };
            combined[row * 5 + column] = (0..4)
                .map(|through| second[row * 5 + through] * first[through * 5 + column])
                .sum::<f32>()
                + offset_of_second;
        }
    }
    combined
}

impl WindowServerBackdropFilters {
    fn add_color_matrix(&mut self, matrix: [f32; 20]) {
        let matrix_to_add_to = if self.blur_standard_deviation.is_some() {
            &mut self.color_matrix_after_blur
        } else {
            &mut self.color_matrix_before_blur
        };
        *matrix_to_add_to = Some(match matrix_to_add_to {
            Some(earlier_matrix) => color_matrix_applying_first_then_second(earlier_matrix, &matrix),
            None => matrix,
        });
    }

    /// Adds the filter functions of a CSS filter list, which nests each function around the one before it, in the
    /// order they apply, with lengths scaled into the viewport. Answers false for a list the window server has no
    /// equivalent of: one with another function than blur() or a color function, or with a blur() after a color
    /// function that follows a blur().
    fn add_functions_of(&mut self, filter: &Filter, scale: f32) -> bool {
        match filter {
            Filter::Compose { outer, inner } => {
                self.add_functions_of(inner, scale) && self.add_functions_of(outer, scale)
            }
            Filter::Blur {
                radius_x,
                radius_y,
                mirrors_at_backdrop_edge: true,
                input: None,
            } if radius_x == radius_y && self.color_matrix_after_blur.is_none() => {
                // Two blurs in a row blur as one whose variance is the sum of theirs.
                let standard_deviation = radius_x * scale;
                self.blur_standard_deviation = Some(match self.blur_standard_deviation {
                    Some(earlier_standard_deviation) => ak::math::hypot(earlier_standard_deviation, standard_deviation),
                    None => standard_deviation,
                });
                true
            }
            Filter::ColorFilter {
                kind,
                amount,
                input: None,
            } => {
                self.add_color_matrix(color_filter_matrix(*kind, *amount));
                true
            }
            Filter::HueRotate {
                angle_degrees,
                input: None,
            } => {
                self.add_color_matrix(hue_rotate_matrix(*angle_degrees));
                true
            }
            _ => false,
        }
    }
}

// Whether the backdrop holds what is drawn outside every layer, as the canvas color is, which what the window server
// shows under the box always holds. A replay adds the canvas color under a backdrop read in the root group, and reads
// the surface itself when there is no root group, but leaves it out of a backdrop read inside any other effect.
fn backdrop_holds_what_is_drawn_outside_every_layer(tree: &VisualContextTree, effect: EffectNodeIndex) -> bool {
    let mut enclosing_effect = tree.effect_nodes[effect.0 as usize].parent;
    while !enclosing_effect.is_none()
        && matches!(
            tree.effect_nodes[enclosing_effect.0 as usize].data,
            EffectNodeData::BackgroundColorAnimation
        )
    {
        enclosing_effect = tree.effect_nodes[enclosing_effect.0 as usize].parent;
    }
    match tree.root_isolation_effect {
        Some(root) => enclosing_effect == root,
        None => enclosing_effect.is_none(),
    }
}

fn corner_radii_scaled_to_whole_device_pixels(corner_radii: CornerRadii, scale: f32) -> CornerRadii {
    let scaled = |corner: CornerRadius| CornerRadius {
        horizontal_radius: (corner.horizontal_radius as f32 * scale).round() as i32,
        vertical_radius: (corner.vertical_radius as f32 * scale).round() as i32,
    };
    CornerRadii {
        top_left: scaled(corner_radii.top_left),
        top_right: scaled(corner_radii.top_right),
        bottom_right: scaled(corner_radii.bottom_right),
        bottom_left: scaled(corner_radii.bottom_left),
    }
}

/// What the window server does to what it shows below the box of `effect`, which has a backdrop filter, or why it
/// cannot apply the filter.
fn window_server_backdrop_of_effect(
    tree: &VisualContextTree,
    world: &WorldTransforms,
    effect: EffectNodeIndex,
) -> Result<WindowServerBackdrop, WhyWindowServerCannotCompositeEffect> {
    let node = &tree.effect_nodes[effect.0 as usize];
    let EffectNodeData::Effects(effects) = &node.data else {
        return Err(WhyWindowServerCannotCompositeEffect::Masks);
    };
    let Some(backdrop) = &effects.backdrop_filter else {
        return Err(WhyWindowServerCannotCompositeEffect::BackdropFilterFunctionWithoutWindowServerEquivalent);
    };
    if backdrop_is_drawn_by_masks(tree, effect) {
        return Err(WhyWindowServerCannotCompositeEffect::BackdropDrawnByMasks);
    }
    if !backdrop_holds_what_is_drawn_outside_every_layer(tree, effect) {
        return Err(WhyWindowServerCannotCompositeEffect::BackdropReadInsideAnotherEffect);
    }
    // The box's layer composites the filtered backdrop along with its content at the box's opacity, where the window
    // server filters what is below a group only once the group is composited.
    if effects.opacity != 1.0 {
        return Err(WhyWindowServerCannotCompositeEffect::BoxWithBackdropFilterHasOpacity);
    }
    let Some(matrix) = world.draw_matrix_if_known(node.spatial) else {
        return Err(WhyWindowServerCannotCompositeEffect::Clipped(
            WhyWindowServerCannotShowMedia::ClippedInSpaceTreeDoesNotHave,
        ));
    };
    let elements = &matrix.elements;
    let (scale_x, scale_y) = (elements[0][0], elements[1][1]);
    if !matrix.is_2d_affine()
        || elements[0][1] != 0.0
        || elements[1][0] != 0.0
        || scale_x <= 0.0
        || (scale_x - scale_y).abs() > UNIFORM_SCALE_TOLERANCE * scale_x
    {
        return Err(WhyWindowServerCannotCompositeEffect::BoxWithBackdropFilterIsNotUniformlyScaled);
    }
    let mut filters = WindowServerBackdropFilters::default();
    let filter_has_window_server_equivalent =
        Filter::deserialize(&backdrop.filter).is_some_and(|filter| filters.add_functions_of(&filter, scale_x));
    if !filter_has_window_server_equivalent {
        return Err(WhyWindowServerCannotCompositeEffect::BackdropFilterFunctionWithoutWindowServerEquivalent);
    }
    Ok(WindowServerBackdrop {
        region: matrix.extract_2d_affine().map_rect(backdrop.region.to_float()),
        corner_radii: corner_radii_scaled_to_whole_device_pixels(backdrop.corner_radii, scale_x),
        filters,
    })
}

/// How the window server composites `effect` by itself around the layers of what the effect holds, or why it cannot.
/// The effect is entered within the clips from `output_clip` out to `clip_entered_outside_effect`, which is none for
/// an effect inside no other effect that is composited. A backdrop filter is the window server's to apply only where
/// `window_server_filters_what_is_below_layers`.
pub fn window_server_placement_of_effect(
    tree: &VisualContextTree,
    world: &WorldTransforms,
    effect: EffectNodeIndex,
    output_clip: ClipNodeIndex,
    clip_entered_outside_effect: ClipNodeIndex,
    window_server_filters_what_is_below_layers: bool,
) -> Result<WindowServerPlacementOfEffect, WhyWindowServerCannotCompositeEffect> {
    let EffectNodeData::Effects(effects) = &tree.effect_nodes[effect.0 as usize].data else {
        return Err(WhyWindowServerCannotCompositeEffect::Masks);
    };
    if effects.blend_mode != CompositingAndBlendingOperator::Normal {
        return Err(WhyWindowServerCannotCompositeEffect::BlendsWithWhatIsBelowIt);
    }
    if effects.filter.is_some() {
        return Err(WhyWindowServerCannotCompositeEffect::Filters);
    }
    let backdrop = match &effects.backdrop_filter {
        Some(_) if !window_server_filters_what_is_below_layers => {
            return Err(WhyWindowServerCannotCompositeEffect::WindowServerFiltersNothingBelowLayers);
        }
        Some(_) => Some(window_server_backdrop_of_effect(tree, world, effect)?),
        None => None,
    };
    let mut clips_in_viewport = ClipsInViewport::default();
    let mut clip = output_clip;
    while !clip.is_none() && clip != clip_entered_outside_effect {
        let node = &tree.clip_nodes[clip.0 as usize];
        match &node.data {
            ClipNodeData::Rect(data) => clips_in_viewport
                .add(world, node.spatial, data.rect, data.corner_radii, data.mode)
                .map_err(WhyWindowServerCannotCompositeEffect::Clipped)?,
            ClipNodeData::Path(_) => {
                return Err(WhyWindowServerCannotCompositeEffect::Clipped(
                    WhyWindowServerCannotShowMedia::ClippedByPath,
                ));
            }
            ClipNodeData::Dead => {}
        }
        clip = node.parent;
    }
    let mut clips = Vec::new();
    if let Some(intersection_of_rect_clips) = clips_in_viewport
        .rect_clips
        .iter()
        .copied()
        .reduce(|intersection, rect| intersection.intersected(rect))
    {
        clips.push((intersection_of_rect_clips, CornerRadii::default()));
    }
    let whole_device_pixels = |(horizontal, vertical): (f32, f32)| CornerRadius {
        horizontal_radius: horizontal.round() as i32,
        vertical_radius: vertical.round() as i32,
    };
    clips.extend(clips_in_viewport.rounded_clips.iter().map(|rounded_clip| {
        let [top_left, top_right, bottom_right, bottom_left] =
            rounded_clip.horizontal_and_vertical_radii_clockwise_from_top_left;
        (
            rounded_clip.rect,
            CornerRadii {
                top_left: whole_device_pixels(top_left),
                top_right: whole_device_pixels(top_right),
                bottom_right: whole_device_pixels(bottom_right),
                bottom_left: whole_device_pixels(bottom_left),
            },
        )
    }));
    Ok(WindowServerPlacementOfEffect {
        clips,
        opacity: effects.opacity,
        backdrop,
    })
}

/// Where the window server can show the canvas or video that `header` and `payload` draw in `run`, within
/// `viewport_rect`, or why it cannot.
pub fn window_server_placement(
    tree: &VisualContextTree,
    world: &WorldTransforms,
    viewport_rect: IntRect,
    run: &DisplayListCommandRun,
    header: &DisplayListCommandHeader,
    payload: &[u8],
) -> Result<WindowServerPlacement, WhyWindowServerCannotShowMedia> {
    let (content_rect, scaling_mode) = match header.command_type {
        DisplayListCommandType::DrawCanvas => {
            let command = read_command::<DrawCanvas>(payload);
            (command.dst_rect, command.scaling_mode)
        }
        DisplayListCommandType::DrawVideoFrame => {
            let command = read_command::<DrawVideoFrame>(payload);
            (command.dst_rect, command.scaling_mode)
        }
        _ => unreachable!("the window server shows only canvases and videos"),
    };
    let mut content_to_viewport = world.draw_matrix(run.context.spatial).extract_2d_affine();
    let mut content_bounds_in_record_space = content_rect.to_float();
    if let Some(inline_transform) = inline_transform_of(header, payload) {
        content_to_viewport = multiply_affine(content_to_viewport, inline_transform);
        content_bounds_in_record_space = inline_transform.map_rect(content_bounds_in_record_space);
    }

    let mut clips = ClipsInViewport::default();
    for clip in inline_clips_of(header, payload) {
        if clip.kind == InlineClipKind::Path {
            return Err(WhyWindowServerCannotShowMedia::ClippedByPath);
        }
        let clip_space = if clip.has_own_space {
            clip.space
        } else {
            run.context.spatial
        };
        if clip_space == run.context.spatial
            && clip_leaves_content_whole(
                clip.clip_rect_or_path_device_bounds,
                clip.corner_radii,
                clip.mode,
                content_bounds_in_record_space,
            )
        {
            continue;
        }
        clips.add(
            world,
            clip_space,
            clip.clip_rect_or_path_device_bounds,
            clip.corner_radii,
            clip.mode,
        )?;
    }
    let mut clip = run.context.clip;
    while !clip.is_none() {
        let node = &tree.clip_nodes[clip.0 as usize];
        match &node.data {
            ClipNodeData::Rect(data)
                if node.spatial == run.context.spatial
                    && clip_leaves_content_whole(
                        data.rect,
                        data.corner_radii,
                        data.mode,
                        content_bounds_in_record_space,
                    ) => {}
            ClipNodeData::Rect(data) => clips.add(world, node.spatial, data.rect, data.corner_radii, data.mode)?,
            ClipNodeData::Path(data) => clips.add_path(world, node.spatial, data)?,
            ClipNodeData::Dead => {}
        }
        clip = node.parent;
    }

    let mut clip_rect = viewport_rect.to_float();
    for rect in clips
        .rect_clips
        .iter()
        .chain(clips.rounded_clips.iter().map(|rounded_clip| &rounded_clip.rect))
    {
        clip_rect = clip_rect.intersected(*rect);
    }
    let visible_content = content_to_viewport
        .map_rect(content_rect.to_float())
        .intersected(clip_rect);
    let mut rounded_clip = None;
    for rounded_clip_in_viewport in &clips.rounded_clips {
        if !rounded_clip_in_viewport.rounded_corner_reaches_into(visible_content) {
            continue;
        }
        if rounded_clip.is_some() {
            return Err(WhyWindowServerCannotShowMedia::MoreThanOneRoundedClip);
        }
        rounded_clip = Some(rounded_clip_in_viewport.with_single_radius()?);
    }

    Ok(WindowServerPlacement {
        content_rect,
        content_to_viewport,
        clip_rect,
        rounded_clip,
        path_clip: clips.path_clip,
        scaling_mode,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::builder::{DisplayListBuilder, PendingInlineClip};
    use crate::display_list::storage::DisplayListStorage;
    use crate::display_list::test_fixtures::{VIEWPORT, canvas, tree_with_root_isolation_effect};
    use libgfx_rust::FloatMatrix4x4;

    // A list from another process can give an inline clip any space.
    #[test]
    fn a_canvas_clipped_in_a_space_the_tree_does_not_have_is_not_shown_by_the_window_server() {
        let (tree, root) = tree_with_root_isolation_effect(1.0);
        let mut builder = DisplayListBuilder::new();
        builder.append_with_inline_clips(
            &canvas(IntRect::new(10, 10, 100, 100), 1),
            &[],
            root,
            &[
                PendingInlineClip::intersecting_float_rect(FloatRect::new(0.0, 0.0, 50.0, 50.0))
                    .in_space(SpatialNodeIndex(0xFFFF_FFF0)),
            ],
        );
        let list = DisplayListStorage::of_builder(&builder);
        let canvas_position = list.whole_span().begin;
        let run = &list.chunk(canvas_position.chunk_index).runs()[0];
        let (header, payload) = list.record_at(canvas_position);
        let world = WorldTransforms::new(&tree, &[], Some(FloatMatrix4x4::identity()));
        assert_eq!(
            window_server_placement(&tree, &world, VIEWPORT, run, &header, payload),
            Err(WhyWindowServerCannotShowMedia::ClippedInSpaceTreeDoesNotHave)
        );
    }
}
