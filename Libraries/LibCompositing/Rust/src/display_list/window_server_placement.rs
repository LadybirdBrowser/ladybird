/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where the window server can show a canvas or a video that a layer plan draws by itself. The window server places
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
pub enum WindowServerRejection {
    ClippedByPath,
    ClippedByExcludingClip,
    ClippedInRotatedOrSkewedSpace,
    RoundedCornersOfDifferingRadii,
    MoreThanOneRoundedClip,
}

impl std::fmt::Display for WindowServerRejection {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            WindowServerRejection::ClippedByPath => "clipped by a path",
            WindowServerRejection::ClippedByExcludingClip => "clipped by an excluding clip",
            WindowServerRejection::ClippedInRotatedOrSkewedSpace => "clipped in a rotated or skewed space",
            WindowServerRejection::RoundedCornersOfDifferingRadii => "rounded corners of differing radii",
            WindowServerRejection::MoreThanOneRoundedClip => "more than one rounded clip",
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

// A rounded clip in viewport pixels, with the radii of its corners in the order of `CornerRadii`.
struct RoundedClipInViewport {
    rect: FloatRect,
    radii: [(f32, f32); 4],
}

impl RoundedClipInViewport {
    fn corner_boxes(&self) -> [FloatRect; 4] {
        let rect = self.rect;
        let [top_left, top_right, bottom_right, bottom_left] = self.radii;
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

    fn with_single_radius(&self) -> Result<RoundedClip, WindowServerRejection> {
        let mut radius = None;
        let mut rounded = [false; 4];
        for (index, &(horizontal, vertical)) in self.radii.iter().enumerate() {
            if horizontal <= 0.0 || vertical <= 0.0 {
                continue;
            }
            let corner_radius = *radius.get_or_insert(horizontal);
            if (horizontal - corner_radius).abs() > RADIUS_TOLERANCE_IN_DEVICE_PIXELS
                || (vertical - corner_radius).abs() > RADIUS_TOLERANCE_IN_DEVICE_PIXELS
            {
                return Err(WindowServerRejection::RoundedCornersOfDifferingRadii);
            }
            rounded[index] = true;
        }
        Ok(RoundedClip {
            rect: self.rect,
            radius: radius.unwrap_or(0.0),
            rounded_corners: RoundedCorners {
                top_left: rounded[0],
                top_right: rounded[1],
                bottom_right: rounded[2],
                bottom_left: rounded[3],
            },
        })
    }
}

// Whether a clip in the space of the content leaves all of it visible, as the clip of a video to its box does when
// the video fills the box. Such a clip needs no layer of its own, even where the space is rotated.
fn clip_leaves_content_whole(
    rect: FloatRect,
    corner_radii: CornerRadii,
    mode: ClipMode,
    content_rect: FloatRect,
) -> bool {
    if mode != ClipMode::Intersect || !rect.contains_rect(content_rect) {
        return false;
    }
    let corner_box = |x: f32, y: f32, corner: CornerRadius| {
        if !corner.is_present() {
            return FloatRect::default();
        }
        FloatRect::new(x, y, corner.horizontal_radius as f32, corner.vertical_radius as f32)
    };
    let boxes = [
        corner_box(rect.x, rect.y, corner_radii.top_left),
        corner_box(
            rect.right() - corner_radii.top_right.horizontal_radius as f32,
            rect.y,
            corner_radii.top_right,
        ),
        corner_box(
            rect.right() - corner_radii.bottom_right.horizontal_radius as f32,
            rect.bottom() - corner_radii.bottom_right.vertical_radius as f32,
            corner_radii.bottom_right,
        ),
        corner_box(
            rect.x,
            rect.bottom() - corner_radii.bottom_left.vertical_radius as f32,
            corner_radii.bottom_left,
        ),
    ];
    boxes
        .iter()
        .all(|corner_box| corner_box.is_empty() || corner_box.intersected(content_rect).is_empty())
}

#[derive(Default)]
struct ClipsInViewport {
    rect_clips: Vec<FloatRect>,
    rounded_clips: Vec<RoundedClipInViewport>,
}

impl ClipsInViewport {
    fn add(
        &mut self,
        world: &WorldTransforms,
        space: SpatialNodeIndex,
        rect: FloatRect,
        corner_radii: CornerRadii,
        mode: ClipMode,
    ) -> Result<(), WindowServerRejection> {
        if mode == ClipMode::Difference {
            return Err(WindowServerRejection::ClippedByExcludingClip);
        }
        let matrix = world.draw_matrix(space);
        let elements = &matrix.elements;
        if !matrix.is_2d_affine() || elements[0][1] != 0.0 || elements[1][0] != 0.0 {
            return Err(WindowServerRejection::ClippedInRotatedOrSkewedSpace);
        }
        let viewport_rect = matrix.extract_2d_affine().map_rect(rect);
        if !corner_radii.has_any_radius() {
            self.rect_clips.push(viewport_rect);
            return Ok(());
        }
        let (scale_x, scale_y) = (elements[0][0], elements[1][1]);
        let scaled = |corner: CornerRadius| {
            if !corner.is_present() {
                return (0.0, 0.0);
            }
            (
                corner.horizontal_radius as f32 * scale_x.abs(),
                corner.vertical_radius as f32 * scale_y.abs(),
            )
        };
        let mut radii = [
            scaled(corner_radii.top_left),
            scaled(corner_radii.top_right),
            scaled(corner_radii.bottom_right),
            scaled(corner_radii.bottom_left),
        ];
        // A mirrored space swaps the corners that each radius lands on.
        if scale_x < 0.0 {
            radii = [radii[1], radii[0], radii[3], radii[2]];
        }
        if scale_y < 0.0 {
            radii = [radii[3], radii[2], radii[1], radii[0]];
        }
        self.rounded_clips.push(RoundedClipInViewport {
            rect: viewport_rect,
            radii,
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
) -> Result<WindowServerPlacement, WindowServerRejection> {
    let (content_rect, scaling_mode) = match header.command_type {
        DisplayListCommandType::DrawCanvas => {
            let command = read_command::<DrawCanvas>(payload);
            (command.dst_rect, command.scaling_mode)
        }
        DisplayListCommandType::DrawVideoFrame => {
            let command = read_command::<DrawVideoFrame>(payload);
            (command.dst_rect, command.scaling_mode)
        }
        _ => unreachable!("only canvases and videos are drawn by media layers"),
    };
    let mut content_to_viewport = world.draw_matrix(run.context.spatial).extract_2d_affine();
    // Clips in the space of the record apply before its inline transform moves the content.
    let mut content_bounds_in_record_space = content_rect.to_float();
    if let Some(inline_transform) = inline_transform_of(header, payload) {
        content_to_viewport = multiply_affine(content_to_viewport, inline_transform);
        content_bounds_in_record_space = inline_transform.map_rect(content_bounds_in_record_space);
    }

    let mut clips = ClipsInViewport::default();
    for clip in inline_clips_of(header, payload) {
        if clip.kind == InlineClipKind::Path {
            return Err(WindowServerRejection::ClippedByPath);
        }
        let space = if clip.has_own_space {
            clip.space
        } else {
            run.context.spatial
        };
        if space == run.context.spatial
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
            space,
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
            ClipNodeData::Path(_) => return Err(WindowServerRejection::ClippedByPath),
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
    // A rounded clip whose corners all miss the visible part of the content clips it as its rectangle does.
    let visible_content = content_to_viewport
        .map_rect(content_rect.to_float())
        .intersected(clip_rect);
    let mut rounded_clip = None;
    for rounded_clip_in_viewport in &clips.rounded_clips {
        let corners_cut_the_content = rounded_clip_in_viewport
            .corner_boxes()
            .iter()
            .any(|corner_box| !corner_box.is_empty() && !corner_box.intersected(visible_content).is_empty());
        if !corners_cut_the_content {
            continue;
        }
        if rounded_clip.is_some() {
            return Err(WindowServerRejection::MoreThanOneRoundedClip);
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
