/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::host::{
    FfiCanvasPaintFacts, FfiFormControlPaintFacts, FfiNavigableContainerPaintFacts, FfiReplacedImagePaintFacts,
    FfiVideoPaintFacts, FfiVideoRepresentation,
};
use crate::painting::image_content::ImageContent;
use crate::painting::record::damage::PaintDamage;
use crate::painting::record::paint::replaced::SizeWithAspectRatio;
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
    pub natural: SizeWithAspectRatio,
    pub content: ImageContent,
}

impl ImagePaintFacts {
    /// # Safety
    ///
    /// `facts.content.frame` must be null or point to a live `Gfx::DecodedImageFrame`.
    pub(crate) unsafe fn from_ffi(facts: &FfiReplacedImagePaintFacts) -> Self {
        Self {
            natural: SizeWithAspectRatio::from_ffi(&facts.natural),
            content: unsafe { ImageContent::from_ffi(&facts.content) },
        }
    }
}

/// Each replaced row's paint facts. A publication shares the table, so a write copies it only while
/// a publication still holds it.
pub(crate) type ReplacedPaintFactsTable =
    crate::css::style::fast_hash::FastMap<crate::layout::node_data::NodeSlotId, ReplacedPaintFacts>;

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum ReplacedPaintFacts {
    FormControl(FfiFormControlPaintFacts),
    Canvas(FfiCanvasPaintFacts),
    NavigableContainer(FfiNavigableContainerPaintFacts),
    Image(ImagePaintFacts),
    Video(VideoPaintFacts),
}

impl ReplacedPaintFacts {
    /// The damage a row takes where its facts change to these. A canvas or a navigable container caches what it paints
    /// and is hit-tested with beside its row, which goes with the facts.
    pub(crate) fn damage_when_changed(&self) -> PaintDamage {
        match self {
            Self::FormControl(_) => PaintDamage::DRAW_FOREGROUND,
            Self::Canvas(_) | Self::NavigableContainer(_) => PaintDamage::ALL_DRAW | PaintDamage::ALL_HIT,
            Self::Image(_) | Self::Video(_) => PaintDamage::ALL_DRAW,
        }
    }

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
