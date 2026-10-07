/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Breakpoint.h: a place in the source code where the debugger pauses.

use std::rc::Rc;

use ak::Utf16String;

use crate::source_code::SourceCode;

pub type BreakpointID = u32;

#[derive(Clone)]
pub struct Breakpoint {
    pub id: BreakpointID,
    /// The source code the breakpoint is in, or None if it is in any source code with its filename.
    pub source_code: Option<Rc<SourceCode>>,
    pub filename: Utf16String,
    pub line: u32,
    pub column: Option<u32>,
}
