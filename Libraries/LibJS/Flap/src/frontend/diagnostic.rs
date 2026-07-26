/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Source locations and compiler diagnostics.

use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SourceLocation {
    pub offset: usize,
    pub line: usize,
    pub column: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SourceSpan {
    pub start: SourceLocation,
    pub end: SourceLocation,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Diagnostic {
    pub(crate) filename: String,
    pub(crate) span: SourceSpan,
    pub(crate) message: String,
}

impl Diagnostic {
    pub(crate) fn new(filename: &str, span: SourceSpan, message: impl Into<String>) -> Self {
        Self {
            filename: filename.to_string(),
            span,
            message: message.into(),
        }
    }
}

impl fmt::Display for Diagnostic {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "{}:{}:{}: error: {}",
            self.filename, self.span.start.line, self.span.start.column, self.message
        )
    }
}
