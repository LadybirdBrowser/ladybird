/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::used_values::OptionalCssPixels;
use crate::painting::host::{
    FfiCanvasPaintFacts, FfiFormControlPaintFacts, FfiNavigableContainerPaintFacts, FfiReplacedImagePaintFacts,
};
use crate::painting::image_content::ImageContent;

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
            content: unsafe { ImageContent::from_ffi(facts.content_kind, facts.frame, facts.vector_content_identity) },
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum ReplacedPaintFacts {
    FormControl(FfiFormControlPaintFacts),
    Canvas(FfiCanvasPaintFacts),
    NavigableContainer(FfiNavigableContainerPaintFacts),
    Image(ImagePaintFacts),
}

impl ReplacedPaintFacts {
    pub(crate) fn image(self) -> Option<ImagePaintFacts> {
        match self {
            Self::Image(facts) => Some(facts),
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
