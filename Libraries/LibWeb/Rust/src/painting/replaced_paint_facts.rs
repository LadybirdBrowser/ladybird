/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::used_values::OptionalCssPixels;
use crate::painting::host::{
    FfiCanvasPaintFacts, FfiFormControlPaintFacts, FfiNavigableContainerPaintFacts, FfiReplacedImagePaintFacts,
    FfiVideoPaintFacts, FfiVideoRepresentation,
};
use crate::painting::image_content::ImageContent;
use libgfx_rust::image_frame::ImageFrameHandle;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct VideoFrameFacts {
    pub src_width: i32,
    pub src_height: i32,
    pub sink_resource_id: u64,
    pub sink_handle: u64,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum VideoPaintFacts {
    VideoFrame(Option<VideoFrameFacts>),
    PosterFrame(Option<ImageFrameHandle>),
    TransparentBlack,
}

impl Default for VideoPaintFacts {
    fn default() -> Self {
        Self::VideoFrame(None)
    }
}

impl VideoPaintFacts {
    /// # Safety
    ///
    /// `facts.poster_frame` must be null or point to a live `Gfx::DecodedImageFrame`.
    pub(crate) unsafe fn from_ffi(facts: &FfiVideoPaintFacts) -> Self {
        match facts.representation {
            FfiVideoRepresentation::VideoFrame => Self::VideoFrame(facts.has_video_frame.then_some(VideoFrameFacts {
                src_width: facts.video_src_width,
                src_height: facts.video_src_height,
                sink_resource_id: facts.video_sink_resource_id,
                sink_handle: facts.video_sink_handle,
            })),
            FfiVideoRepresentation::PosterFrame => Self::PosterFrame(
                (!facts.poster_frame.is_null()).then(|| unsafe { ImageFrameHandle::retain(facts.poster_frame) }),
            ),
            FfiVideoRepresentation::TransparentBlack => Self::TransparentBlack,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct ImagePaintFacts {
    pub has_decoded_image_data: bool,
    pub natural_width: OptionalCssPixels,
    pub natural_height: OptionalCssPixels,
    pub natural_aspect_ratio: Option<(CssPixels, CssPixels)>,
    pub content: ImageContent,
}

impl ImagePaintFacts {
    /// # Safety
    ///
    /// `facts.frame` must be null or point to a live `Gfx::DecodedImageFrame`.
    pub(crate) unsafe fn from_ffi(facts: &FfiReplacedImagePaintFacts) -> Self {
        Self {
            has_decoded_image_data: facts.has_decoded_image_data,
            natural_width: facts.natural_width,
            natural_height: facts.natural_height,
            natural_aspect_ratio: facts.has_natural_aspect_ratio.then_some((
                facts.natural_aspect_ratio_numerator,
                facts.natural_aspect_ratio_denominator,
            )),
            content: unsafe {
                ImageContent::from_ffi(
                    facts.content_kind,
                    facts.frame,
                    facts.vector_content_identity,
                    facts.vector_has_active_view_box,
                )
            },
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum ReplacedPaintFacts {
    FormControl(FfiFormControlPaintFacts),
    Canvas(FfiCanvasPaintFacts),
    NavigableContainer(FfiNavigableContainerPaintFacts),
    Image(ImagePaintFacts),
    Video(VideoPaintFacts),
}

impl ReplacedPaintFacts {
    pub(crate) fn image(self) -> Option<ImagePaintFacts> {
        match self {
            Self::Image(facts) => Some(facts),
            _ => None,
        }
    }

    pub(crate) fn video(self) -> Option<VideoPaintFacts> {
        match self {
            Self::Video(facts) => Some(facts),
            _ => None,
        }
    }

    pub(crate) fn form_control(self) -> Option<FfiFormControlPaintFacts> {
        match self {
            Self::FormControl(facts) => Some(facts),
            _ => None,
        }
    }

    pub(crate) fn canvas(self) -> Option<FfiCanvasPaintFacts> {
        match self {
            Self::Canvas(facts) => Some(facts),
            _ => None,
        }
    }

    pub(crate) fn navigable_container(self) -> Option<FfiNavigableContainerPaintFacts> {
        match self {
            Self::NavigableContainer(facts) => Some(facts),
            _ => None,
        }
    }
}
