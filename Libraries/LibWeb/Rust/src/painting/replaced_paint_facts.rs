/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::host::{FfiCanvasPaintFacts, FfiFormControlPaintFacts, FfiNavigableContainerPaintFacts};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ReplacedPaintFacts {
    FormControl(FfiFormControlPaintFacts),
    Canvas(FfiCanvasPaintFacts),
    NavigableContainer(FfiNavigableContainerPaintFacts),
}

impl ReplacedPaintFacts {
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
