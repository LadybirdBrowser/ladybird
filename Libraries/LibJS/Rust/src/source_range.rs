/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/SourceRange.h and Libraries/LibJS/Position.h: where in its source code a piece of bytecode came
//! from.

use std::rc::Rc;

use ak::Utf16String;

use crate::source_code::SourceCode;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
pub struct Position {
    pub line: u32,
    pub column: u32,
}

#[derive(Clone)]
pub struct SourceRange {
    pub code: Rc<SourceCode>,
    pub start: Position,
}

impl SourceRange {
    pub fn filename(&self) -> &Utf16String {
        self.code.filename()
    }
}
