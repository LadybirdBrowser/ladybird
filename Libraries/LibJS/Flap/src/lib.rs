/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Flap compiler library.
//!
//! Flap compiles typed interpreter handler definitions through semantic
//! analysis, SSA construction and optimization, machine lowering, register
//! allocation, and target-specific assembly emission. The compiler operates
//! entirely on in-memory inputs and outputs; the `flapc` binary is only a file
//! system and command-line adapter.

pub(crate) mod bytecode;
pub(crate) mod frontend;
pub(crate) mod hir;
pub(crate) mod identity;
pub(crate) mod intrinsic;
pub(crate) mod ssa;
pub(crate) mod types;

use frontend::diagnostic::{Diagnostic, SourceLocation};
use frontend::layout::LayoutError;
use std::error::Error;
use std::fmt;

pub use frontend::diagnostic::SourceSpan;

/// A diagnostic produced by one of the compiler stages.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompileStage {
    Layout,
    Parse,
    Semantic,
    Ssa,
    LowIr,
    Selection,
    Allocation,
    Verification,
    Finalization,
    Emission,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompileError {
    pub stage: CompileStage,
    pub handler: Option<String>,
    pub span: Option<SourceSpan>,
    pub message: String,
}

impl CompileError {
    pub(crate) fn new(
        stage: CompileStage,
        handler: Option<&str>,
        message: impl Into<String>,
    ) -> Self {
        Self {
            stage,
            handler: handler.map(str::to_string),
            span: None,
            message: message.into(),
        }
    }

    fn from_diagnostic(stage: CompileStage, diagnostic: Diagnostic) -> Self {
        Self {
            stage,
            handler: None,
            span: Some(diagnostic.span),
            message: diagnostic.to_string(),
        }
    }

    fn from_layout_error(error: LayoutError) -> Self {
        let location = SourceLocation {
            offset: 0,
            line: error.line,
            column: 1,
        };
        Self {
            stage: CompileStage::Layout,
            handler: None,
            span: Some(SourceSpan {
                start: location,
                end: location,
            }),
            message: error.to_string(),
        }
    }
}

impl fmt::Display for CompileError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{:?}", self.stage)?;
        if let Some(handler) = &self.handler {
            write!(formatter, " error in handler '{handler}'")?;
        } else {
            formatter.write_str(" error")?;
        }
        write!(formatter, ": {}", self.message)
    }
}

impl Error for CompileError {}
