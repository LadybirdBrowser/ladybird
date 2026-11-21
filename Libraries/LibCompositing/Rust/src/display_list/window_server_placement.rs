/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where the window server can show by itself a canvas or a video that is a layer of its own. The window server places
//! an image with a 2D affine transform and clips it to rectangles, one of which can have rounded corners of a single
//! radius. A media layer drawn under any other clip has to be replayed instead.

use crate::display_list::builder::{inline_clips_of, inline_transform_of, read_command};
use crate::display_list::commands::{ClipMode, DisplayListCommandType, DrawCanvas, DrawVideoFrame, SpatialNodeIndex};
use crate::display_list::tape::{DisplayListCommandHeader, DisplayListCommandRun, InlineClipKind};
use crate::visual_context::{ClipNodeData, VisualContextTree, WorldTransforms};
use libgfx_rust::{AffineTransform, CornerRadii, CornerRadius, FloatRect, IntRect, ScalingMode, multiply_affine};

const RADIUS_TOLERANCE_IN_DEVICE_PIXELS: f32 = 0.01;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WindowServerPlacement {
    /// Where the record draws its content, in the record's own space.
    pub content_rect: IntRect,
    pub content_to_viewport: AffineTransform,
    /// The intersection of every clip the record is drawn under, rounded or not, in viewport pixels.
    pub clip_rect: FloatRect,
    pub rounded_clip: Option<RoundedClip>,
    pub scaling_mode: ScalingMode,
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
        let Some(rounded_clip) = self.rounded_clip else {
            return Ok(());
        };
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

#[derive(Default)]
struct ClipsInViewport {
    rect_clips: Vec<FloatRect>,
    rounded_clips: Vec<RoundedRect>,
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
        let matrix = world
            .draw_matrix_if_known(space)
            .ok_or(WhyWindowServerCannotShowMedia::ClippedInSpaceTreeDoesNotHave)?;
        let elements = &matrix.elements;
        if !matrix.is_2d_affine() || elements[0][1] != 0.0 || elements[1][0] != 0.0 {
            return Err(WhyWindowServerCannotShowMedia::ClippedInRotatedOrSkewedSpace);
        }
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
            ClipNodeData::Path(_) => return Err(WhyWindowServerCannotShowMedia::ClippedByPath),
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
