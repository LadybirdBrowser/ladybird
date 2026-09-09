/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::host::{FfiCanvasPaintFacts, FfiFormControlPaintFacts};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ReplacedPaintFacts {
    FormControl(FfiFormControlPaintFacts),
    Canvas(FfiCanvasPaintFacts),
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
}
