/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::host::FfiFormControlPaintFacts;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ReplacedPaintFacts {
    FormControl(FfiFormControlPaintFacts),
}

impl ReplacedPaintFacts {
    pub(crate) fn form_control(self) -> FfiFormControlPaintFacts {
        match self {
            Self::FormControl(facts) => facts,
        }
    }
}
